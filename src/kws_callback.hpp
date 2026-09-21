/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Callback Interface
 *
 * 内部回调接口定义。
 */

#ifndef KWS_CALLBACK_HPP
#define KWS_CALLBACK_HPP

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "kws_types.hpp"

namespace kws {

// =============================================================================
// KWS Callback Interface
// =============================================================================

class IKwsCallback {
public:
    virtual ~IKwsCallback() = default;

    /// @brief 检测会话开始
    virtual void onStart() {}

    /// @brief 检测会话完成
    virtual void onComplete() {}

    /// @brief 会话关闭
    virtual void onClose() {}

    /// @brief 收到一次打分结果（含未命中的）
    virtual void onResult(const DetectionResult& result) = 0;

    /// @brief 命中关键词
    /// @param keyword 关键词文本
    /// @param score 得分
    /// @param timestamp_ms 关键词结束处的音频时间（毫秒）
    virtual void onWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) {}

    /// @brief 发生错误
    virtual void onError(const ErrorInfo& error) = 0;
};

// =============================================================================
// Lambda Callback Types
// =============================================================================

using OnStartCallback = std::function<void()>;
using OnCompleteCallback = std::function<void()>;
using OnCloseCallback = std::function<void()>;
using OnResultCallback = std::function<void(const DetectionResult&)>;
using OnWakeWordCallback = std::function<void(const std::string&, float, int64_t)>;
using OnErrorCallback = std::function<void(const ErrorInfo&)>;

// =============================================================================
// Lambda Callback Adapter
// =============================================================================

class LambdaCallback : public IKwsCallback {
public:
    class Builder {
    public:
        Builder& onStart(OnStartCallback cb) {
            on_start_ = std::move(cb);
            return *this;
        }

        Builder& onComplete(OnCompleteCallback cb) {
            on_complete_ = std::move(cb);
            return *this;
        }

        Builder& onClose(OnCloseCallback cb) {
            on_close_ = std::move(cb);
            return *this;
        }

        Builder& onResult(OnResultCallback cb) {
            on_result_ = std::move(cb);
            return *this;
        }

        Builder& onWakeWord(OnWakeWordCallback cb) {
            on_wake_word_ = std::move(cb);
            return *this;
        }

        Builder& onError(OnErrorCallback cb) {
            on_error_ = std::move(cb);
            return *this;
        }

        std::unique_ptr<LambdaCallback> build() {
            auto cb = std::make_unique<LambdaCallback>();
            cb->on_start_ = std::move(on_start_);
            cb->on_complete_ = std::move(on_complete_);
            cb->on_close_ = std::move(on_close_);
            cb->on_result_ = std::move(on_result_);
            cb->on_wake_word_ = std::move(on_wake_word_);
            cb->on_error_ = std::move(on_error_);
            return cb;
        }

    private:
        OnStartCallback on_start_;
        OnCompleteCallback on_complete_;
        OnCloseCallback on_close_;
        OnResultCallback on_result_;
        OnWakeWordCallback on_wake_word_;
        OnErrorCallback on_error_;
    };

    static Builder create() { return Builder(); }

    void onStart() override {
        if (on_start_) on_start_();
    }

    void onComplete() override {
        if (on_complete_) on_complete_();
    }

    void onClose() override {
        if (on_close_) on_close_();
    }

    void onResult(const DetectionResult& result) override {
        if (on_result_) on_result_(result);
    }

    void onWakeWord(const std::string& keyword, float score, int64_t ts) override {
        if (on_wake_word_) on_wake_word_(keyword, score, ts);
    }

    void onError(const ErrorInfo& error) override {
        if (on_error_) on_error_(error);
    }

private:
    friend class Builder;

    OnStartCallback on_start_;
    OnCompleteCallback on_complete_;
    OnCloseCallback on_close_;
    OnResultCallback on_result_;
    OnWakeWordCallback on_wake_word_;
    OnErrorCallback on_error_;
};

// =============================================================================
// Simple Callback (for testing)
// =============================================================================

class SimpleCallback : public IKwsCallback {
public:
    void onResult(const DetectionResult& result) override {
        last_result_ = result;
        has_result_ = true;
    }

    void onWakeWord(const std::string& keyword, float score, int64_t ts) override {
        wake_count_++;
        last_keyword_ = keyword;
        last_score_ = score;
        last_wake_ms_ = ts;
    }

    void onError(const ErrorInfo& error) override {
        last_error_ = error;
        has_error_ = true;
    }

    bool hasResult() const { return has_result_; }
    bool hasError() const { return has_error_; }
    int wakeCount() const { return wake_count_; }

    const DetectionResult& getLastResult() const { return last_result_; }
    const ErrorInfo& getLastError() const { return last_error_; }
    const std::string& getLastKeyword() const { return last_keyword_; }
    float getLastScore() const { return last_score_; }
    int64_t getLastWakeMs() const { return last_wake_ms_; }

    void reset() {
        has_result_ = false;
        has_error_ = false;
        wake_count_ = 0;
    }

private:
    DetectionResult last_result_;
    ErrorInfo last_error_;
    std::string last_keyword_;
    float last_score_ = 0.0f;
    int64_t last_wake_ms_ = -1;
    int wake_count_ = 0;
    bool has_result_ = false;
    bool has_error_ = false;
};

}  // namespace kws

#endif  // KWS_CALLBACK_HPP
