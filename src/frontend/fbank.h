/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// Kaldi-compatible 80-bin log-mel fbank, matching the settings the model was
// trained with: 25 ms/10 ms, hamming, preemphasis 0.97, DC removal, power
// spectrum, 512-point FFT, low edge 20 Hz. Frames are produced incrementally.
#ifndef FBANK_H
#define FBANK_H

#include <vector>
#include "frontend/fft.h"

namespace kws::frontend {

class Fbank {
public:
    static constexpr int kWin = 400;    // 25 ms
    static constexpr int kShift = 160;  // 10 ms
    static constexpr int kPad = 512;    // round_to_power_of_two
    static constexpr int kBins = 80;

    void init();
    void reset();
    // consumes n samples (int16 scale) and appends one frame per completed window
    void feed(const float *x, int n, std::vector<std::vector<float>> &out);

private:
    void build_mel();
    void compute(const float *raw, std::vector<float> &out);

    FFT fft_;
    std::vector<float> window_, mel_, buf_, re_, im_, power_;
};

}  // namespace kws::frontend

#endif  // FBANK_H
