/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Stream Demo
 *
 * 从标准输入读 16 kHz S16_LE 裸流，边收边检测，命中时可播一段应答音。
 * 不依赖 PortAudio，采集交给 arecord：
 *
 *   arecord -q -D hw:1,0 -f S16_LE -c 4 -r 16000 -t raw | \
 *       kws_stream_demo --channels 4 --ack "aplay -q ~/.cache/models/assets/audio/006_im_here.wav"
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "kws_service.h"

namespace {

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    char hms[32], out[48];
    std::strftime(hms, sizeof hms, "%H:%M:%S", std::localtime(&tt));
    snprintf(out, sizeof out, "%s.%03d", hms, (int)ms.count());
    return out;
}

class StreamCallback : public SpacemiT::KwsEngineCallback {
public:
    StreamCallback(std::string ack, bool quiet) : ack_(std::move(ack)), quiet_(quiet) {}

    void OnEvent(std::shared_ptr<SpacemiT::KwsResult> result) override {
        if (!result->IsWakeWord() && !quiet_ && result->GetScore() > 0.02f) {
            printf("[%s]  score %.3f (t=%.1fs)\n", timestamp().c_str(), result->GetScore(),
                    result->GetTimestampMs() / 1000.0);
            fflush(stdout);
        }
    }

    void OnWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) override {
        printf("[%s] [WAKE] %s score %.3f (t=%.1fs)\n", timestamp().c_str(), keyword.c_str(),
                score, timestamp_ms / 1000.0);
        fflush(stdout);
        if (!ack_.empty()) {
            const std::string cmd = ack_ + " &";
            (void)!system(cmd.c_str());
        }
    }

    void OnError(const std::string& message) override {
        fprintf(stderr, "error: %s\n", message.c_str());
    }

private:
    std::string ack_;
    bool quiet_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string model_dir, keyword, ack, preset;
    int channels = 1;
    float threshold = -1.0f;
    bool beam = false, quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--channels") channels = atoi(val().c_str());
        else if (k == "--model-dir") model_dir = val();
        else if (k == "--keyword") keyword = val();
        else if (k == "--preset") preset = val();
        else if (k == "--ack") ack = val();
        else if (k == "--thr") threshold = atof(val().c_str());
        else if (k == "--beam") beam = true;
        else if (k == "--no-beam") beam = false;
        else if (k == "--quiet") quiet = true;
        else {
            fprintf(stderr,
                    "usage: kws_stream_demo [--channels 4] [--beam] [--model-dir DIR]\n"
                    "                       [--preset NAME] [--keyword TEXT] [--thr 0.3]\n"
                    "                       [--ack CMD] [--quiet]   (reads S16_LE from stdin)\n");
            return k == "--help" ? 0 : 1;
        }
    }
    if (channels >= 4 && !beam) beam = true;   // 4 通道默认就是 SPV 复合设备

    auto config = SpacemiT::KwsConfig::Preset(
        !preset.empty() ? preset : (beam ? "xiaojin-4mic" : "xiaojin"));
    config.num_channels = channels;
    config.use_beamforming = beam;
    if (!model_dir.empty()) config.model_dir = model_dir;
    if (!keyword.empty()) config = config.withKeyword(keyword);
    if (threshold > 0.0f) config = config.withThreshold(threshold);

    SpacemiT::KwsEngine engine(config);
    if (!engine.IsInitialized()) {
        std::cerr << "Failed to initialize KWS engine: " << engine.GetLastError() << std::endl;
        return 1;
    }

    engine.SetCallback(std::make_shared<StreamCallback>(ack, quiet));
    engine.Start();
    printf("listening: %s, thr %.2f, %d channel(s), beam %s, lookahead %d ms\n",
            engine.GetEngineName().c_str(), config.threshold, channels,
            beam ? "on" : "off", engine.GetLookaheadMs());
    fflush(stdout);

    const size_t hop = 160;
    std::vector<short> pcm(hop * channels);
    std::vector<float> frame(hop * channels);
    while (fread(pcm.data(), sizeof(short), pcm.size(), stdin) == pcm.size()) {
        for (size_t i = 0; i < pcm.size(); ++i) frame[i] = pcm[i] / 32768.0f;
        engine.SendAudioFrame(frame.data(), hop);
    }
    engine.Stop();
    return 0;
}
