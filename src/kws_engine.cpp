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
        default:                     return kws::BackendType::CFSMN;
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

// =============================================================================
// Callback Adapter
// =============================================================================

class CallbackAdapter : public kws::IKwsCallback {
public:
    CallbackAdapter(std::shared_ptr<KwsEngineCallback> callback, float* last_score)
        : callback_(callback), last_score_(last_score) {}

    void onStart() override {
        if (callback_) callback_->OnOpen();
    }

    void onComplete() override {
        if (callback_) callback_->OnComplete();
    }

    void onClose() override {
        if (callback_) callback_->OnClose();
    }

    void onResult(const kws::DetectionResult& result) override {
        if (last_score_) *last_score_ = result.score;
        if (!callback_) return;

        auto kwsResult = std::make_shared<KwsResult>();
        kwsResult->impl_->score = result.score;
        kwsResult->impl_->is_wake_word = result.is_wake_word;
        kwsResult->impl_->keyword_index = result.keyword_index;
        kwsResult->impl_->keyword = result.keyword;
        kwsResult->impl_->timestamp_ms = result.timestamp_ms;
        kwsResult->impl_->processing_time_ms = result.processing_time_ms;
        kwsResult->impl_->success = true;

        callback_->OnEvent(kwsResult);
    }

    void onWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) override {
        if (callback_) callback_->OnWakeWord(keyword, score, timestamp_ms);
    }

    void onError(const kws::ErrorInfo& error) override {
        if (callback_) {
            callback_->OnError(error.detail.empty() ? error.message
                                                    : error.message + ": " + error.detail);
        }
    }

private:
    std::shared_ptr<KwsEngineCallback> callback_;
    float* last_score_ = nullptr;
};

// =============================================================================
// KwsEngine Implementation (Pimpl)
// =============================================================================

struct KwsEngine::Impl {
    KwsConfig config;
    std::unique_ptr<kws::IKwsBackend> backend;
    std::unique_ptr<CallbackAdapter> callback_adapter;
    std::shared_ptr<KwsEngineCallback> user_callback;
    std::mutex mutex;
    bool initialized = false;
    bool streaming = false;
    float last_score = 0.0f;
    kws::ErrorInfo last_error = kws::ErrorInfo::ok();

    void setup() {
        backend = kws::KwsBackendFactory::create(toInternalBackendType(config.backend));
        if (!backend) {
            last_error = kws::ErrorInfo::error(kws::ErrorCode::INVALID_CONFIG,
                                                "Backend not available");
            return;
        }
        last_error = backend->initialize(toInternalConfig(config));
        initialized = last_error.isOk();
    }
};

KwsEngine::KwsEngine(KwsBackendType backend, const std::string& model_dir)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = KwsConfig();
    impl_->config.backend = backend;
    if (!model_dir.empty()) {
        impl_->config.model_dir = model_dir;
    }
    impl_->setup();
}

KwsEngine::KwsEngine(const KwsConfig& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->setup();
}

KwsEngine::~KwsEngine() {
    // 不在析构里加锁：先摘回调，避免回调进入已经销毁的对象（尤其是 Python 侧）。
    if (impl_) {
        impl_->user_callback.reset();
        impl_->callback_adapter.reset();
        if (impl_->backend) {
            impl_->backend->setCallback(nullptr);
            if (impl_->streaming) {
                impl_->backend->stopStream();
            }
            impl_->backend->shutdown();
        }
        impl_->streaming = false;
    }
}

std::shared_ptr<KwsResult> KwsEngine::Detect(const std::vector<float>& audio, int sample_rate) {
    const int channels = impl_->config.num_channels > 0 ? impl_->config.num_channels : 1;
    return Detect(audio.data(), audio.size() / (size_t)channels, sample_rate);
}

// 整段检测：把音频喂完，返回其中最高的一次打分（命中优先）。
std::shared_ptr<KwsResult> KwsEngine::Detect(const float* data, size_t num_samples,
                                                int sample_rate) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto result = std::make_shared<KwsResult>();

    if (!impl_->backend || !impl_->initialized) {
        result->impl_->success = false;
        result->impl_->code = "NOT_INITIALIZED";
        result->impl_->message = impl_->last_error.isOk()
            ? "KWS engine not initialized"
            : impl_->last_error.message + ": " + impl_->last_error.detail;
        return result;
    }

    impl_->backend->reset();
    auto chunk = kws::AudioChunk::fromFloat(data, num_samples, impl_->config.num_channels,
                                            sample_rate);
    std::vector<kws::DetectionResult> results;
    auto err = impl_->backend->process(chunk, results);
    if (!err.isOk()) {
        result->impl_->success = false;
        result->impl_->code = std::to_string(static_cast<int>(err.code));
        result->impl_->message = err.message;
        return result;
    }

    const kws::DetectionResult* best = nullptr;
    for (const auto& r : results) {
        if (!best || (r.is_wake_word && !best->is_wake_word) ||
            (r.is_wake_word == best->is_wake_word && r.score > best->score)) {
            best = &r;
        }
    }
    if (best) {
        result->impl_->score = best->score;
        result->impl_->is_wake_word = best->is_wake_word;
        result->impl_->keyword_index = best->keyword_index;
        result->impl_->keyword = best->keyword;
        result->impl_->timestamp_ms = best->timestamp_ms;
        result->impl_->processing_time_ms = best->processing_time_ms;
        impl_->last_score = best->score;
    }
    result->impl_->success = true;
    return result;
}

void KwsEngine::SetCallback(std::shared_ptr<KwsEngineCallback> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->user_callback = callback;
    impl_->callback_adapter = std::make_unique<CallbackAdapter>(callback, &impl_->last_score);
    if (impl_->backend) {
        impl_->backend->setCallback(impl_->callback_adapter.get());
    }
}

bool KwsEngine::Start() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (!impl_->backend || !impl_->initialized) {
        return false;
    }
    if (impl_->streaming) {
        return true;
    }

    impl_->backend->reset();
    auto err = impl_->backend->startStream();
    if (err.isOk()) {
        impl_->streaming = true;
        if (impl_->callback_adapter) {
            impl_->callback_adapter->onStart();
        }
        return true;
    }
    return false;
}

void KwsEngine::SendAudioFrame(const std::vector<float>& data) {
    const int channels = impl_->config.num_channels > 0 ? impl_->config.num_channels : 1;
    SendAudioFrame(data.data(), data.size() / (size_t)channels);
}

// num_samples 为单通道采样点数；多通道时 data 按 config.num_channels 交织。
void KwsEngine::SendAudioFrame(const float* data, size_t num_samples) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (!impl_->backend || !impl_->streaming) {
        return;
    }

    auto chunk = kws::AudioChunk::fromFloat(data, num_samples, impl_->config.num_channels,
                                            impl_->config.sample_rate);
    impl_->backend->feedAudio(chunk);
}

void KwsEngine::Stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (!impl_->backend || !impl_->streaming) {
        return;
    }

    impl_->backend->stopStream();
    impl_->streaming = false;

    if (impl_->callback_adapter) {
        impl_->callback_adapter->onComplete();
        impl_->callback_adapter->onClose();
    }
}

void KwsEngine::Reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->backend) {
        impl_->backend->reset();
    }
    impl_->last_score = 0.0f;
}

bool KwsEngine::IsInitialized() const { return impl_->initialized; }
bool KwsEngine::IsStreaming() const { return impl_->streaming; }

std::string KwsEngine::GetLastError() const {
    if (impl_->last_error.isOk()) return "";
    return impl_->last_error.detail.empty()
        ? impl_->last_error.message
        : impl_->last_error.message + ": " + impl_->last_error.detail;
}

void KwsEngine::SetThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config.threshold = threshold;
    for (KwsKeyword& kw : impl_->config.keywords) kw.threshold = threshold;
    if (impl_->backend) {
        impl_->backend->setThreshold(threshold);
    }
}

KwsConfig KwsEngine::GetConfig() const { return impl_->config; }

std::vector<std::string> KwsEngine::GetKeywords() const {
    if (impl_->backend) {
        return impl_->backend->getKeywords();
    }
    return {};
}

std::string KwsEngine::GetEngineName() const {
    if (impl_->backend) {
        return impl_->backend->getName();
    }
    return "Unknown";
}

KwsBackendType KwsEngine::GetBackendType() const { return impl_->config.backend; }

float KwsEngine::GetLastScore() const { return impl_->last_score; }

int KwsEngine::GetLookaheadMs() const {
    if (impl_->backend) {
        return impl_->backend->getLookaheadMs();
    }
    return 0;
}

}  // namespace SpacemiT
