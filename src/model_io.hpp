/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MODEL_IO_HPP
#define MODEL_IO_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace kws {

inline uint32_t decodeLe32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline bool readLe32(FILE* f, uint32_t& value) {
    unsigned char bytes[4];
    if (fread(bytes, 1, sizeof(bytes), f) != sizeof(bytes)) return false;
    value = decodeLe32(bytes);
    return true;
}

// Legacy assets contain little-endian IEEE float32. Check the exact size before
// allocating, and inspect exponent bits (also safe in -ffast-math translation units).
inline bool readModelFloats(FILE* f, size_t count, std::vector<float>& out) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                    "KWS requires IEEE float32");
    constexpr size_t kMaxFloats = 16 * 1024 * 1024;
    const long begin = ftell(f);
    if (begin < 0 || count > kMaxFloats || fseek(f, 0, SEEK_END) != 0) return false;
    const long end = ftell(f);
    if (end < begin || static_cast<size_t>(end - begin) != count * 4 ||
        fseek(f, begin, SEEK_SET) != 0) return false;
    out.resize(count);
    if (fread(out.data(), 4, count, f) != count) return false;
    for (float& value : out) {
        const uint32_t bits = decodeLe32(reinterpret_cast<const unsigned char*>(&value));
        if ((bits & 0x7f800000U) == 0x7f800000U) return false;
        std::memcpy(&value, &bits, 4);
    }
    return true;
}

}  // namespace kws

#endif  // MODEL_IO_HPP
