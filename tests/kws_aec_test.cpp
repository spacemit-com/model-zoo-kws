/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "kws_aec_audio.hpp"

static void require(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
    kws_demo::WebRtcAec invalid(4, 2, 3, 50);
    require(!invalid.Initialize(), "invalid channel mapping must fail");
    kws_demo::WebRtcAec aec(4, 1, 3, 50);
    require(aec.Initialize(), aec.Error().c_str());
    std::array<float, 160> reference{};
    std::array<float, 640> capture{}, output{};
    std::vector<float> history(16000 * 8 + 160, 0);
    unsigned int random = 7;
    double before = 0, after = 0;
    for (int block = 0; block < 800; ++block) {
        for (int i = 0; i < 160; ++i) {
            const int t = block * 160 + i;
            random = 1664525U * random + 1013904223U;
            reference[i] = (static_cast<int>(random >> 16) - 32768) / 32768.0f * 0.12f;
            history[t] = reference[i];
            capture[i * 4] = 0.123f;  // Unconsumed channel must be unchanged.
            for (int c = 1; c < 4; ++c) {
                const int delay = 640 + c * 23;
                capture[i * 4 + c] = t >= delay ? history[t - delay] * (0.6f + 0.1f * c) : 0;
            }
        }
        require(aec.Process(capture.data(), reference.data(), output.data()), aec.Error().c_str());
        for (int i = 0; i < 160; ++i) {
            require(output[i * 4] == capture[i * 4], "AEC changed the unused channel");
            for (int c = 1; c < 4; ++c) {
                require(std::isfinite(output[i * 4 + c]), "AEC produced nonfinite samples");
                if (block >= 500) {
                    before += capture[i * 4 + c] * capture[i * 4 + c];
                    after += output[i * 4 + c] * output[i * 4 + c];
                }
            }
        }
    }
    const double reduction = 10 * std::log10(before / std::max(after, 1e-20));
    printf("Synthetic delayed-echo reduction: %.2f dB\n", reduction);
    require(reduction > 10, "AEC did not suppress a known delayed echo");
    // A newly initialized filter must pass speech when its playback reference is silent.
    kws_demo::WebRtcAec near(1, 0, 1, 50);
    require(near.Initialize(), near.Error().c_str());
    reference.fill(0);
    before = after = 0;
    for (int block = 0; block < 100; ++block) {
        for (int i = 0; i < 160; ++i) capture[i] = 0.1f * std::sin((block * 160 + i) * 0.17f);
        require(near.Process(capture.data(), reference.data(), output.data()), near.Error().c_str());
        if (block > 20) {
            for (int i = 0; i < 160; ++i) {
                before += capture[i] * capture[i];
                after += output[i] * output[i];
            }
        }
    }
    require(after > before * 0.5 && after < before * 2, "silent-reference AEC muted or amplified near audio");
    puts("AEC regression: PASS");
}
