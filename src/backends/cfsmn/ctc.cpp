/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ctc.h"
#include <algorithm>
#include <cmath>

namespace kws::cfsmn {

int is_sublist(const std::vector<int> &main_list, const std::vector<int> &check) {
    if (check.empty() || main_list.size() < check.size()) return -1;
    if (main_list.size() == check.size()) return main_list == check ? 0 : -1;
    for (size_t i = 0; i + check.size() <= main_list.size(); ++i) {
        if (main_list[i] != check[0]) continue;
        bool all = true;
        for (size_t j = 0; j < check.size(); ++j)
            if (main_list[i + j] != check[j]) { all = false; break; }
        if (all) return (int)i;
    }
    return -1;
}

void CtcKeywordDecoder::set_keyword(const std::vector<int> &ids) {
    kw_ = ids;
    tokenset_ = {0};                       // blank is always allowed
    for (int id : ids)
        if (std::find(tokenset_.begin(), tokenset_.end(), id) == tokenset_.end())
            tokenset_.push_back(id);
}

bool CtcKeywordDecoder::set_rivals(const std::vector<std::vector<int>> &words) {
    rivals_.clear();
    for (const std::vector<int> &w : words) {
        if (w.size() != kw_.size()) continue;
        for (size_t i = 0; i < w.size(); ++i) {
            if (w[i] == kw_[i]) continue;
            auto g = std::find_if(rivals_.begin(), rivals_.end(),
                    [&](const RivalGroup &r) { return r.token == kw_[i]; });
            if (g == rivals_.end()) {
                if ((int)rivals_.size() == kMaxRivalGroups) return false;
                rivals_.push_back(RivalGroup{kw_[i], {}});
                g = rivals_.end() - 1;
            }
            if (std::find(g->ids.begin(), g->ids.end(), w[i]) == g->ids.end()) g->ids.push_back(w[i]);
        }
    }
    return true;
}

void CtcKeywordDecoder::candidates(const float *logits, int n, FrameCand &out) const {
    float mx = logits[0];
    for (int i = 1; i < n; ++i) if (logits[i] > mx) mx = logits[i];
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += std::exp((double)logits[i] - mx);
    int bi[3] = {-1, -1, -1};
    float bv[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < n; ++i) {          // top-3 over the full vocabulary first
        const float v = logits[i];
        if (v > bv[0]) { bv[2] = bv[1]; bi[2] = bi[1]; bv[1] = bv[0]; bi[1] = bi[0]; bv[0] = v; bi[0] = i; }
        else if (v > bv[1]) { bv[2] = bv[1]; bi[2] = bi[1]; bv[1] = v; bi[1] = i; }
        else if (v > bv[2]) { bv[2] = v; bi[2] = i; }
    }
    out.n = 0;
    for (int k = 0; k < 3; ++k) {
        if (bi[k] < 0) continue;
        const float p = (float)(std::exp((double)bv[k] - mx) / sum);
        if (p <= 0.05f) continue;
        if (std::find(tokenset_.begin(), tokenset_.end(), bi[k]) == tokenset_.end()) continue;
        out.idx[out.n] = bi[k];
        out.prob[out.n] = p;
        ++out.n;
    }
    for (size_t g = 0; g < rivals_.size(); ++g) {
        out.rival[g] = 0.0f;
        out.rival_idx[g] = -1;
        for (int id : rivals_[g].ids) {
            const float p = (float)(std::exp((double)logits[id] - mx) / sum);
            if (p > out.rival[g]) { out.rival[g] = p; out.rival_idx[g] = id; }
        }
    }
}

Hyp &CtcKeywordDecoder::slot(const std::vector<int> &prefix) {
    for (Hyp &h : next_) if (h.prefix == prefix) return h;
    next_.push_back(Hyp{prefix, 0.0, 0.0, {}});
    return next_.back();
}

float CtcKeywordDecoder::score(const FrameCand *frames, int T, int *end_frame) {
    return score(frames, T, 0.0f, -1, end_frame);
}

float CtcKeywordDecoder::score(const FrameCand *frames, int T, float threshold, int min_end,
        int *end_frame) {
    if (end_frame) *end_frame = -1;
    hit_.clear();
    if (kw_.empty() || T <= 0) return 0.0f;
    cur_.clear();
    cur_.push_back(Hyp{{}, 1.0, 0.0, {}});
    for (int t = 0; t < T; ++t) {
        const FrameCand &c = frames[t];
        if (c.n == 0) continue;
        next_.clear();
        for (int k = 0; k < c.n; ++k) {
            const int s = c.idx[k];
            const double ps = c.prob[k];
            for (const Hyp &e : cur_) {
                const int last = e.prefix.empty() ? -1 : e.prefix.back();
                if (s == 0) {                                   // blank extends the prefix
                    Hyp &n = slot(e.prefix);
                    n.pb += e.pb * ps + e.pnb * ps;
                    n.nodes = e.nodes;
                } else if (s == last) {                         // repeat
                    if (std::fabs(e.pnb) > 1e-6) {              // *ss -> *s
                        Hyp &n = slot(e.prefix);
                        n.pnb += e.pnb * ps;
                        n.nodes = e.nodes;
                        if (!n.nodes.empty() && ps > n.nodes.back().prob) {
                            n.nodes.back().prob = (float)ps;
                            n.nodes.back().frame = t;
                        }
                    }
                    if (std::fabs(e.pb) > 1e-6) {               // *s-s -> *ss
                        std::vector<int> np = e.prefix;
                        np.push_back(s);
                        Hyp &n = slot(np);
                        n.pnb += e.pb * ps;
                        n.nodes = e.nodes;
                        n.nodes.push_back(CtcNode{s, t, (float)ps});
                    }
                } else {                                        // new token
                    std::vector<int> np = e.prefix;
                    np.push_back(s);
                    Hyp &n = slot(np);
                    if (!n.nodes.empty()) {
                        if (ps > n.nodes.back().prob) {
                            n.nodes.back().prob = (float)ps;
                            n.nodes.back().frame = t;
                        }
                    } else {
                        n.nodes = e.nodes;
                        n.nodes.push_back(CtcNode{s, t, (float)ps});
                    }
                    n.pnb += e.pb * ps + e.pnb * ps;
                }
            }
        }
        std::sort(next_.begin(), next_.end(), [](const Hyp &a, const Hyp &b) {
            return (a.pb + a.pnb) > (b.pb + b.pnb);
        });
        if ((int)next_.size() > path_beam) next_.resize(path_beam);
        cur_ = next_;
    }
    // Every occurrence is scored on its own: a weak earlier call in the window
    // must not mask a later one until it slides out. The earliest occurrence
    // that ends after min_end and reaches the threshold wins; otherwise the
    // best sub-threshold score is reported with end_frame = -1.
    float below = 0.0f;
    for (const Hyp &hyp : cur_) {
        const size_t n = std::min(hyp.prefix.size(), hyp.nodes.size());
        for (size_t off = 0; off + kw_.size() <= n; ++off) {
            if (!std::equal(kw_.begin(), kw_.end(), hyp.prefix.begin() + off)) continue;
            const int end = hyp.nodes[off + kw_.size() - 1].frame;
            if (end <= min_end) continue;
            double p = 1.0;
            for (size_t i = off; i < off + kw_.size(); ++i) p *= hyp.nodes[i].prob;
            const float s = (float)std::sqrt(p);
            if (s >= threshold) {
                if (end_frame) *end_frame = end;
                hit_.assign(hyp.nodes.begin() + off, hyp.nodes.begin() + off + kw_.size());
                return s;
            }
            below = std::max(below, s);
        }
    }
    return below;
}

float CtcKeywordDecoder::partial(int min_tokens, int min_end, int last_end, int *end_frame,
        int *kw_start, int *len) {
    *end_frame = -1;
    *kw_start = *len = 0;
    hit_.clear();
    const int K = (int)kw_.size();
    float best = 0.0f;
    for (const Hyp &hyp : cur_) {
        const int nodes = (int)std::min(hyp.prefix.size(), hyp.nodes.size());
        for (int n = K - 1; n >= std::max(1, min_tokens); --n) {
            for (int ks : {0, K - n}) {                          // keyword prefix, then suffix
                for (int off = 0; off + n <= nodes; ++off) {
                    if (!std::equal(kw_.begin() + ks, kw_.begin() + ks + n, hyp.prefix.begin() + off))
                        continue;
                    const int end = hyp.nodes[off + n - 1].frame;
                    if (end <= min_end || end > last_end) continue;
                    double p = 1.0;
                    for (int i = off; i < off + n; ++i) p *= hyp.nodes[i].prob;
                    const float s = (float)std::pow(p, (double)K / (2.0 * n));
                    if (s > best) {
                        best = s;
                        *end_frame = end;
                        *kw_start = ks;
                        *len = n;
                        hit_.assign(hyp.nodes.begin() + off, hyp.nodes.begin() + off + n);
                    }
                }
            }
        }
    }
    return best;
}

int CtcKeywordDecoder::rivaled(const FrameCand *frames, int T, Rivalry *why, int *contested) const {
    int beaten = 0, nodes = 0;
    float clearest = 0.0f;
    for (const CtcNode &node : hit_) {
        for (size_t g = 0; g < rivals_.size(); ++g) {
            if (rivals_[g].token != node.token) continue;
            ++nodes;
            int best = -1;
            for (int f = std::max(0, node.frame - 1); f <= std::min(T - 1, node.frame + 1); ++f)
                if (frames[f].rival[g] > node.prob && (best < 0 || frames[f].rival[g] > frames[best].rival[g]))
                    best = f;
            if (best < 0) continue;
            ++beaten;
            const float margin = frames[best].rival[g] - node.prob;
            if (margin > clearest) {
                clearest = margin;
                if (why) *why = Rivalry{node.token, frames[best].rival_idx[g], best, node.prob, frames[best].rival[g]};
            }
        }
    }
    if (contested) *contested = nodes;
    return beaten;
}

}  // namespace kws::cfsmn
