/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kws_aec_audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>
#include "spsc_queue.hpp"
#include <vector>

#include "audio_duplex.hpp"
#include "wav_reader.hpp"
#include <webrtc/modules/audio_processing/include/audio_processing.h>

namespace kws_demo {
namespace {
constexpr int kRate = 16000;
constexpr int kFrames = 160;
constexpr size_t kQueueSize = 32;

class WavWriter {
public:
    ~WavWriter() { Close(); }
    bool Open(const std::string& path, int channels) {
        file_ = std::fopen(path.c_str(), "wbx");  // Never replace an existing recording.
        if (!file_) { perror(path.c_str()); return false; }
        channels_ = channels;
        return Header();
    }
    bool Write(const float* data, size_t frames) {
        if (!file_) return true;
        if (frames + frames_ > (UINT32_MAX - 36U) / (2U * channels_)) return false;
        std::array<uint8_t, kFrames * 64 * 2> pcm{};
        if (frames > kFrames) return false;
        const size_t samples = frames * channels_;
        for (size_t i = 0; i < samples; ++i) {
            const float v = std::isfinite(data[i]) ? data[i] : 0;
            const int value = static_cast<int>(std::clamp(v * 32768.0f, -32768.0f, 32767.0f));
            const uint16_t bits = static_cast<uint16_t>(value);
            pcm[2 * i] = bits & 255;
            pcm[2 * i + 1] = bits >> 8;
        }
        const bool ok = std::fwrite(pcm.data(), 2, samples, file_) == samples;
        if (ok) frames_ += frames;
        // Keep the header valid about once a second: a recording cut short by a
        // signal or a dead pipe still plays back up to the last sync.
        if (ok && frames_ - synced_ >= kRate) return Sync();
        return ok;
    }
    bool Sync() {
        synced_ = frames_;
        return std::fseek(file_, 0, SEEK_SET) == 0 && Header() && std::fseek(file_, 0, SEEK_END) == 0 &&
                std::fflush(file_) == 0;
    }
    bool Close() {
        if (!file_) return true;
        const bool ok = std::fseek(file_, 0, SEEK_SET) == 0 && Header();
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        return ok && closed;
    }
private:
    bool Header() {
        std::array<uint8_t, 44> h{};
        const auto le = [&](size_t at, uint32_t value, int bytes) {
            for (int i = 0; i < bytes; ++i) h[at + i] = (value >> (8 * i)) & 255;
        };
        std::memcpy(h.data(), "RIFF", 4);
        le(4, 36 + frames_ * channels_ * 2, 4);
        std::memcpy(h.data() + 8, "WAVEfmt ", 8);
        le(16, 16, 4); le(20, 1, 2); le(22, channels_, 2);
        le(24, kRate, 4); le(28, kRate * channels_ * 2, 4);
        le(32, channels_ * 2, 2); le(34, 16, 2);
        std::memcpy(h.data() + 36, "data", 4);
        le(40, frames_ * channels_ * 2, 4);
        return std::fwrite(h.data(), 1, h.size(), file_) == h.size();
    }
    FILE* file_ = nullptr;
    int channels_ = 1;
    size_t frames_ = 0;
    size_t synced_ = 0;
};

bool loadMono(const std::string& path, std::vector<float>& samples) {
    if (path.empty()) return true;
    Wav wav;
    if (!readWav(path.c_str(), wav)) return false;
    if (wav.channels != 1) {
        fprintf(stderr, "AEC playback expects a 16 kHz mono PCM16 WAV: %s\n", path.c_str());
        return false;
    }
    samples = std::move(wav.samples);
    return true;
}
}  // namespace

struct WebRtcAec::Impl {
    int channels, first, microphones, delay_ms;
    std::vector<rtc::scoped_refptr<webrtc::AudioProcessing>> filters;
    std::string error;
};

WebRtcAec::WebRtcAec(int channels, int first, int microphones, int delay_ms)
    : impl_(new Impl{channels, first, microphones, delay_ms, {}, {}}) {}
WebRtcAec::~WebRtcAec() = default;
const std::string& WebRtcAec::Error() const { return impl_->error; }

bool WebRtcAec::Initialize() {
    auto& p = *impl_;
    if (p.channels < 1 || p.channels > 64 || p.first < 0 || p.microphones < 1 ||
        p.first + p.microphones > p.channels || p.delay_ms < 0 || p.delay_ms > 500) {
        p.error = "Invalid AEC channel or delay configuration";
        return false;
    }
    p.filters.clear();
    for (int c = 0; c < p.microphones; ++c) {
        auto apm = webrtc::AudioProcessingBuilder().Create();
        if (!apm) { p.error = "Could not create WebRTC APM"; return false; }
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        config.echo_canceller.mobile_mode = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;
        config.noise_suppression.enabled = false;
        apm->ApplyConfig(config);
        if (apm->Initialize() != 0) { p.error = "Could not initialize WebRTC APM"; return false; }
        p.filters.push_back(std::move(apm));
    }
    return true;
}

bool WebRtcAec::Process(const float* capture, const float* render, float* output) {
    auto& p = *impl_;
    if (!capture || !render || !output || p.filters.size() != static_cast<size_t>(p.microphones)) {
        p.error = "AEC is not initialized or received null audio";
        return false;
    }
    std::copy_n(capture, kFrames * p.channels, output);
    const webrtc::StreamConfig mono(kRate, 1);
    std::array<float, kFrames> mic{}, clean{}, reverse{};
    const float* ref_ptr[] = {render};
    float* reverse_ptr[] = {reverse.data()};
    const float* mic_ptr[] = {mic.data()};
    float* clean_ptr[] = {clean.data()};
    for (int c = 0; c < p.microphones; ++c) {
        auto& apm = p.filters[c];
        for (int i = 0; i < kFrames; ++i) mic[i] = capture[i * p.channels + p.first + c];
        // No manual reference shift: WebRTC owns reference history and delay alignment.
        int rc = apm->ProcessReverseStream(ref_ptr, mono, mono, reverse_ptr);
        if (rc == 0) rc = apm->set_stream_delay_ms(p.delay_ms);
        if (rc == 0) rc = apm->ProcessStream(mic_ptr, mono, mono, clean_ptr);
        if (rc != 0) {
            p.error = "WebRTC processing failed on microphone " + std::to_string(p.first + c) +
                " (code " + std::to_string(rc) + ")";
            return false;
        }
        for (int i = 0; i < kFrames; ++i) output[i * p.channels + p.first + c] = clean[i];
    }
    return true;
}

struct AecDuplexAudio::Impl {
    struct Frame {
        std::vector<float> input;
        std::array<float, kFrames> reference{};
        uint64_t offset = 0;
    };
    explicit Impl(const AecAudioOptions& value)
        : options(value), aec(value.channels, value.first_microphone, value.microphones, value.delay_ms),
            duplex(value.input_device, value.output_device), queue(kQueueSize) {
        for (auto& frame : queue.storage()) frame.input.resize(kFrames * options.channels);
    }
    AecAudioOptions options;
    WebRtcAec aec;
    SpacemitAudio::AudioDuplex duplex;
    std::vector<float> response, playback;
    const std::vector<float>* playing = nullptr;
    size_t position = 0;
    std::atomic<bool> requested{false}, running{false};
    kws::SpscQueue<Frame> queue;
    uint64_t capture_offset = 0;
    size_t high_water = 0; // Producer only, read after Close joins it.
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> callback_error{false};
    std::string error;
    WavWriter raw_file, clean_file, ref_file;

    void Capture(const float* input, float* output, size_t frames, int inputs, int outputs) {
        if (output) std::fill_n(output, frames * outputs, 0.0f);
        if (frames != kFrames || inputs != options.channels || outputs != options.output_channels ||
            !input || !output) {
            callback_error = true;
            return;
        }
        // A fresh wake can interrupt and restart the acknowledgement. Do not
        // discard it merely because the previous acknowledgement is playing.
        // The explicit diagnostic stimulus keeps its fixed playback timeline.
        if (requested.exchange(false) && playing != &playback && !response.empty()) {
            playing = &response;
            position = 0;
        }
        std::array<float, kFrames> reference{};
        if (playing) {
            const size_t count = std::min<size_t>(kFrames, playing->size() - position);
            std::copy_n(playing->data() + position, count, reference.data());
            position += count;
            if (position == playing->size()) playing = nullptr;
        }
        for (int i = 0; i < kFrames; ++i)
            for (int c = 0; c < outputs; ++c) output[i * outputs + c] = reference[i];
        // The audio callback only fills output and copies aligned capture/reference frames.
        // AEC, inference, disk writes, and logging run on the consumer thread below.
        if (queue.freeSlots()) {
            auto& frame = queue.writeSlot();
            std::copy_n(input, kFrames * inputs, frame.input.data());
            frame.reference = reference;
            frame.offset = capture_offset;
            queue.publish();
            high_water = std::max(high_water, queue.size());
        } else {
            dropped.fetch_add(kFrames, std::memory_order_relaxed);
        }
        capture_offset += kFrames;
    }
};

AecDuplexAudio::AecDuplexAudio(const AecAudioOptions& options) : impl_(new Impl(options)) {}
AecDuplexAudio::~AecDuplexAudio() {
    impl_->running = false;
    impl_->duplex.Stop();
    impl_->duplex.Close();
}
const std::string& AecDuplexAudio::Error() const { return impl_->error; }

bool AecDuplexAudio::Initialize() {
    auto& p = *impl_;
    if (!p.options.bypass && !p.aec.Initialize()) { p.error = p.aec.Error(); return false; }
    if (!loadMono(p.options.response_wav, p.response) || !loadMono(p.options.playback_wav, p.playback)) {
        p.error = "Could not load AEC playback WAV";
        return false;
    }
    if (!p.options.record_prefix.empty()) {
        const auto& name = p.options.record_prefix;
        if (!p.raw_file.Open(name + ".raw.wav", p.options.channels) ||
            !p.clean_file.Open(name + ".aec.wav", p.options.channels) ||
            !p.ref_file.Open(name + ".ref.wav", 1)) {
            p.error = "Could not create AEC recording files (existing files are not replaced)";
            return false;
        }
    }
    p.duplex.SetCallbackEx([&p](const float* in, float* out, size_t frames, int inputs, int outputs) {
        p.Capture(in, out, frames, inputs, outputs);
    });
    return true;
}

void AecDuplexAudio::PlayResponse() {
    if (impl_->running && !impl_->response.empty()) impl_->requested = true;
}

bool AecDuplexAudio::Run(SpacemiT::KwsEngine& engine, int seconds,
                        const volatile std::sig_atomic_t& stop_requested) {
    auto& p = *impl_;
    if (!p.playback.empty()) p.playing = &p.playback;
    p.running = true;
    if (!p.duplex.Start(kRate, p.options.channels, p.options.output_channels, kFrames)) {
        p.running = false;
        p.duplex.Close();
        p.error = "Could not start full-duplex audio; check -l, -i, -o, and channel counts";
        return false;
    }
    printf("AEC %s: duplex 16000 Hz, input=%d/%dch output=%d/%dch, microphones=%d..%d, delay=%d ms\n",
            p.options.bypass ? "BYPASS" : "ON (WebRTC AEC3)", p.options.input_device, p.options.channels,
            p.options.output_device, p.options.output_channels, p.options.first_microphone,
            p.options.first_microphone + p.options.microphones - 1, p.options.delay_ms);
    printf("microphone listening: full-duplex; Ctrl+C to stop\n");
    fflush(stdout);
    const auto started = std::chrono::steady_clock::now();
    auto last_audio = started;
    Impl::Frame frame;
    frame.input.resize(kFrames * p.options.channels);
    std::vector<float> clean(frame.input.size());
    size_t processed = 0;
    uint64_t expected_offset = 0;
    bool ok = true;
    const auto process = [&]() {
        if (frame.offset != expected_offset) {
            // Capture and render are dropped together. Reset state at the next
            // accepted block instead of joining noncontiguous AEC/CTC history.
            if (!p.options.bypass && !p.aec.Initialize()) { p.error = p.aec.Error(); return false; }
            engine.Reset();
        }
        expected_offset = frame.offset + kFrames;
        if (p.options.bypass) clean = frame.input;
        else if (!p.aec.Process(frame.input.data(), frame.reference.data(), clean.data())) {
            p.error = p.aec.Error();
            return false;
        }
        if (!p.raw_file.Write(frame.input.data(), kFrames) || !p.clean_file.Write(clean.data(), kFrames) ||
            !p.ref_file.Write(frame.reference.data(), kFrames)) {
            p.error = "AEC recording write failed";
            return false;
        }
        const auto status = engine.SendAudioFrame(clean.data(), kFrames);
        if (status != SpacemiT::KwsAudioStatus::ACCEPTED && status != SpacemiT::KwsAudioStatus::QUEUE_FULL) {
            p.error = "KWS rejected an AEC audio block";
            return false;
        }
        processed += kFrames;
        if (!engine.IsStreaming()) {
            p.error = "KWS engine stopped while processing duplex audio";
            return false;
        }
        return true;
    };
    const auto pop = [&]() {
        const auto* queued = p.queue.front();
        if (!queued) return false;
        std::copy(queued->input.begin(), queued->input.end(), frame.input.begin());
        frame.reference = queued->reference;
        frame.offset = queued->offset;
        p.queue.pop();
        return true;
    };
    while (!stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (seconds > 0 && now - started >= std::chrono::seconds(seconds)) break;
        if (p.callback_error) {
            p.error = "Invalid duplex block";
            ok = false;
            break;
        }
        const bool have_frame = pop();
        if (have_frame) {
            if (!process()) { ok = false; break; }
            last_audio = std::chrono::steady_clock::now();
        } else if (!p.duplex.IsRunning() || std::chrono::steady_clock::now() - last_audio >
                    std::chrono::seconds(5)) {
            p.error = "Full-duplex microphone stopped delivering audio";
            ok = false;
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    p.running = false;
    p.duplex.Stop();
    p.duplex.Close();
    if (p.callback_error) {
        p.error = "Invalid duplex block";
        ok = false;
    }
    while (ok && pop()) ok = process();
    const bool files_ok = p.raw_file.Close() & p.clean_file.Close() & p.ref_file.Close();
    if (!files_ok) p.error = "Could not finalize AEC recordings";
    printf("captured %zu frames (%.2f s), AEC queue high-water %zu blocks (%.0f ms)\n",
            processed, processed / 16000.0, p.high_water, p.high_water * 10.0);
    if (p.dropped) fprintf(stderr, "AEC capture overrun: dropped %llu aligned samples; state recovered\n",
            static_cast<unsigned long long>(p.dropped.load()));
    return ok && files_ok && (processed > 0 || stop_requested);
}

}  // namespace kws_demo
