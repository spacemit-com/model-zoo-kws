/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KwsEngine Implementation
 *
 * SpacemiT::KwsEngine 的实现，封装内部后端。
 */

#include "kws_service.h"

#include <memory>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include "spsc_queue.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "backends/kws_backend.hpp"

namespace SpacemiT {

// =============================================================================
// Type Conversion Helpers
// =============================================================================

namespace {

kws::BackendType toInternalBackendType(KwsBackendType type) {
    switch (type) {
        case KwsBackendType::CFSMN:  return kws::BackendType::CFSMN;
        case KwsBackendType::CUSTOM: return kws::BackendType::CUSTOM;
        default:                     return kws::BackendType::CUSTOM;
    }
}

kws::KwsConfig toInternalConfig(const KwsConfig& config) {
    kws::KwsConfig internal;
    internal.backend = toInternalBackendType(config.backend);
    internal.model_dir = config.model_dir;
    for (const KwsKeyword& kw : config.keywords) {
        internal.keywords.push_back(kws::Keyword{kw.text, kw.token_ids, kw.threshold});
    }
    internal.sample_rate = config.sample_rate;
    internal.num_channels = config.num_channels;
    internal.frame_size = config.frame_size;
    internal.use_beamforming = config.use_beamforming;
    internal.beam_first_channel = config.beam_first_channel;
    internal.threshold = config.threshold;
    internal.holdoff_ms = config.holdoff_ms;
    internal.decode_context = config.decode_context;
    internal.score_interval = config.score_interval;
    internal.num_threads = config.num_threads;
    internal.partial_threshold = config.partial_threshold;
    internal.partial_wait_ms = config.partial_wait_ms;
    return internal;
}

}  // anonymous namespace

// =============================================================================
// KwsResult Implementation (Pimpl)
// =============================================================================

struct KwsResult::Impl {
    float score = 0.0f;
    bool is_wake_word = false;
    int keyword_index = -1;
    std::string keyword;
    int64_t timestamp_ms = 0;
    int processing_time_ms = 0;
    bool success = true;
    std::string code;
    std::string message;
};

KwsResult::KwsResult() : impl_(std::make_unique<Impl>()) {}
KwsResult::~KwsResult() = default;
KwsResult::KwsResult(KwsResult&&) noexcept = default;
KwsResult& KwsResult::operator=(KwsResult&&) noexcept = default;

float KwsResult::GetScore() const { return impl_->score; }
bool KwsResult::IsWakeWord() const { return impl_->is_wake_word; }
std::string KwsResult::GetKeyword() const { return impl_->keyword; }
int KwsResult::GetKeywordIndex() const { return impl_->keyword_index; }
int64_t KwsResult::GetTimestampMs() const { return impl_->timestamp_ms; }
int KwsResult::GetProcessingTimeMs() const { return impl_->processing_time_ms; }
bool KwsResult::IsSuccess() const { return impl_->success; }
std::string KwsResult::GetCode() const { return impl_->code; }
std::string KwsResult::GetMessage() const { return impl_->message; }

// Results are converted on the inference worker and delivered by the event thread.
class CallbackAdapter {
public:
    static std::shared_ptr<KwsResult> convert(const kws::DetectionResult& result) {
        auto out = std::make_shared<KwsResult>();
        out->impl_->score = result.score;
        out->impl_->is_wake_word = result.is_wake_word;
        out->impl_->keyword_index = result.keyword_index;
        out->impl_->keyword = result.keyword;
        out->impl_->timestamp_ms = result.timestamp_ms;
        out->impl_->processing_time_ms = result.processing_time_ms;
        return out;
    }
};

struct KwsEngine::Impl {
    static constexpr size_t kBlockSamples = 160, kAudioBlocks = 128, kEventCapacity = 128;
    static constexpr unsigned kAccepting = 1, kSubmitting = 2;
    static_assert(std::atomic<unsigned>::is_always_lock_free &&
            std::atomic<uint64_t>::is_always_lock_free &&
            std::atomic<KwsAudioStatus>::is_always_lock_free, "Audio admission must be lock-free");
    struct AudioFrame {
        std::vector<float> data;
        size_t samples = 0;
        uint64_t offset = 0;
    };
    enum class Kind { OPEN, RESULT, COMPLETE, ERROR, CLOSE };
    struct Event {
        Kind kind;
        std::shared_ptr<KwsEngineCallback> callback;
        std::shared_ptr<KwsResult> result;
        std::string message;
        uint64_t generation, serial;
    };

    KwsConfig config;
    std::unique_ptr<kws::IKwsBackend> backend;
    bool initialized = false;
    int channels = 1; // Immutable after setup; the audio endpoint never reads mutable config.
    std::mutex mutex, control_mutex, event_mutex;
    std::condition_variable event_ready, event_done;
    std::shared_ptr<KwsEngineCallback> user_callback;
    // Retain registrations until teardown so evicting an event cannot execute an
    // application callback destructor on the inference worker.
    std::vector<std::shared_ptr<KwsEngineCallback>> callback_owners;
    std::deque<Event> events;
    std::thread worker, dispatcher;
    kws::SpscQueue<AudioFrame> audio{kAudioBlocks};
    std::atomic<unsigned> admission{0}, input_errors{0};
    std::atomic<bool> streaming{false}, stopping{false}, shutdown{false}, event_shutdown{false};
    std::atomic<uint64_t> generation{0}, close_serial{0};
    uint64_t event_serial = 0, completed_serial = 0; // event_mutex
    uint64_t input_offset = 0; // Producer owns it; control accesses only with admission closed.
    uint64_t expected_offset = 0, segment_offset = 0; // mutex
    std::atomic<uint64_t> accepted{0}, dropped{0}, overruns{0}, event_overruns{0};
    std::atomic<KwsAudioStatus> input_status{KwsAudioStatus::ACCEPTED};
    bool failed = false;
    float last_score = 0.0f;
    kws::ErrorInfo last_error = kws::ErrorInfo::ok();

    static std::string errorText(const kws::ErrorInfo& error) {
        return error.detail.empty() ? error.message : error.message + ": " + error.detail;
    }
    static const char* statusText(KwsAudioStatus status) {
        switch (status) {
            case KwsAudioStatus::INVALID_PARAMETER: return "Invalid audio shape or chunk exceeds 20480 samples/channel";
            case KwsAudioStatus::QUEUE_FULL: return "Audio queue full; chunk dropped, stream continues";
            case KwsAudioStatus::BUSY: return "Concurrent audio producer; chunk rejected";
            case KwsAudioStatus::NOT_STARTED: return "Stream not started";
            default: return "";
        }
    }
    void setup() {
        backend = kws::KwsBackendFactory::create(toInternalBackendType(config.backend));
        if (!backend) {
            last_error = kws::ErrorInfo::error(kws::ErrorCode::INVALID_CONFIG, "Backend not available");
            return;
        }
        last_error = backend->initialize(toInternalConfig(config));
        if (!last_error.isOk()) return;
        channels = config.num_channels;
        for (auto& slot : audio.storage()) slot.data.resize(kBlockSamples * channels);
        initialized = true;
        try {
            dispatcher = std::thread([this] { dispatchLoop(); });
            worker = std::thread([this] { workLoop(); });
        } catch (...) {
            stopEvents();
            if (dispatcher.joinable()) dispatcher.join();
            throw;
        }
    }
    ~Impl() {
        admission.fetch_and(~kAccepting, std::memory_order_acq_rel);
        shutdown = true;
        if (worker.joinable()) worker.join();
        streaming = false;
        stopEvents();
        if (dispatcher.joinable()) dispatcher.join();
        if (backend) { backend->setCallback(nullptr); backend->shutdown(); }
    }
    // Set under event_mutex: the dispatcher checks the flag and then sleeps
    // atomically under it, so an unlocked store could slip in between.
    void stopEvents() {
        {
            std::lock_guard<std::mutex> lock(event_mutex);
            event_shutdown = true;
        }
        event_ready.notify_all();
        event_done.notify_all();
    }
    void closeAdmission() {
        admission.fetch_and(~kAccepting, std::memory_order_acq_rel);
        while (admission.load(std::memory_order_acquire) & kSubmitting)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    KwsAudioStatus reject(KwsAudioStatus status) noexcept {
        input_status.store(status, std::memory_order_relaxed);
        if (status == KwsAudioStatus::INVALID_PARAMETER) input_errors.fetch_or(1);
        if (status == KwsAudioStatus::BUSY) input_errors.fetch_or(2);
        return status;
    }
    KwsAudioStatus submit(const float* data, size_t samples) noexcept {
        if (!samples) return KwsAudioStatus::EMPTY;
        if (!data || samples > kBlockSamples * kAudioBlocks)
            return reject(KwsAudioStatus::INVALID_PARAMETER);
        unsigned expected = kAccepting;
        if (!admission.compare_exchange_strong(expected, kAccepting | kSubmitting,
                std::memory_order_acquire))
            return reject(expected & kAccepting ? KwsAudioStatus::BUSY : KwsAudioStatus::NOT_STARTED);
        const size_t blocks = (samples + kBlockSamples - 1) / kBlockSamples;
        KwsAudioStatus status = KwsAudioStatus::ACCEPTED;
        if (audio.freeSlots() < blocks) {
            dropped.fetch_add(samples, std::memory_order_relaxed);
            overruns.fetch_add(1, std::memory_order_relaxed);
            input_errors.fetch_or(4, std::memory_order_relaxed);
            status = KwsAudioStatus::QUEUE_FULL;
        } else {
            for (size_t i = 0, offset = 0; i < blocks; ++i) {
                auto& slot = audio.writeSlot(i);
                slot.samples = std::min(kBlockSamples, samples - offset);
                slot.offset = input_offset + offset;
                std::copy_n(data + offset * channels, slot.samples * channels, slot.data.data());
                offset += slot.samples;
            }
            audio.publish(blocks); // Publish the complete call atomically, never a partial chunk.
            accepted.fetch_add(samples, std::memory_order_relaxed);
        }
        input_offset += samples; // A rejected live chunk leaves an observable timeline gap.
        input_status.store(status, std::memory_order_relaxed);
        admission.fetch_and(~kSubmitting, std::memory_order_release);
        return status;
    }

    uint64_t enqueue(Kind kind, std::shared_ptr<KwsResult> result = nullptr,
            std::string message = {}, uint64_t epoch = UINT64_MAX) {
        std::lock_guard<std::mutex> lock(event_mutex);
        if (kind == Kind::RESULT && !user_callback) return event_serial;
        if (events.size() == kEventCapacity) {
            auto expendable = std::find_if(events.begin(), events.end(), [](const Event& e) {
                return e.kind == Kind::RESULT && !e.result->IsWakeWord();
            });
            if (expendable == events.end())
                expendable = std::find_if(events.begin(), events.end(), [](const Event& e) {
                    return e.kind == Kind::RESULT || e.kind == Kind::ERROR;
                });
            // Start cannot overlap an undelivered Close, so lifecycle events alone
            // cannot fill this queue. Never wait for a slow application callback.
            if (expendable != events.end()) events.erase(expendable);
            else return event_serial;
            event_overruns.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t serial = ++event_serial;
        events.push_back({kind, user_callback, std::move(result), std::move(message),
                epoch == UINT64_MAX ? generation.load() : epoch, serial});
        event_ready.notify_one();
        return serial;
    }
    void append(const std::vector<kws::DetectionResult>& results, uint64_t epoch) {
        for (const auto& result : results) {
            last_score = result.score;
            enqueue(Kind::RESULT, CallbackAdapter::convert(result), {}, epoch);
        }
    }
    void dispatchLoop() {
        uint64_t reported_overruns = 0, observed_generation = generation.load();
        std::unique_lock<std::mutex> lock(event_mutex);
        while (true) {
            event_ready.wait(lock, [&] { return event_shutdown || !events.empty(); });
            if (event_shutdown) { events.clear(); return; }
            Event event = std::move(events.front());
            events.pop_front();
            lock.unlock();
            try {
                const uint64_t epoch = generation.load();
                if (epoch != observed_generation) { observed_generation = epoch; reported_overruns = 0; }
                const uint64_t lost = event_overruns.load();
                if (event.callback && event.kind != Kind::OPEN && lost > reported_overruns) {
                    reported_overruns = lost;
                    event.callback->OnError("KWS event queue overflow; inspect GetStreamStats().event_overruns");
                }
                if (event.callback && (event.kind != Kind::RESULT || event.generation == generation.load())) {
                    switch (event.kind) {
                        case Kind::OPEN: event.callback->OnOpen(); break;
                        case Kind::COMPLETE: event.callback->OnComplete(); break;
                        case Kind::ERROR: event.callback->OnError(event.message); break;
                        case Kind::CLOSE: event.callback->OnClose(); break;
                        case Kind::RESULT:
                            event.callback->OnEvent(event.result);
                            if (event.generation == generation.load() && event.result->IsWakeWord())
                                event.callback->OnWakeWord(event.result->GetKeyword(), event.result->GetScore(),
                                        event.result->GetTimestampMs());
                            break;
                    }
                }
            } catch (...) {
                // Never let a user exception terminate the dispatcher/process.
                std::lock_guard<std::mutex> state_lock(mutex);
                last_error = kws::ErrorInfo::error(kws::ErrorCode::INTERNAL_ERROR, "Callback threw an exception");
                admission.fetch_and(~kAccepting);
                failed = true;
                ++generation; // Cancel pending results from the failed callback session.
                stopping = true;
            }
            lock.lock();
            completed_serial = event.serial;
            event_done.notify_all();
        }
    }
    void workLoop() {
        while (!shutdown) {
            if (streaming) {
                try {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (failed) {
                        if (!(admission.load(std::memory_order_acquire) & kSubmitting)) {
                            audio.discard();
                            backend->reset();
                            backend->stopStream();
                            close_serial = enqueue(Kind::CLOSE);
                            streaming = false;
                        }
                        continue;
                    }
                    const unsigned errors = input_errors.exchange(0);
                    if (errors & 1) enqueue(Kind::ERROR, nullptr, statusText(KwsAudioStatus::INVALID_PARAMETER));
                    if (errors & 2) enqueue(Kind::ERROR, nullptr, statusText(KwsAudioStatus::BUSY));
                    if (errors & 4) enqueue(Kind::ERROR, nullptr, statusText(KwsAudioStatus::QUEUE_FULL));
                    if (const auto* frame = audio.front()) {
                        if (frame->offset != expected_offset) {
                            backend->reset(); // Do not splice audio across an overrun.
                            segment_offset = frame->offset;
                        }
                        expected_offset = frame->offset + frame->samples;
                        std::vector<kws::DetectionResult> results;
                        const auto error = backend->process(kws::AudioChunk::fromFloat(
                            frame->data.data(), frame->samples, channels, config.sample_rate), results);
                        if (error.isOk()) {
                            last_error = kws::ErrorInfo::ok();
                            for (auto& r : results) r.timestamp_ms += segment_offset * 1000 / config.sample_rate;
                            append(results, generation.load());
                        } else {
                            last_error = error;
                            dropped.fetch_add(frame->samples);
                            enqueue(Kind::ERROR, nullptr, errorText(error));
                            backend->reset();
                            segment_offset = expected_offset;
                            if (error.code != kws::ErrorCode::INVALID_PARAMETER) {
                                admission.fetch_and(~kAccepting);
                                stopping = true;
                                failed = true;
                            }
                        }
                        audio.pop();
                        continue;
                    }
                    if (stopping && !(admission.load(std::memory_order_acquire) & kSubmitting)) {
                        std::vector<kws::DetectionResult> results;
                        const auto error = backend->finish(results);
                        backend->stopStream();
                        if (error.isOk()) {
                            last_error = kws::ErrorInfo::ok();
                            for (auto& r : results) r.timestamp_ms += segment_offset * 1000 / config.sample_rate;
                            append(results, generation.load());
                            enqueue(Kind::COMPLETE);
                        } else { last_error = error; enqueue(Kind::ERROR, nullptr, errorText(error)); }
                        close_serial = enqueue(Kind::CLOSE);
                        streaming = false;
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    last_error = kws::ErrorInfo::error(kws::ErrorCode::INTERNAL_ERROR, e.what());
                    admission.fetch_and(~kAccepting);
                    backend->reset();
                    backend->stopStream();
                    enqueue(Kind::ERROR, nullptr, errorText(last_error));
                    close_serial = enqueue(Kind::CLOSE);
                    streaming = false;
                }
            }
            // No syscall/condition-variable notification is needed at the audio endpoint.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void waitEvents(uint64_t serial) {
        if (std::this_thread::get_id() == dispatcher.get_id()) return;
        std::unique_lock<std::mutex> lock(event_mutex);
        event_done.wait(lock, [&] { return completed_serial >= serial || event_shutdown; });
    }
};

KwsEngine::KwsEngine(KwsBackendType backend, const std::string& model_dir)
    : impl_(std::make_unique<Impl>()) {
    impl_->config.backend = backend;
    impl_->config.model_dir = model_dir;
    impl_->setup();
}
KwsEngine::KwsEngine(const KwsConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->setup();
}
KwsEngine::~KwsEngine() = default;

std::shared_ptr<KwsResult> KwsEngine::Detect(const std::vector<float>& audio, int sample_rate) {
    const int channels = GetConfig().num_channels;
    if (channels < 1 || audio.size() % (size_t)channels != 0)
        return Detect(nullptr, 0, sample_rate);
    return Detect(audio.data(), audio.size() / (size_t)channels, sample_rate);
}

std::shared_ptr<KwsResult> KwsEngine::Detect(const float* data, size_t num_samples, int sample_rate) {
    std::lock_guard<std::mutex> control(impl_->control_mutex);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = std::make_shared<KwsResult>();
    if (!impl_->initialized) {
        result->impl_->success = false;
        result->impl_->code = "NOT_INITIALIZED";
        result->impl_->message = Impl::errorText(impl_->last_error);
        return result;
    }
    if (impl_->streaming) {
        impl_->last_error = kws::ErrorInfo::error(kws::ErrorCode::ALREADY_STARTED,
                                                    "Stop the stream before calling Detect");
    } else {
        impl_->backend->reset();
        impl_->last_score = 0.0f;
        std::vector<kws::DetectionResult> results;
        impl_->last_error = impl_->backend->process(
            kws::AudioChunk::fromFloat(data, num_samples, impl_->config.num_channels, sample_rate), results);
        if (impl_->last_error.isOk()) impl_->last_error = impl_->backend->finish(results);
        if (impl_->last_error.isOk()) {
            const kws::DetectionResult* best = nullptr;
            for (const auto& r : results) {
                if (!best || (r.is_wake_word && !best->is_wake_word) ||
                    (r.is_wake_word == best->is_wake_word && r.score > best->score)) best = &r;
            }
            if (best) {
                impl_->last_score = best->score;
                return CallbackAdapter::convert(*best);
            }
            return result;
        }
    }
    result->impl_->success = false;
    result->impl_->code = std::to_string(static_cast<int>(impl_->last_error.code));
    result->impl_->message = Impl::errorText(impl_->last_error);
    return result;
}

void KwsEngine::SetCallback(std::shared_ptr<KwsEngineCallback> callback) {
    std::shared_ptr<KwsEngineCallback> previous;
    {
        std::lock_guard<std::mutex> lock(impl_->event_mutex);
        if (callback && std::find(impl_->callback_owners.begin(), impl_->callback_owners.end(), callback) ==
                            impl_->callback_owners.end()) impl_->callback_owners.push_back(callback);
        previous = std::move(impl_->user_callback);
        impl_->user_callback = std::move(callback);
    }
}

bool KwsEngine::Start() {
    auto& p = *impl_;
    std::lock_guard<std::mutex> control(p.control_mutex);
    if (!p.initialized) return false;
    if (p.streaming) return !p.stopping;
    bool closed;
    {
        std::lock_guard<std::mutex> events(p.event_mutex);
        closed = p.completed_serial >= p.close_serial.load();
    }
    if (!closed) {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.input_status = KwsAudioStatus::ACCEPTED;
        p.last_error = kws::ErrorInfo::error(kws::ErrorCode::ALREADY_STARTED, "Previous OnClose is still pending");
        return false;
    }
    p.closeAdmission();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.audio.discard();
    p.last_error = p.backend->startStream();
    if (!p.last_error.isOk()) return false;
    p.input_offset = p.expected_offset = p.segment_offset = 0;
    p.accepted = p.dropped = p.overruns = p.event_overruns = 0;
    p.input_status = KwsAudioStatus::ACCEPTED;
    p.input_errors = 0;
    ++p.generation;
    p.last_score = 0;
    p.failed = false;
    p.stopping = false;
    p.enqueue(Impl::Kind::OPEN);
    p.streaming = true;
    p.admission.store(Impl::kAccepting, std::memory_order_release);
    return true; // Operation succeeded, regardless of a subsequent OnOpen -> Stop.
}

KwsAudioStatus KwsEngine::SendAudioFrame(const std::vector<float>& data) noexcept {
    const size_t channels = static_cast<size_t>(impl_->channels);
    if (data.size() % channels) return impl_->reject(KwsAudioStatus::INVALID_PARAMETER);
    return impl_->submit(data.data(), data.size() / channels);
}
KwsAudioStatus KwsEngine::SendAudioFrame(const float* data, size_t num_samples) noexcept {
    return impl_->submit(data, num_samples);
}
KwsStreamStats KwsEngine::GetStreamStats() const noexcept {
    const auto& p = *impl_;
    return {p.accepted.load(), p.dropped.load(), p.overruns.load(), p.event_overruns.load(), p.audio.size()};
}

void KwsEngine::Stop() {
    auto& p = *impl_;
    uint64_t serial;
    {
        std::lock_guard<std::mutex> control(p.control_mutex);
        p.closeAdmission();
        p.stopping = true;
        while (p.streaming) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        serial = p.close_serial.load();
    }
    // Never hold a control/backend lock while waiting for application callbacks.
    p.waitEvents(serial);
}

void KwsEngine::Reset() {
    auto& p = *impl_;
    std::lock_guard<std::mutex> control(p.control_mutex);
    p.closeAdmission();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.audio.discard();
    if (p.initialized) { p.backend->reset(); p.last_error = kws::ErrorInfo::ok(); }
    p.input_offset = p.expected_offset = p.segment_offset = 0;
    p.accepted = p.dropped = p.overruns = p.event_overruns = 0;
    p.input_status = KwsAudioStatus::ACCEPTED;
    p.input_errors = 0;
    ++p.generation;
    p.last_score = 0;
    if (p.streaming && !p.stopping) p.admission.store(Impl::kAccepting, std::memory_order_release);
}

bool KwsEngine::IsInitialized() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized;
}

bool KwsEngine::IsStreaming() const {
    return impl_->streaming.load(std::memory_order_acquire);
}

std::string KwsEngine::GetLastError() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->input_status.load();
    return status != KwsAudioStatus::ACCEPTED ? Impl::statusText(status) : Impl::errorText(impl_->last_error);
}

void KwsEngine::SetThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) return;
    impl_->last_error = impl_->backend->setThreshold(threshold);
    if (!impl_->last_error.isOk()) return;
    impl_->config.threshold = threshold;
    for (auto& kw : impl_->config.keywords) kw.threshold = threshold;
}

int KwsEngine::GetNumChannels() const noexcept {
    return impl_->channels;
}

KwsConfig KwsEngine::GetConfig() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config;
}

std::vector<std::string> KwsEngine::GetKeywords() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized ? impl_->backend->getKeywords() : std::vector<std::string>{};
}

std::string KwsEngine::GetEngineName() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->backend ? impl_->backend->getName() : "Unknown";
}

KwsBackendType KwsEngine::GetBackendType() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config.backend;
}

float KwsEngine::GetLastScore() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_score;
}

int KwsEngine::GetLookaheadMs() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized ? impl_->backend->getLookaheadMs() : 0;
}

}  // namespace SpacemiT
