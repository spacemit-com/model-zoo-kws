/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cFSMN char-CTC 唤醒后端
 *
 * 链路：可选 3 麦固定波束 → kaldi fbank → 上下文拼接/抽帧 → 流式 cFSMN → CTC 关键词搜索。
 * 全部手写，不依赖任何推理引擎或 BLAS；K3 上约占单核 2.7%。
 */

#ifndef CFSMN_BACKEND_HPP
#define CFSMN_BACKEND_HPP

#include <string>
#include <vector>

#include "backends/kws_backend.hpp"
#include "frontend/beam.h"
#include "frontend/fbank.h"

#include "ctc.h"
#include "fsmn.h"

namespace kws {

class CfsmnBackend : public IKwsBackend {
public:
    static constexpr int kHop = 160;          ///< 10 ms at 16 kHz
    static constexpr int kFrameMs = 30;       ///< 一个模型帧 = 3 个 fbank 帧

    ErrorInfo initialize(const KwsConfig& config) override;
    void shutdown() override;
    bool isInitialized() const override { return initialized_; }

    BackendType getType() const override { return BackendType::CFSMN; }
    std::string getName() const override { return "cFSMN char-CTC"; }
    int getRecommendedFrameSize() const override { return kHop; }
    int getLookaheadMs() const override { return model_.lookahead() * kFrameMs; }
    std::vector<std::string> getKeywords() const override;

    ErrorInfo process(const AudioChunk& audio,
                        std::vector<DetectionResult>& results) override;
    void reset() override;
    ErrorInfo setThreshold(float threshold) override;

private:
    // 一个关键词的解码状态：受限 token 集、每帧候选环、静默期。
    struct Slot {
        std::string text;
        float threshold = 0.3f;
        double last_fire = -1e9;
        cfsmn::CtcKeywordDecoder decoder;
        std::vector<cfsmn::FrameCand> ring, order;
    };

    ErrorInfo loadKeywords(const KwsConfig& config, const std::string& model_dir);
    void processHop(const float* interleaved, std::vector<DetectionResult>& results);
    void onFbankFrame(const std::vector<float>& frame, std::vector<DetectionResult>& results);
    void scoreFrame(std::vector<DetectionResult>& results);

    bool initialized_ = false;
    int source_channel_ = 0;        ///< 不做波束时取哪一路
    cfsmn::Model model_;
    frontend::Beamformer beam_;
    frontend::Fbank fbank_;
    cfsmn::Stream stream_;
    std::vector<Slot> slots_;

    std::vector<float> pending_;                    ///< 未满一个 hop 的输入（交织）
    std::vector<float> chan_, beamed_, feat_, logits_;   ///< chan_: 送进波束的 3 路交织
    std::vector<std::vector<float>> fb_new_, fb_hist_;
    std::vector<long> fb_idx_;
    long fb_next_ = 0, nposts_ = 0, model_frames_ = 0;
    double audio_time_ = 0.0;
};

}  // namespace kws

#endif  // CFSMN_BACKEND_HPP
