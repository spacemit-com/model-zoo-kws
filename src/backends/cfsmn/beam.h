/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// Fixed MVDR beamformer over mics 2-4, WOLA in the STFT domain with the weights
// calibrated on the board (echo covariance from a playback-only session,
// steering from the quiet 3 m words).
#pragma once
#include <vector>
#include "fft.h"

namespace kws::cfsmn {

class Beamformer {
public:
    static constexpr int kN = 512, kHop = 160, kCh = 3;

    bool load(const char *path);
    void reset();
    // in: kHop frames of kCh channels, interleaved; out: kHop beamformed samples
    void process(const float *in, float *out);

private:
    FFT fft_;
    std::vector<float> w_, win_, xbuf_, ola_, nrm_, re_, im_, yr_, yi_;
};

}  // namespace kws::cfsmn
