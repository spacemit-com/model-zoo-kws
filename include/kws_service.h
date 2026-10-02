/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SpacemiT KWS（关键词唤醒）引擎 C++ 接口。提供整段检测与流式检测。
 */

#ifndef KWS_SERVICE_H
#define KWS_SERVICE_H

#include <cstdint>
#include <cstddef>

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
    std::string model_dir;                 ///< 模型目录，默认 ~/.cache/models/kws/xiaojin-v1（仅默认目录缺失时自动下载）
    std::vector<KwsKeyword> keywords;      ///< 空表示使用模型目录中的默认关键词

    // 音频参数：16 kHz、int16 归一化到 [-1, 1] 的 float。
    int sample_rate = 16000;
    int num_channels = 1;                  ///< 送入的交织通道数，4 = SPV 复合设备
    int frame_size = 160;                  ///< 调用方建议分块大小；内部 hop 固定 160，可送任意长度

    // 前处理：3 麦固定 MVDR 波束，要求 num_channels >= beam_first_channel + 3。
    bool use_beamforming = false;
    int beam_first_channel = 1;            ///< SPV：ch0 为板端处理结果，ch1~ch3 为裸麦

    // 检测参数
    float threshold = 0.3f;                ///< 关键词得分阈值
    int holdoff_ms = 2500;                 ///< 两次上报之间的静默期
    int decode_context = 86;               ///< 解码上下文，模型帧（30 ms/帧）
    int score_interval = 1;                ///< 每 N 个模型帧打一次分
    int num_threads = 1;                   ///< 当前仅支持单线程推理，其他值初始化失败

    // 部分匹配（实验）：快读时常只解出关键词的一半（"小进小进"只出一个"小进"）。
    // 完整关键词未命中、且前缀/后缀静置 partial_wait_ms 后，用这个更严的阈值判定；0 关闭。
    float partial_threshold = 0.0f;
    int partial_wait_ms = 500;             ///< 部分匹配结束后等待完整关键词的时间

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

    int64_t GetTimestampMs() const;  ///< 命中时为末 token 对齐帧的结束时间（30 ms 粒度），非命中为打分帧时间
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

enum class KwsAudioStatus {
    ACCEPTED, EMPTY, NOT_STARTED, INVALID_PARAMETER, QUEUE_FULL, BUSY
};

struct KwsStreamStats {
    uint64_t accepted_samples = 0; // Per channel, since Start/Reset.
    uint64_t dropped_samples = 0;
    uint64_t input_overruns = 0;
    uint64_t event_overruns = 0;
    size_t queued_blocks = 0;     // Each block contains at most 160 samples/channel.
};

// 回调由独立事件线程按序执行；不得在回调中销毁引擎。
// Stop 在外部线程等待排空和关闭回调；在回调内只等待推理排空，不等待自身。
// 输入拒绝/溢出只报告 OnError，监听继续；内部故障才报告 OnError → OnClose。
// 回调在锁外按序执行，可以调用 Stop/Reset/SetThreshold/SetCallback。Stop 会排空已有音频，
// 其尾部事件和关闭事件排在当前回调之后；Reset 丢弃尚未分发的旧打分事件。
// 回调不应抛异常；引擎析构须等待所有调用/回调结束，不能在回调内销毁引擎。
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

    // 整段检测：包含前瞻缓存的尾部结果，返回最高分（命中优先）；开流期间不允许调用。
    // 失败时 IsSuccess() 为 false。短于完整特征窗口的音频成功返回零分。
    // audio 为 [-1, 1] 的 float，按 config.num_channels 交织。
    std::shared_ptr<KwsResult> Detect(const std::vector<float>& audio,
                                        int sample_rate = 16000);
    std::shared_ptr<KwsResult> Detect(const float* data, size_t num_samples,
                                        int sample_rate = 16000);

    void SetCallback(std::shared_ptr<KwsEngineCallback> callback);
    bool Start();
    // 非阻塞、无分配、无锁等待；整块接受或拒绝。音频在返回前复制完成。
    // 单个生产者；并发送帧立即返回 BUSY。最多 20480 样本/通道/次。
    KwsAudioStatus SendAudioFrame(const std::vector<float>& data) noexcept;
    KwsAudioStatus SendAudioFrame(const float* data, size_t num_samples) noexcept;
    KwsStreamStats GetStreamStats() const noexcept;
    int GetNumChannels() const noexcept;  ///< 无锁，送帧线程可调用
    void Stop();   ///< 排空尾部结果并关闭；重复调用不产生新事件

    void Reset();
    bool IsInitialized() const;
    bool IsStreaming() const;

    // 最近一次初始化、检测、送帧或阈值更新的错误，成功时为空串。
    std::string GetLastError() const;

    void SetThreshold(float threshold);  ///< 非有限值或不在 [0,1] 内时保留原值，并设置 GetLastError()
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
