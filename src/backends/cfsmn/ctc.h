/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// CTC prefix beam search restricted to the keyword tokens, ported from
// modelscope's kws_utils.batch_utils so the scores stay comparable with the
// python pipeline, with the terminal keyword alignment included.
#ifndef CTC_H
#define CTC_H

#include <vector>

namespace kws::cfsmn {

struct CtcNode { int token; int frame; float prob; };

// Keyword tokens that have rivals (tokens of confusable words), at most this many.
constexpr int kMaxRivalGroups = 4;

// Per frame only the top-3 posteriors above 0.05 that belong to the keyword
// token set can matter, so that filtering happens once, when the frame is
// produced, instead of on every decode call. rival[g] is the best posterior
// among the rivals of rival group g, whatever its rank.
struct FrameCand {
    int n = 0;
    int idx[3];
    float prob[3];
    float rival[kMaxRivalGroups] = {};
    int rival_idx[kMaxRivalGroups] = {};
};

// A keyword token beaten by one of its rivals at a node of a detection.
struct Rivalry { int token = -1, rival = -1, frame = -1; float prob = 0, rival_prob = 0; };

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
    // Confusable words (keywords.txt "!小姐小姐"): where a word of the keyword's
    // length differs from it, its token rivals the keyword token. False when the
    // rivals span more than kMaxRivalGroups distinct keyword tokens.
    bool set_rivals(const std::vector<std::vector<int>> &words);
    const std::vector<int> &tokenset() const { return tokenset_; }

    void candidates(const float *logits, int n, FrameCand &out) const;
    float score(const FrameCand *frames, int T, int *end_frame = nullptr);
    // Earliest occurrence ending after min_end with score >= threshold.
    float score(const FrameCand *frames, int T, float threshold, int min_end, int *end_frame);
    // Best keyword prefix or suffix of min_tokens..K-1 tokens in the beam of the
    // last score() call, ending in (min_end, last_end]. Scored on the full-match
    // scale, (prod p)^(K / 2n). *kw_start/*len locate it inside the keyword;
    // *end_frame is -1 when there is none.
    float partial(int min_tokens, int min_end, int last_end, int *end_frame, int *kw_start,
            int *len);
    // Nodes of the occurrence last reported by score() (at or above its threshold)
    // or partial() where a rival beats the keyword token within one frame, i.e. the
    // confusable word fits that stretch better; *why describes the clearest one and
    // *contested counts the nodes that have rivals at all.
    int rivaled(const FrameCand *frames, int T, Rivalry *why, int *contested = nullptr) const;

private:
    struct RivalGroup { int token; std::vector<int> ids; };

    Hyp &slot(const std::vector<int> &prefix);

    std::vector<int> kw_, tokenset_;
    std::vector<RivalGroup> rivals_;
    std::vector<CtcNode> hit_;
    std::vector<Hyp> cur_, next_;
};

}  // namespace kws::cfsmn

#endif  // CTC_H
