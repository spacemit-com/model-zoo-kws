/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ctc.h"
#include <algorithm>
#include <cmath>

namespace kws::cfsmn {

int is_sublist(const std::vector<int> &main_list, const std::vector<int> &check) {
    if (main_list.size() < check.size()) return -1;
    if (main_list.size() == check.size()) return main_list == check ? 0 : -1;
    for (size_t i = 0; i + check.size() < main_list.size(); ++i) {
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
}

Hyp &CtcKeywordDecoder::slot(const std::vector<int> &prefix) {
    for (Hyp &h : next_) if (h.prefix == prefix) return h;
    next_.push_back(Hyp{prefix, 0.0, 0.0, {}});
    return next_.back();
}

float CtcKeywordDecoder::score(const FrameCand *frames, int T) {
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
    for (const Hyp &hyp : cur_) {
        const int off = is_sublist(hyp.prefix, kw_);
        if (off < 0) continue;
        double p = 1.0;
        for (size_t i = off; i < off + kw_.size() && i < hyp.nodes.size(); ++i)
            p *= hyp.nodes[i].prob;
        return (float)std::sqrt(p);
    }
    return 0.0f;
}

}  // namespace kws::cfsmn
