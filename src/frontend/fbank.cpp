/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "frontend/fbank.h"
#include <algorithm>
#include <cmath>

namespace kws::frontend {

namespace {
constexpr float kPreemph = 0.97f;
constexpr float kEps = 1.1920928955078125e-07f;   // torch.finfo(float32).eps
inline float mel_scale(float f) { return 1127.0f * std::log(1.0f + f / 700.0f); }
}  // namespace

void Fbank::init() {
    fft_.init(kPad);
    window_.resize(kWin);
    for (int i = 0; i < kWin; ++i)   // hamming, periodic=False
        window_[i] = 0.54f - 0.46f * std::cos(2.0 * M_PI * i / (kWin - 1));
    build_mel();
    re_.resize(kPad);
    im_.resize(kPad);
    power_.resize(kPad / 2 + 1);
}

void Fbank::reset() { buf_.clear(); }

void Fbank::build_mel() {
    const int nfft = kPad / 2;          // 256; one zero column is appended
    const float fs = 16000.0f, low = 20.0f, high = 8000.0f;
    const float bin_w = fs / kPad;
    const float mlo = mel_scale(low), mhi = mel_scale(high);
    const float d = (mhi - mlo) / (kBins + 1);
    mel_.assign((size_t)kBins * (nfft + 1), 0.0f);
    for (int b = 0; b < kBins; ++b) {
        const float l = mlo + b * d, c = l + d, r = c + d;
        for (int i = 0; i < nfft; ++i) {
            const float m = mel_scale(bin_w * i);
            const float up = (m - l) / (c - l), down = (r - m) / (r - c);
            const float v = std::min(up, down);
            mel_[(size_t)b * (nfft + 1) + i] = v > 0 ? v : 0.0f;
        }
    }
}

void Fbank::compute(const float *raw, std::vector<float> &out) {
    float frame[kWin];
    double mean = 0;
    for (int i = 0; i < kWin; ++i) mean += raw[i];
    mean /= kWin;
    for (int i = 0; i < kWin; ++i) frame[i] = raw[i] - (float)mean;   // remove_dc_offset
    float prev = frame[0];                                            // replicate padding
    for (int i = 0; i < kWin; ++i) {                                  // preemphasis
        const float cur = frame[i];
        frame[i] = cur - kPreemph * prev;
        prev = cur;
    }
    std::fill(re_.begin(), re_.end(), 0.0f);
    std::fill(im_.begin(), im_.end(), 0.0f);
    for (int i = 0; i < kWin; ++i) re_[i] = frame[i] * window_[i];
    fft_.run(re_.data(), im_.data());
    for (int i = 0; i <= kPad / 2; ++i)
        power_[i] = re_[i] * re_[i] + im_[i] * im_[i];                // use_power
    out.resize(kBins);
    const int nfft = kPad / 2;
    for (int b = 0; b < kBins; ++b) {
        const float *w = &mel_[(size_t)b * (nfft + 1)];
        float s = 0;
        for (int i = 0; i < nfft; ++i) s += w[i] * power_[i];
        out[b] = std::log(std::max(s, kEps));
    }
}

void Fbank::feed(const float *x, int n, std::vector<std::vector<float>> &out) {
    buf_.insert(buf_.end(), x, x + n);
    while ((int)buf_.size() >= kWin) {
        out.emplace_back();
        compute(buf_.data(), out.back());
        buf_.erase(buf_.begin(), buf_.begin() + kShift);
    }
}

}  // namespace kws::frontend
