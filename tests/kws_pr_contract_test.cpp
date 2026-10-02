/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PR contract tests. Two suites, selected by argv[1]:
 *
 *   --config-and-backend-contract  预设、配置构建器、后端流式生命周期与回调
 *   --invalid-input-error-path     错误配置与错误输入必须快速失败且不产生检测
 *
 * 前者只用假后端，不需要模型；后者会构造真正的 cFSMN 后端，但只走失败路径，
 * 同样不需要模型文件。
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/cfsmn/cfsmn_backend.hpp"
#include "backends/kws_backend.hpp"
#include "kws_service.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

class RecordingCallback : public kws::IKwsCallback {
public:
    void onStart() override { start_count++; }
    void onComplete() override { complete_count++; }
    void onClose() override { close_count++; }

    void onResult(const kws::DetectionResult& result) override {
        result_count++;
        last_result = result;
    }

    void onWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) override {
        wake_count++;
        last_keyword = keyword;
        last_score = score;
        last_wake_ms = timestamp_ms;
    }

    void onError(const kws::ErrorInfo& error) override {
        error_count++;
        last_error = error;
    }

    int start_count = 0;
    int complete_count = 0;
    int close_count = 0;
    int result_count = 0;
    int wake_count = 0;
    int error_count = 0;
    std::string last_keyword;
    float last_score = 0.0f;
    int64_t last_wake_ms = -1;
    kws::DetectionResult last_result;
    kws::ErrorInfo last_error = kws::ErrorInfo::ok();
};

// 每收到 kFramesPerHit 帧就报一次命中，用来验证框架的流式约定，不涉及真模型。
class FakeKwsBackend : public kws::IKwsBackend {
public:
    static constexpr int kFramesPerHit = 3;

    kws::ErrorInfo initialize(const kws::KwsConfig& config) override {
        if (config.keywords.empty()) {
            return kws::ErrorInfo::error(kws::ErrorCode::KEYWORD_NOT_FOUND, "No keyword");
        }
        config_ = config;
        initialized_ = true;
        return kws::ErrorInfo::ok();
    }

    void shutdown() override { initialized_ = false; }
    bool isInitialized() const override { return initialized_; }
    kws::BackendType getType() const override { return kws::BackendType::CUSTOM; }
    std::string getName() const override { return "fake"; }
    int getLookaheadMs() const override { return 240; }

    std::vector<std::string> getKeywords() const override {
        std::vector<std::string> out;
        for (const auto& kw : config_.keywords) out.push_back(kw.text);
        return out;
    }

    kws::ErrorInfo process(const kws::AudioChunk& audio,
                            std::vector<kws::DetectionResult>& results) override {
        if (!initialized_) {
            return kws::ErrorInfo::error(kws::ErrorCode::NOT_INITIALIZED, "Not initialized");
        }
        if (audio.isEmpty()) {
            return kws::ErrorInfo::error(kws::ErrorCode::INVALID_PARAMETER, "Empty audio chunk");
        }
        frames_++;
        audio_ms_ += (int64_t)(audio.num_samples * 1000 / audio.sample_rate);

        kws::DetectionResult result;
        result.keyword = config_.keywords.front().text;
        result.keyword_index = 0;
        result.timestamp_ms = audio_ms_;
        result.score = frames_ % kFramesPerHit == 0 ? 0.9f : 0.1f;
        result.is_wake_word = result.score >= config_.threshold;
        results.push_back(result);
        return kws::ErrorInfo::ok();
    }

    void reset() override {
        frames_ = 0;
        audio_ms_ = 0;
    }

    kws::ErrorInfo setThreshold(float threshold) override {
        config_.threshold = threshold;
        return kws::ErrorInfo::ok();
    }

private:
    bool initialized_ = false;
    int frames_ = 0;
    int64_t audio_ms_ = 0;
};

kws::KwsConfig fakeConfig() {
    kws::KwsConfig config;
    config.keywords = {kws::Keyword{"小进小进", {1462, 2428, 1462, 2428}, 0.0f}};
    config.threshold = 0.3f;
    return config;
}

// =============================================================================
// Suite 1: presets, config builders, streaming contract
// =============================================================================

void testPresets() {
    const auto presets = SpacemiT::KwsConfig::AvailablePresets();
    require(!presets.empty(), "AvailablePresets must not be empty");
    require(std::find(presets.begin(), presets.end(), "xiaojin") != presets.end(),
            "xiaojin preset must exist");
    require(std::find(presets.begin(), presets.end(), "xiaojin-4mic") != presets.end(),
            "xiaojin-4mic preset must exist");

    const auto mono = SpacemiT::KwsConfig::Preset("xiaojin");
    require(mono.backend == SpacemiT::KwsBackendType::CFSMN, "default backend is cFSMN");
    require(mono.num_channels == 1, "xiaojin preset is single channel");
    require(!mono.use_beamforming, "xiaojin preset has beamforming off");
    require(mono.keywords.size() == 1 && mono.keywords[0].text == "小进小进",
            "xiaojin preset carries the default keyword");

    const auto quad = SpacemiT::KwsConfig::Preset("xiaojin-4mic");
    require(quad.num_channels == 4, "xiaojin-4mic preset is 4 channels");
    require(quad.use_beamforming, "xiaojin-4mic preset enables beamforming");
    require(quad.beam_first_channel == 1, "beamforming starts at the first raw mic");

    bool threw = false;
    try {
        SpacemiT::KwsConfig::Preset("does-not-exist");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "unknown preset must throw");
}

void testConfigBuilders() {
    const auto base = SpacemiT::KwsConfig::Preset("xiaojin");
    const auto tuned = base.withThreshold(0.55f)
                            .withHoldoff(1000)
                            .withScoreInterval(3)
                            .withKeyword("你好小进");

    require(base.threshold == 0.3f, "builders must not mutate the original config");
    require(base.keywords[0].text == "小进小进", "original keyword unchanged");
    require(tuned.threshold == 0.55f, "withThreshold applies");
    require(tuned.holdoff_ms == 1000, "withHoldoff applies");
    require(tuned.score_interval == 3, "withScoreInterval applies");
    require(tuned.keywords.size() == 1 && tuned.keywords[0].text == "你好小进",
            "withKeyword replaces the keyword list");
    require(tuned.model_dir == base.model_dir, "unrelated fields are carried over");
}

void testStreamingContract() {
    FakeKwsBackend backend;
    RecordingCallback callback;
    backend.setCallback(&callback);

    require(backend.initialize(fakeConfig()).isOk(), "fake backend initializes");
    require(backend.getKeywords().size() == 1, "keyword list is exposed");
    require(backend.getLookaheadMs() == 240, "lookahead is reported");

    const std::vector<float> frame(160, 0.0f);
    auto chunk = kws::AudioChunk::fromFloat(frame.data(), frame.size(), 1, 16000);

    // 未开流时喂音频必须报错，且不产生任何结果。
    auto err = backend.feedAudio(chunk);
    require(!err.isOk() && err.code == kws::ErrorCode::NOT_STARTED,
            "feeding before start must fail with NOT_STARTED");
    require(callback.result_count == 0, "no results before the stream starts");

    require(backend.startStream().isOk(), "startStream succeeds");
    require(backend.isStreamActive(), "stream is active after start");
    require(!backend.startStream().isOk(), "starting twice must fail");

    for (int i = 0; i < FakeKwsBackend::kFramesPerHit * 2; ++i) {
        require(backend.feedAudio(chunk).isOk(), "feedAudio succeeds while streaming");
    }
    require(callback.result_count == FakeKwsBackend::kFramesPerHit * 2,
            "every frame produces exactly one result");
    require(callback.wake_count == 2, "two hits at the configured cadence");
    require(callback.last_keyword == "小进小进", "the keyword text reaches the callback");
    require(callback.last_score >= 0.3f, "wake score is above the threshold");
    require(callback.last_wake_ms > 0, "wake carries an audio timestamp");
    require(callback.error_count == 0, "no errors on the happy path");

    require(backend.stopStream().isOk(), "stopStream succeeds");
    require(!backend.isStreamActive(), "stream is inactive after stop");
    require(!backend.stopStream().isOk(), "stopping twice must fail");

    // 阈值调高后同样的输入不应再命中。
    const int wakes_before = callback.wake_count;
    require(backend.setThreshold(0.95f).isOk(), "setThreshold succeeds");
    backend.reset();
    require(backend.startStream().isOk(), "restart after reset");
    for (int i = 0; i < FakeKwsBackend::kFramesPerHit; ++i) backend.feedAudio(chunk);
    require(callback.wake_count == wakes_before, "raised threshold suppresses the hit");
    backend.stopStream();
}

// =============================================================================
// Suite 2: invalid configuration and invalid input
// =============================================================================

void testInvalidBackendConfig() {
    kws::CfsmnBackend backend;

    auto config = fakeConfig();
    config.model_dir = "/nonexistent/kws-model";
    config.sample_rate = 8000;
    auto err = backend.initialize(config);
    require(err.code == kws::ErrorCode::UNSUPPORTED_SAMPLE_RATE,
            "8 kHz must be rejected before anything else");

    config = fakeConfig();
    config.model_dir = "/nonexistent/kws-model";
    config.num_channels = 2;
    config.use_beamforming = true;
    err = backend.initialize(config);
    require(err.code == kws::ErrorCode::INVALID_CONFIG,
            "beamforming with 2 channels must be rejected");

    config = fakeConfig();
    config.model_dir = "/nonexistent/kws-model";
    config.score_interval = 0;
    err = backend.initialize(config);
    require(err.code == kws::ErrorCode::INVALID_CONFIG, "score_interval 0 must be rejected");

    config = fakeConfig();
    config.model_dir = "/nonexistent/kws-model";
    err = backend.initialize(config);
    require(err.code == kws::ErrorCode::MODEL_NOT_FOUND, "missing weights must be reported");
    require(err.detail.find("cfsmn.bin") != std::string::npos,
            "the error must name the file it looked for");
    require(!backend.isInitialized(), "backend stays uninitialized after a failure");

    // The default directory is fetched on demand; KWS_MODEL_DOWNLOAD=0 must turn that off and
    // report the missing default model without touching the network.
    const char* home = std::getenv("HOME");
    const std::string saved_home = home ? home : "";
    const char* model_dir_env = std::getenv("KWS_MODEL_DIR");
    const std::string saved_model_dir = model_dir_env ? model_dir_env : "";
    setenv("HOME", "/nonexistent/kws-home", 1);
    unsetenv("KWS_MODEL_DIR");
    setenv("KWS_MODEL_DOWNLOAD", "0", 1);
    config = fakeConfig();
    config.model_dir.clear();
    err = backend.initialize(config);
    require(err.code == kws::ErrorCode::MODEL_NOT_FOUND,
            "a missing default model with downloads off must be reported");
    require(err.detail == "/nonexistent/kws-home/.cache/models/kws/xiaojin-v1/cfsmn.bin",
            "the default model directory must be ~/.cache/models/kws/xiaojin-v1");
    unsetenv("KWS_MODEL_DOWNLOAD");
    if (home) {
        setenv("HOME", saved_home.c_str(), 1);
    } else {
        unsetenv("HOME");
    }
    if (model_dir_env) setenv("KWS_MODEL_DIR", saved_model_dir.c_str(), 1);

    std::vector<kws::DetectionResult> results;
    const std::vector<float> frame(160, 0.0f);
    err = backend.process(kws::AudioChunk::fromFloat(frame.data(), frame.size(), 1, 16000),
                            results);
    require(err.code == kws::ErrorCode::NOT_INITIALIZED,
            "process before a successful initialize must fail");
    require(results.empty(), "a failed process must not report detections");
}

void testEngineErrorPath() {
    auto config = SpacemiT::KwsConfig::Preset("xiaojin").withModelDir("/nonexistent/kws-model");
    SpacemiT::KwsEngine engine(config);

    require(!engine.IsInitialized(), "engine must not report success without a model");
    require(!engine.GetLastError().empty(), "the failure reason must be available");
    require(engine.GetLastError().find("/nonexistent/kws-model") != std::string::npos,
            "the failure reason must name the model directory");

    const std::vector<float> audio(16000, 0.0f);
    auto result = engine.Detect(audio);
    require(!result->IsSuccess(), "Detect on an uninitialized engine fails");
    require(result->GetCode() == "NOT_INITIALIZED", "and says why");
    require(!result->IsWakeWord() && result->GetScore() == 0.0f, "and reports no wake word");

    require(!engine.Start(), "Start must fail on an uninitialized engine");
    engine.SendAudioFrame(audio.data(), 160);   // must not crash
    engine.Stop();
    require(engine.GetKeywords().empty(), "no keywords are loaded");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "--config-and-backend-contract";

    if (suite == "--config-and-backend-contract") {
        testPresets();
        testConfigBuilders();
        testStreamingContract();
        std::cout << "kws config and backend contract: OK" << std::endl;
        return 0;
    }
    if (suite == "--invalid-input-error-path") {
        testInvalidBackendConfig();
        testEngineErrorPath();
        std::cout << "kws invalid input error path: OK" << std::endl;
        return 0;
    }

    std::cerr << "unknown suite: " << suite << std::endl;
    return 2;
}
