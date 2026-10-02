/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WAV_READER_HPP
#define WAV_READER_HPP
#include <cstdio>
#include <cstring>
#include <vector>
#include "model_io.hpp"
namespace kws_demo {
struct Wav {
    std::vector<float> samples;   // 交织，[-1, 1]
    int channels = 1;
    int sample_rate = 16000;
};

inline bool readWav(const char* path, Wav& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    unsigned char header[12];
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    const long file_size = ftell(f);
    rewind(f);
    if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
        static_cast<uint64_t>(kws::decodeLe32(header + 4)) + 8 != static_cast<uint64_t>(file_size)) {
        fclose(f);
        fprintf(stderr, "%s: expected a complete RIFF/WAVE file\n", path);
        return false;
    }
    char id[4];
    uint32_t size = 0;
    bool have_fmt = false;
    while (fread(id, 1, 4, f) == 4 && kws::readLe32(f, size)) {
        const long begin = ftell(f);
        if (begin < 0 || static_cast<uint64_t>(begin) + size + (size & 1U) > (uint64_t)file_size) break;
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fmt[16] = {0};
            if (size < 16 || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) break;
            out.channels = fmt[2] | (fmt[3] << 8);
            out.sample_rate = static_cast<int>(kws::decodeLe32(fmt + 4));
            if ((fmt[0] | (fmt[1] << 8)) != 1 || (fmt[14] | (fmt[15] << 8)) != 16 ||
                out.channels < 1 || out.channels > 64 || out.sample_rate != 16000 ||
                (fmt[12] | (fmt[13] << 8)) != out.channels * 2 ||
                kws::decodeLe32(fmt + 8) != (uint32_t)(16000 * out.channels * 2)) break;
            have_fmt = true;
        } else if (!memcmp(id, "data", 4)) {
            if (!have_fmt || size == 0 || size % (out.channels * 2) != 0) break;
            std::vector<unsigned char> pcm(size);
            if (fread(pcm.data(), 1, size, f) != size) break;
            out.samples.resize(size / 2);
            for (size_t i = 0; i < out.samples.size(); ++i) {
                const int value = pcm[2 * i] | (pcm[2 * i + 1] << 8);
                out.samples[i] = (value >= 32768 ? value - 65536 : value) / 32768.0f;
            }
            fclose(f);
            return true;
        }
        if (fseek(f, begin + size + (size & 1U), SEEK_SET) != 0) break;
    }
    fclose(f);
    fprintf(stderr, "%s: expected 16 kHz PCM16 WAV with complete fmt/data chunks\n", path);
    return false;
}

}  // namespace kws_demo
#endif  // WAV_READER_HPP
