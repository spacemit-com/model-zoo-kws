/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fft.h"
#include <algorithm>
#include <cmath>

namespace kws::cfsmn {

void FFT::init(int size) {
    n = size;
    rev.resize(n);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev[i] = r;
    }
    cos_t.resize(n / 2);
    sin_t.resize(n / 2);
    for (int i = 0; i < n / 2; ++i) {
        cos_t[i] = std::cos(-2.0 * M_PI * i / n);
        sin_t[i] = std::sin(-2.0 * M_PI * i / n);
    }
}

void FFT::run(float *re, float *im) const {
    for (int i = 0; i < n; ++i)
        if (i < rev[i]) { std::swap(re[i], re[rev[i]]); std::swap(im[i], im[rev[i]]); }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                float wr = cos_t[j * step], wi = sin_t[j * step];
                float xr = re[i + j + half], xi = im[i + j + half];
                float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + j + half] = re[i + j] - tr;
                im[i + j + half] = im[i + j] - ti;
                re[i + j] += tr;
                im[i + j] += ti;
            }
        }
    }
}

}  // namespace kws::cfsmn
