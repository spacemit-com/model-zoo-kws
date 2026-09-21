/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS File Demo
 *
 * 回放一个 16 kHz wav，按 10 ms 一帧喂给引擎，打印唤醒事件与 CPU 占用。
 * 4 通道文件（SPV 复合设备的录音）默认走 3 麦波束。
 *
 *   kws_file_demo --wav play-3m.wav
 *   kws_file_demo --wav mono.wav --no-beam --thr 0.4
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "kws_service.h"

namespace {

struct Wav {
    std::vector<float> samples;   // 交织，[-1, 1]
    int channels = 1;
    int sample_rate = 16000;
};

bool readWav(const char* path, Wav& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    char id[4];
    unsigned size = 0;
    bool have_fmt = false;
    fseek(f, 12, SEEK_SET);
    while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fmt[16] = {0};
            if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) break;
            out.channels = fmt[2] | (fmt[3] << 8);
            out.sample_rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            have_fmt = true;
            if (size > sizeof(fmt)) fseek(f, size - sizeof(fmt), SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            std::vector<short> pcm(size / 2);
            if (fread(pcm.data(), 2, pcm.size(), f) != pcm.size()) break;
            out.samples.resize(pcm.size());
            for (size_t i = 0; i < pcm.size(); ++i) out.samples[i] = pcm[i] / 32768.0f;
            fclose(f);
            return have_fmt && !out.samples.empty();
        } else {
            fseek(f, size, SEEK_CUR);
        }
    }
    fclose(f);
    fprintf(stderr, "%s: no usable fmt/data chunk\n", path);
    return false;
}

class DemoCallback : public SpacemiT::KwsEngineCallback {
public:
    explicit DemoCallback(bool quiet) : quiet_(quiet) {}

    void OnEvent(std::shared_ptr<SpacemiT::KwsResult> result) override {
        if (!result->IsWakeWord() && !quiet_ && result->GetScore() > 0.02f) {
            printf("       score %.3f  (t=%.1fs)\n", result->GetScore(),
                    result->GetTimestampMs() / 1000.0);
        }
    }

    void OnWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) override {
        wakes++;
        printf("[WAKE] %s  score %.3f  (t=%.1fs)\n", keyword.c_str(), score,
                timestamp_ms / 1000.0);
        fflush(stdout);
    }

    void OnError(const std::string& message) override {
        fprintf(stderr, "error: %s\n", message.c_str());
    }

    int wakes = 0;

private:
    bool quiet_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string wav_path, model_dir, keyword, preset;
    float threshold = -1.0f;
    bool no_beam = false, quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--wav") wav_path = val();
        else if (k == "--model-dir") model_dir = val();
        else if (k == "--keyword") keyword = val();
        else if (k == "--preset") preset = val();
        else if (k == "--thr") threshold = atof(val().c_str());
        else if (k == "--no-beam") no_beam = true;
        else if (k == "--quiet") quiet = true;
        else {
            fprintf(stderr,
                    "usage: kws_file_demo --wav FILE [--model-dir DIR] [--preset NAME]\n"
                    "                     [--keyword TEXT] [--thr 0.3] [--no-beam] [--quiet]\n");
            return k == "--help" ? 0 : 1;
        }
    }
    if (wav_path.empty()) {
        fprintf(stderr, "need --wav FILE\n");
        return 1;
    }

    Wav wav;
    if (!readWav(wav_path.c_str(), wav)) return 1;
    if (wav.sample_rate != 16000) {
        fprintf(stderr, "%s: %d Hz, the model needs 16000\n", wav_path.c_str(), wav.sample_rate);
        return 1;
    }

    const bool use_beam = !no_beam && wav.channels >= 4;
    auto config = SpacemiT::KwsConfig::Preset(
        !preset.empty() ? preset : (use_beam ? "xiaojin-4mic" : "xiaojin"));
    config.num_channels = wav.channels;
    config.use_beamforming = use_beam;
    if (!model_dir.empty()) config.model_dir = model_dir;
    if (!keyword.empty()) config = config.withKeyword(keyword);
    if (threshold > 0.0f) config = config.withThreshold(threshold);

    SpacemiT::KwsEngine engine(config);
    if (!engine.IsInitialized()) {
        std::cerr << "Failed to initialize KWS engine: " << engine.GetLastError() << "\n"
                  << "See README 2.2 for how to fetch the model." << std::endl;
        return 1;
    }

    auto callback = std::make_shared<DemoCallback>(quiet);
    engine.SetCallback(callback);
    engine.Start();

    const auto& keywords = engine.GetKeywords();
    printf("engine  : %s, keywords:", engine.GetEngineName().c_str());
    for (const auto& k : keywords) printf(" %s", k.c_str());
    printf(", thr %.2f, lookahead %d ms, beam %s\n", config.threshold,
            engine.GetLookaheadMs(), use_beam ? "on" : "off");

    const size_t hop = 160;
    const size_t frames = wav.samples.size() / wav.channels;
    const double cpu0 = (double)clock() / CLOCKS_PER_SEC;
    for (size_t i = 0; i + hop <= frames; i += hop) {
        engine.SendAudioFrame(&wav.samples[i * wav.channels], hop);
    }
    engine.Stop();

    const double cpu = (double)clock() / CLOCKS_PER_SEC - cpu0;
    const double seconds = (double)frames / 16000.0;
    printf("done    | %d wake(s) over %.1fs audio, cpu %.2fs = %.1f%% of one core\n",
            callback->wakes, seconds, cpu, cpu / seconds * 100.0);
    return 0;
}
