/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CLI_UTILS_HPP
#define CLI_UTILS_HPP

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

inline float parseThreshold(const std::string& value) {
    char* end = nullptr;
    errno = 0;
    const float number = std::strtof(value.c_str(), &end);
    if (value.empty() || *end || errno || !std::isfinite(number) || number < 0 || number > 1) {
        fprintf(stderr, "--thr expects a finite number in [0, 1]\n");
        std::exit(1);
    }
    return number;
}

inline int parseChannels(const std::string& value) {
    char* end = nullptr;
    errno = 0;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end || errno || number < 1 || number > 64) {
        fprintf(stderr, "--channels expects an integer in [1, 64]\n");
        std::exit(1);
    }
    return static_cast<int>(number);
}

#endif  // CLI_UTILS_HPP
