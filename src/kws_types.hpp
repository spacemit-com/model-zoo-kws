/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Internal Types
 *
 * 内部类型定义，包括错误码、检测结果、音频块等。
 */

#ifndef KWS_TYPES_HPP
#define KWS_TYPES_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kws {

// =============================================================================
// Backend Type
// =============================================================================

enum class BackendType {
    CFSMN,    ///< 内置 cFSMN char-CTC 检测器（无外部推理引擎）
    CUSTOM,   ///< 自定义后端
};

inline const char* backendTypeToString(BackendType type) {
    switch (type) {
        case BackendType::CFSMN:  return "cfsmn";
        case BackendType::CUSTOM: return "custom";
        default:                  return "unknown";
    }
}

// =============================================================================
// Error Code
// =============================================================================

enum class ErrorCode {
    OK = 0,

    // 配置错误 (1xx)
    INVALID_CONFIG = 100,
    MODEL_NOT_FOUND = 101,
    UNSUPPORTED_SAMPLE_RATE = 102,
    INVALID_PARAMETER = 103,
    KEYWORD_NOT_FOUND = 104,

    // 运行时错误 (2xx)
    NOT_INITIALIZED = 200,
    ALREADY_INITIALIZED = 201,
    ALREADY_STARTED = 202,
    NOT_STARTED = 203,
    INFERENCE_FAILED = 204,

    // 内部错误 (4xx)
    INTERNAL_ERROR = 400,
    OUT_OF_MEMORY = 401,
    BACKEND_ERROR = 402,
};

// =============================================================================
// Error Info
// =============================================================================

struct ErrorInfo {
    ErrorCode code;
    std::string message;
    std::string detail;

    bool isOk() const { return code == ErrorCode::OK; }

    static ErrorInfo ok() {
        return {ErrorCode::OK, "", ""};
    }

    static ErrorInfo error(ErrorCode code, const std::string& msg,
                            const std::string& detail = "") {
        return {code, msg, detail};
    }
};

// =============================================================================
// Keyword
// =============================================================================

struct Keyword {
    std::string text;
    std::vector<int> token_ids;   ///< CTC 词表下标，例如 小进小进 = 1462,2428,1462,2428
    float threshold = 0.0f;       ///< 0 表示沿用全局阈值
};

// =============================================================================
// KWS Config (Internal)
// =============================================================================

struct KwsConfig {
    BackendType backend = BackendType::CFSMN;

    std::string model_dir;
    std::vector<Keyword> keywords;

    int sample_rate = 16000;
    int num_channels = 1;
    int frame_size = 160;

    bool use_beamforming = false;
    int beam_first_channel = 1;

    float threshold = 0.3f;
    int holdoff_ms = 2500;
    int decode_context = 86;
    int score_interval = 1;
    int num_threads = 1;

    std::map<std::string, std::string> extra_params;
};

// =============================================================================
// Detection Result
// =============================================================================

struct DetectionResult {
    float score = 0.0f;
    bool is_wake_word = false;      ///< 越过阈值且通过静默期
    int keyword_index = -1;         ///< 命中的关键词下标，未命中为 -1
    std::string keyword;

    int64_t timestamp_ms = 0;       ///< 已扣除模型前瞻的音频时间
    int processing_time_ms = 0;
};

// =============================================================================
// Audio Chunk
// =============================================================================

// data 为 [-1, 1] 的 float，按 num_channels 交织；num_samples 为单通道采样点数。
struct AudioChunk {
    const float* data = nullptr;
    size_t num_samples = 0;
    int num_channels = 1;
    int sample_rate = 16000;
    int64_t timestamp_ms = -1;

    static AudioChunk fromFloat(const float* data, size_t samples, int channels = 1,
                                int sample_rate = 16000, int64_t timestamp = -1) {
        return {data, samples, channels, sample_rate, timestamp};
    }

    static AudioChunk fromVector(const std::vector<float>& vec, int channels = 1,
                                int sample_rate = 16000, int64_t timestamp = -1) {
        return {vec.data(), vec.size() / (channels > 0 ? channels : 1), channels,
                sample_rate, timestamp};
    }

    bool isEmpty() const { return data == nullptr || num_samples == 0; }
};

}  // namespace kws

#endif  // KWS_TYPES_HPP
