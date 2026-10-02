/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// Minimal iterative radix-2 complex FFT. 512 points at 100 Hz costs nothing,
// so there is no reason to pull in a library.
#ifndef FFT_H
#define FFT_H

#include <vector>

namespace kws::frontend {

struct FFT {
    int n = 0;
    std::vector<int> rev;
    std::vector<float> cos_t, sin_t;

    void init(int size);
    void run(float *re, float *im) const;   // in place; inverse = conjugate around the call
};

}  // namespace kws::frontend

#endif  // FFT_H
