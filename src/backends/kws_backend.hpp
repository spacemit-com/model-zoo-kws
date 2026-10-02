/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Backend Interface
 *
 * 后端抽象接口定义。唤醒是连续过程：feedAudio 每次送一帧（默认 10 ms），
 * 后端按自己的节奏产生打分结果，因此没有「一帧一结果」的约定。
 */

#ifndef KWS_BACKEND_HPP
#define KWS_BACKEND_HPP

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "kws_callback.hpp"
#include "kws_types.hpp"

namespace kws {

// =============================================================================
// KWS Backend Interface
// =============================================================================

class IKwsBackend {
public:
    virtual ~IKwsBackend() = default;

    // -------------------------------------------------------------------------
    // 生命周期管理
    // -------------------------------------------------------------------------

    /// @brief 初始化后端（加载模型、解析关键词）
    virtual ErrorInfo initialize(const KwsConfig& config) = 0;

    /// @brief 释放资源
    virtual void shutdown() = 0;

    /// @brief 是否已初始化
    virtual bool isInitialized() const = 0;

    // -------------------------------------------------------------------------
    // 后端信息
    // -------------------------------------------------------------------------

    virtual BackendType getType() const = 0;
    virtual std::string getName() const = 0;
    virtual std::string getVersion() const { return "1.0.0"; }

    virtual std::vector<int> getSupportedSampleRates() const { return {16000}; }

    /// @brief 推荐帧大小（单通道采样点数）
    virtual int getRecommendedFrameSize() const { return 160; }

    /// @brief 模型固有前瞻（毫秒）
    virtual int getLookaheadMs() const { return 0; }

    /// @brief 已装载的关键词
    virtual std::vector<std::string> getKeywords() const { return {}; }

    // -------------------------------------------------------------------------
    // 检测接口
    // -------------------------------------------------------------------------

    /// @brief 送入一帧音频，追加本帧产生的全部打分结果（可能为 0 个）
    virtual ErrorInfo process(const AudioChunk& audio,
                                std::vector<DetectionResult>& results) = 0;

    /// @brief 排空有限音频的前瞻缓存，追加尾部结果；重复调用不产生新结果。
    virtual ErrorInfo finish(std::vector<DetectionResult>& results) {
        (void)results;
        return ErrorInfo::ok();
    }

    /// @brief 重置内部状态（音频时间、FSMN 记忆、静默期）
    virtual void reset() = 0;

    /// @brief 动态改阈值
    virtual ErrorInfo setThreshold(float threshold) {
        (void)threshold;
        return ErrorInfo::error(ErrorCode::INTERNAL_ERROR, "Not supported by this backend");
    }

    virtual KwsConfig getConfig() const { return config_; }

    // -------------------------------------------------------------------------
    // 流式检测
    // -------------------------------------------------------------------------

    virtual ErrorInfo startStream() {
        if (!isInitialized())
            return ErrorInfo::error(ErrorCode::NOT_INITIALIZED, "Backend not initialized");
        if (streaming_.load()) {
            return ErrorInfo::error(ErrorCode::ALREADY_STARTED, "Stream already started");
        }
        reset();
        streaming_.store(true);
        notifyStart();
        return ErrorInfo::ok();
    }

    virtual ErrorInfo feedAudio(const AudioChunk& audio) {
        if (!streaming_.load()) {
            return ErrorInfo::error(ErrorCode::NOT_STARTED, "Stream not started");
        }
        std::vector<DetectionResult> results;
        auto err = process(audio, results);
        if (!err.isOk()) {
            streaming_.store(false);
            reset();
            notifyError(err);
            notifyClose();
            return err;
        }
        for (const auto& result : results) {
            notifyResult(result);
            if (result.is_wake_word) {
                notifyWakeWord(result.keyword, result.score, result.timestamp_ms);
            }
        }
        return err;
    }

    virtual ErrorInfo stopStream() {
        if (!streaming_.load()) {
            return ErrorInfo::error(ErrorCode::NOT_STARTED, "Stream not started");
        }
        streaming_.store(false);
        std::vector<DetectionResult> results;
        auto err = finish(results);
        if (!err.isOk()) {
            notifyError(err);
            notifyClose();
            return err;
        }
        for (const auto& result : results) {
            notifyResult(result);
            if (result.is_wake_word)
                notifyWakeWord(result.keyword, result.score, result.timestamp_ms);
        }
        notifyComplete();
        notifyClose();
        return err;
    }

    virtual bool isStreamActive() const { return streaming_.load(); }

    // -------------------------------------------------------------------------
    // 回调设置
    // -------------------------------------------------------------------------

    virtual void setCallback(IKwsCallback* callback) { callback_ = callback; }
    IKwsCallback* getCallback() const { return callback_; }

protected:
    IKwsCallback* callback_ = nullptr;
    KwsConfig config_;
    std::atomic<bool> streaming_{false};

    void notifyStart() {
        if (callback_) callback_->onStart();
    }

    void notifyComplete() {
        if (callback_) callback_->onComplete();
    }

    void notifyClose() {
        if (callback_) callback_->onClose();
    }

    void notifyResult(const DetectionResult& result) {
        if (callback_) callback_->onResult(result);
    }

    void notifyWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) {
        if (callback_) callback_->onWakeWord(keyword, score, timestamp_ms);
    }

    void notifyError(const ErrorInfo& error) {
        if (callback_) callback_->onError(error);
    }
};

// =============================================================================
// Backend Factory
// =============================================================================

class KwsBackendFactory {
public:
    /// @brief 创建后端实例，失败返回 nullptr
    static std::unique_ptr<IKwsBackend> create(BackendType type);

    static bool isAvailable(BackendType type);
    static std::vector<BackendType> getAvailableBackends();
    static int getDefaultSampleRate(BackendType type);
    static int getRecommendedFrameSize(BackendType type);
};

}  // namespace kws

#endif  // KWS_BACKEND_HPP
