/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// cFSMN char-CTC forward. Two modes: whole-window recompute (for checking
// against PyTorch) and frame-by-frame with the FSMN memory carried across
// calls - the `in_cache` that the ModelScope python declares but never wires up.
#pragma once
#include <vector>

namespace kws::cfsmn {

struct Layer { const float *pw, *cl, *cr, *aw, *ab; };

struct Model {
    int idim = 0, a1 = 0, ldim = 0, pdim = 0, lorder = 0, rorder = 0, layers = 0, a2 = 0, odim = 0;
    std::vector<float> blob;
    const float *mean = nullptr, *istd = nullptr, *in1w = nullptr, *in1b = nullptr,
                *in2w = nullptr, *in2b = nullptr, *o1w = nullptr, *o1b = nullptr,
                *o2w = nullptr, *o2b = nullptr;
    std::vector<Layer> L;

    bool load(const char *path);
    int lookahead() const { return layers * rorder; }   // frames of pipeline delay
};

void forward_window(const Model &m, const float *X, int T, float *logits);

// Ring of the last lorder+rorder frames of one layer's projected input.
// Pushing frame t lets us finish frame t-rorder, whose right context just arrived.
struct MemConv {
    int pdim = 0, lorder = 0, rorder = 0, ring = 0;
    long t = 0;
    std::vector<float> buf;

    void init(int pdim, int lorder, int rorder);
    bool push(const float *x, const Layer &ly, float *out);
};

struct Stream {
    const Model *m = nullptr;
    std::vector<MemConv> st;
    std::vector<float> ti, ta, h, p, o;

    void init(const Model &m);
    bool push(const float *feat, float *logits);   // false while the pipeline fills
};

}  // namespace kws::cfsmn
