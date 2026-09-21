/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// CTC prefix beam search restricted to the keyword tokens, ported from
// modelscope's kws_utils.batch_utils so the scores stay comparable with the
// python pipeline - including its quirks (node-overwrite rules, and is_sublist
// skipping the last possible alignment).
#pragma once
#include <vector>

namespace kws::cfsmn {

struct CtcNode { int token; int frame; float prob; };

// Per frame only the top-3 posteriors above 0.05 that belong to the keyword
// token set can matter, so that filtering happens once, when the frame is
// produced, instead of on every decode call.
struct FrameCand {
    int n = 0;
    int idx[3];
    float prob[3];
};

struct Hyp {
    std::vector<int> prefix;
    double pb = 0, pnb = 0;
    std::vector<CtcNode> nodes;
};

int is_sublist(const std::vector<int> &main_list, const std::vector<int> &check);

class CtcKeywordDecoder {
public:
    int path_beam = 20;

    void set_keyword(const std::vector<int> &ids);
    const std::vector<int> &keyword() const { return kw_; }
    const std::vector<int> &tokenset() const { return tokenset_; }

    void candidates(const float *logits, int n, FrameCand &out) const;
    float score(const FrameCand *frames, int T);   // 0 = keyword not found

private:
    Hyp &slot(const std::vector<int> &prefix);

    std::vector<int> kw_, tokenset_;
    std::vector<Hyp> cur_, next_;
};

}  // namespace kws::cfsmn
