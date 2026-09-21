/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frontend/beam.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace kws::frontend {

bool Beamformer::load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    int nbin = 0, nch = 0;
    if (fread(&nbin, 4, 1, f) != 1 || fread(&nch, 4, 1, f) != 1) { fclose(f); return false; }
    if (nbin != kN / 2 + 1 || nch != kCh) {
        fprintf(stderr, "beam weights are %dx%d, expected %dx%d\n", nbin, nch, kN / 2 + 1, kCh);
        fclose(f);
        return false;
    }
    w_.resize((size_t)nbin * nch * 2);
    const bool ok = fread(w_.data(), sizeof(float), w_.size(), f) == w_.size();
    fclose(f);
    fft_.init(kN);
    win_.resize(kN);
    for (int i = 0; i < kN; ++i)   // np.hanning: symmetric
        win_[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / (kN - 1));
    xbuf_.assign((size_t)kN * kCh, 0.0f);
    ola_.assign(kN, 0.0f);
    nrm_.assign(kN, 0.0f);
    re_.resize(kN); im_.resize(kN);
    yr_.resize(kN); yi_.resize(kN);
    return ok;
}

void Beamformer::reset() {
    std::fill(xbuf_.begin(), xbuf_.end(), 0.0f);
    std::fill(ola_.begin(), ola_.end(), 0.0f);
    std::fill(nrm_.begin(), nrm_.end(), 0.0f);
}

void Beamformer::process(const float *in, float *out) {
    const int keep = kN - kHop;
    for (int c = 0; c < kCh; ++c) {
        float *col = &xbuf_[(size_t)c * kN];
        memmove(col, col + kHop, sizeof(float) * keep);
        for (int i = 0; i < kHop; ++i) col[keep + i] = in[i * kCh + c];
    }
    std::fill(yr_.begin(), yr_.end(), 0.0f);
    std::fill(yi_.begin(), yi_.end(), 0.0f);
    for (int c = 0; c < kCh; ++c) {
        const float *col = &xbuf_[(size_t)c * kN];
        for (int i = 0; i < kN; ++i) { re_[i] = col[i] * win_[i]; im_[i] = 0.0f; }
        fft_.run(re_.data(), im_.data());
        for (int b = 0; b <= kN / 2; ++b) {          // multiply by conj(w)
            const float wr = w_[((size_t)b * kCh + c) * 2], wi = w_[((size_t)b * kCh + c) * 2 + 1];
            yr_[b] += re_[b] * wr + im_[b] * wi;
            yi_[b] += im_[b] * wr - re_[b] * wi;
        }
    }
    for (int b = kN / 2 + 1; b < kN; ++b) {          // hermitian mirror
        yr_[b] = yr_[kN - b];
        yi_[b] = -yi_[kN - b];
    }
    for (int i = 0; i < kN; ++i) yi_[i] = -yi_[i];   // inverse via conjugation
    fft_.run(yr_.data(), yi_.data());
    const float inv = 1.0f / kN;
    for (int i = 0; i < kN - kHop; ++i) { ola_[i] = ola_[i + kHop]; nrm_[i] = nrm_[i + kHop]; }
    for (int i = kN - kHop; i < kN; ++i) { ola_[i] = 0.0f; nrm_[i] = 0.0f; }
    for (int i = 0; i < kN; ++i) {
        ola_[i] += yr_[i] * inv * win_[i];
        nrm_[i] += win_[i] * win_[i];
    }
    for (int i = 0; i < kHop; ++i)
        out[i] = ola_[i] / std::max(nrm_[i], 1e-3f);
}

}  // namespace kws::frontend
