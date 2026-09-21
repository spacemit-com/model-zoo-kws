/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SpacemiT KWS（关键词唤醒）引擎 C++ 接口。提供整段检测与流式检测。
 */

#ifndef KWS_SERVICE_H
#define KWS_SERVICE_H

#include <cstdint>

#include <memory>
#include <string>
#include <vector>

// Forward declaration of internal types
namespace kws {
    class IKwsBackend;
    struct DetectionResult;
    struct ErrorInfo;
}

namespace SpacemiT {

// -----------------------------------------------------------------------------
// KwsBackendType
// -----------------------------------------------------------------------------

enum class KwsBackendType {
    CFSMN,   ///< 内置 cFSMN char-CTC 检测器，纯 C++，无推理引擎依赖
    CUSTOM,  ///< 自定义后端
};

// -----------------------------------------------------------------------------
// KwsKeyword
// -----------------------------------------------------------------------------

// 一个关键词。token_ids 为空时由模型目录的 keywords.txt 解析 text 得到。
struct KwsKeyword {
    std::string text;
    std::vector<int> token_ids;
    float threshold = 0.0f;   ///< 0 表示用 KwsConfig::threshold
};

// -----------------------------------------------------------------------------
// KwsConfig
// -----------------------------------------------------------------------------

// 引擎配置，可用 Preset("xiaojin") 创建。
struct KwsConfig {
    KwsBackendType backend = KwsBackendType::CFSMN;
    std::string model_dir;                 ///< 模型目录，默认 ~/.cache/models/kws/xiaojin
    std::vector<KwsKeyword> keywords;      ///< 空表示使用模型目录中的默认关键词

    // 音频参数：16 kHz、int16 归一化到 [-1, 1] 的 float。
    int sample_rate = 16000;
    int num_channels = 1;                  ///< 送入的交织通道数，4 = SPV 复合设备
    int frame_size = 160;                  ///< 每帧采样点数（单通道计），10 ms

    // 前处理：3 麦固定 MVDR 波束，要求 num_channels >= beam_first_channel + 3。
    bool use_beamforming = false;
    int beam_first_channel = 1;            ///< SPV：ch0 为板端处理结果，ch1~ch3 为裸麦

    // 检测参数
    float threshold = 0.3f;                ///< 关键词得分阈值
    int holdoff_ms = 2500;                 ///< 两次上报之间的静默期
    int decode_context = 86;               ///< 解码上下文，模型帧（30 ms/帧）
    int score_interval = 1;                ///< 每 N 个模型帧打一次分
    int num_threads = 1;

    static KwsConfig Preset(const std::string& name);
    static std::vector<std::string> AvailablePresets();

    KwsConfig withModelDir(const std::string& dir) const {
        auto c = *this;
        c.model_dir = dir;
        return c;
    }
    KwsConfig withKeyword(const std::string& text) const {
        auto c = *this;
        c.keywords = {KwsKeyword{text, {}, 0.0f}};
        return c;
    }
    KwsConfig withKeywords(const std::vector<KwsKeyword>& keywords) const {
        auto c = *this;
        c.keywords = keywords;
        return c;
    }
    KwsConfig withThreshold(float threshold) const {
        auto c = *this;
        c.threshold = threshold;
        return c;
    }
    KwsConfig withHoldoff(int ms) const {
        auto c = *this;
        c.holdoff_ms = ms;
        return c;
    }
    KwsConfig withChannels(int channels) const {
        auto c = *this;
        c.num_channels = channels;
        return c;
    }
    KwsConfig withBeamforming(bool enable) const {
        auto c = *this;
        c.use_beamforming = enable;
        return c;
    }
    KwsConfig withScoreInterval(int frames) const {
        auto c = *this;
        c.score_interval = frames;
        return c;
    }
    KwsConfig withDecodeContext(int frames) const {
        auto c = *this;
        c.decode_context = frames;
        return c;
    }
    KwsConfig withSampleRate(int rate) const {
        auto c = *this;
        c.sample_rate = rate;
        return c;
    }
    KwsConfig withNumThreads(int threads) const {
        auto c = *this;
        c.num_threads = threads;
        return c;
    }
};

// -----------------------------------------------------------------------------
// KwsResult
// -----------------------------------------------------------------------------

// 一次打分的结果。IsWakeWord() 为 true 表示越过阈值且通过了静默期。
class KwsResult {
public:
    KwsResult();
    ~KwsResult();

    KwsResult(const KwsResult&) = delete;
    KwsResult& operator=(const KwsResult&) = delete;
    KwsResult(KwsResult&&) noexcept;
    KwsResult& operator=(KwsResult&&) noexcept;

    float GetScore() const;
    bool IsWakeWord() const;
    std::string GetKeyword() const;
    int GetKeywordIndex() const;    ///< 命中的关键词下标，未命中为 -1

    int64_t GetTimestampMs() const;  ///< 关键词结束处的音频时间，已扣除模型前瞻
    int GetProcessingTimeMs() const;

    bool IsSuccess() const;
    std::string GetCode() const;
    std::string GetMessage() const;

private:
    friend class KwsEngine;
    friend class CallbackAdapter;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// KwsEngineCallback
// -----------------------------------------------------------------------------

// 流式回调：OnOpen → OnEvent（命中时另有 OnWakeWord）→ OnComplete → OnClose，出错时 OnError → OnClose。
class KwsEngineCallback {
public:
    virtual ~KwsEngineCallback() = default;

    virtual void OnOpen() {}
    virtual void OnEvent(std::shared_ptr<KwsResult> result) {}
    virtual void OnWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) {}
    virtual void OnComplete() {}
    virtual void OnError(const std::string& message) {}
    virtual void OnClose() {}
};

// -----------------------------------------------------------------------------
// KwsEngine
// -----------------------------------------------------------------------------

// 关键词唤醒引擎，支持整段检测与流式检测。
class KwsEngine {
public:
    explicit KwsEngine(KwsBackendType backend = KwsBackendType::CFSMN,
                        const std::string& model_dir = "");
    explicit KwsEngine(const KwsConfig& config);
    virtual ~KwsEngine();

    KwsEngine(const KwsEngine&) = delete;
    KwsEngine& operator=(const KwsEngine&) = delete;

    // 整段检测：返回整段中最高的一次打分，失败时 IsSuccess() 为 false。
    // audio 为 [-1, 1] 的 float，按 config.num_channels 交织。
    std::shared_ptr<KwsResult> Detect(const std::vector<float>& audio,
                                        int sample_rate = 16000);
    std::shared_ptr<KwsResult> Detect(const float* data, size_t num_samples,
                                        int sample_rate = 16000);

    void SetCallback(std::shared_ptr<KwsEngineCallback> callback);
    bool Start();
    void SendAudioFrame(const std::vector<float>& data);
    void SendAudioFrame(const float* data, size_t num_samples);
    void Stop();

    void Reset();
    bool IsInitialized() const;
    bool IsStreaming() const;

    // 初始化失败的原因（模型缺失、关键词无 token id 等），成功时为空串。
    std::string GetLastError() const;

    void SetThreshold(float threshold);
    KwsConfig GetConfig() const;
    std::vector<std::string> GetKeywords() const;

    std::string GetEngineName() const;
    KwsBackendType GetBackendType() const;
    float GetLastScore() const;

    // 模型固有的前瞻（cFSMN 右序 2 帧 × 4 层 = 240 ms），排期时有用。
    int GetLookaheadMs() const;

private:
    friend class CallbackAdapter;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace SpacemiT

#endif  // KWS_SERVICE_H
