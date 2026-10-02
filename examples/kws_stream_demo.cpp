/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Stream Demo
 *
 * 默认通过 SDK audio 组件采集麦克风；--stdin 接收 16 kHz S16_LE 裸流。
 *
 *   kws_stream_demo -l
 *   kws_stream_demo -i 0 -c 4 --model-dir /path/to/model
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <atomic>
#include <thread>
#include <cstdint>
#include <climits>
#include <csignal>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "kws_service.h"
#include "cli_utils.hpp"
#ifdef KWS_HAS_AUDIO
#include "audio_base.hpp"
#endif
#ifdef KWS_HAS_AEC
#include "kws_aec_audio.hpp"
#endif

namespace {

void usage() {
    printf("usage: kws_stream_demo [options]\n"
            "  -l, --list             List input/output devices (SDK audio indices)\n"
            "  -i, --input N          Microphone device (-1 = default)\n"
            "  -o, --output N         Wake response device (-1 = default)\n"
            "  -c, --channels N       Input channels (default 1; SPV uses 4)\n"
            "  -t, --time SECONDS     Microphone duration (0 = until Ctrl+C)\n"
            "  --stdin                Read 16 kHz S16_LE PCM from stdin instead\n"
            "  --model-dir DIR        Model directory\n"
            "  --preset NAME          xiaojin or xiaojin-4mic\n"
            "  --keyword TEXT         Wake word\n"
            "  --thr FLOAT            Detection threshold (default 0.3)\n"
            "  --holdoff-ms N         Wake cooldown (microphone default 0, stdin preset)\n"
            "  --partial-thr FLOAT    Also accept half the keyword above this score (0 = off)\n"
            "  --partial-wait-ms N    Wait for the full keyword before a partial fires (500)\n"
            "  --beam, --no-beam      Override beamforming\n"
            "  --ack-wav FILE         Wake response WAV (microphone mode defaults to\n"
            "                        ~/.cache/models/assets/audio/006_im_here.wav)\n"
            "  --no-ack               Disable the wake response\n"
            "  --ack CMD              Run a command instead of the built-in response\n"
            "  --aec                  WebRTC AEC3 with synchronized capture/playback\n"
            "  --aec-bypass           Same duplex path with AEC bypassed (A/B check)\n"
            "  --aec-delay-ms N       Device delay hint (0..500, default 50)\n"
            "  --playback-channels N  Duplex output channels (default 2)\n"
            "  --aec-playback FILE    Play a 16 kHz mono test WAV when capture starts\n"
            "  --aec-record PREFIX    Save raw/AEC/reference WAVs; requires -t\n"
            "  --quiet                Hide intermediate detection scores\n"
            "  -h, --help             Show this help\n");
#ifndef KWS_HAS_AUDIO
    printf("This build supports --stdin only; enable BUILD_KWS_MICROPHONE for capture.\n");
#elif defined(__linux__)
    printf("Linux audio defaults to ALSA hardware devices. Set PA_ALSA_IGNORE_ALL_PLUGINS=0\n"
            "to include virtual devices; use the same setting for -l and capture.\n");
#endif
}

int parseInteger(const std::string& value, const char* option, int minimum) {
    char* end = nullptr;
    errno = 0;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end || errno || number < minimum || number > INT_MAX) {
        fprintf(stderr, "%s expects an integer in [%d, %d]\n", option, minimum, INT_MAX);
        std::exit(1);
    }
    return static_cast<int>(number);
}

bool sendPcm(SpacemiT::KwsEngine& engine, const uint8_t* pcm, size_t bytes, int channels,
            std::vector<float>& frame) {
    if (bytes % (channels * 2) != 0) {
        fprintf(stderr, "incomplete interleaved PCM frame\n");
        return false;
    }
    frame.resize(bytes / 2);
    for (size_t i = 0; i < frame.size(); ++i) {
        const int value = pcm[2 * i] | (pcm[2 * i + 1] << 8);
        frame[i] = (value >= 32768 ? value - 65536 : value) / 32768.0f;
    }
    const size_t samples = bytes / (channels * 2);
    const size_t needed = (samples + 159) / 160;
    while (engine.IsStreaming() && engine.GetStreamStats().queued_blocks + needed > 128)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto status = engine.SendAudioFrame(frame.data(), samples);
    return status == SpacemiT::KwsAudioStatus::ACCEPTED || status == SpacemiT::KwsAudioStatus::EMPTY;
}

#ifdef KWS_HAS_AUDIO
bool configureAudioDevices() {
#ifdef __linux__
    // PortAudio probes every ALSA plugin during initialization, even with an
    // explicit device index. A desktop PipeWire plugin can hang without a usable
    // server. This hardware demo opts out before any SDK audio initialization;
    // keep an explicit environment override for virtual-device users.
    if (setenv("PA_ALSA_IGNORE_ALL_PLUGINS", "1", 0) != 0) {
        perror("Could not configure ALSA device enumeration");
        return false;
    }
    const bool hardware_only = std::atoi(std::getenv("PA_ALSA_IGNORE_ALL_PLUGINS")) != 0;
    printf("audio devices: ALSA %s\n", hardware_only ? "hardware only" : "hardware and plugins");
    fflush(stdout);
#endif
    return true;
}

constexpr const char* kAckUrl =
    "https://archive.spacemit.com/spacemit-ai/model_zoo/assets/audio/006_im_here.wav";

class WakePlayer {
public:
    WakePlayer(std::string path, int device) : path_(std::move(path)), device_(device) {}

    void Play() {
        if (playback_.valid()) {
            // Do not overlap responses or block inference while the previous response is playing.
            if (playback_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            Finish();
        }
        playback_ = std::async(std::launch::async, [this]() {
            SpacemitAudio::AudioPlayer player(device_);
            const bool ok = player.PlayFile(path_);
            if (ok) {
                printf("[ACK] played %s\n", path_.c_str());
                fflush(stdout);
            } else {
                fprintf(stderr, "[ACK] playback failed: %s (output device %d). Use -l/-o to select output.\n",
                        path_.c_str(), device_);
            }
            return ok;
        });
    }

    bool Finish() {
        if (playback_.valid()) ok_ = playback_.get() && ok_;
        return ok_;
    }

    ~WakePlayer() { Finish(); }

private:
    std::string path_;
    int device_;
    bool ok_ = true;
    std::future<bool> playback_;
};

volatile std::sig_atomic_t stop_requested = 0;

void stopSignal(int) {
    stop_requested = 1;
}

// Before any device or recording opens: Ctrl+C on `demo | tee` or a dropped
// ssh session must end in a clean shutdown that finalizes recordings, not in
// SIGPIPE/SIGHUP killing the process mid-write.
void installStopHandlers() {
    std::signal(SIGINT, stopSignal);
    std::signal(SIGTERM, stopSignal);
    std::signal(SIGHUP, stopSignal);
    std::signal(SIGPIPE, SIG_IGN);
}

bool captureMicrophone(SpacemiT::KwsEngine& engine, int device, int channels, int seconds) {
    // The library owns the preallocated audio queue and the inference worker.
    std::vector<float> frame(160 * channels);
    std::atomic<size_t> captured{0};
    std::atomic<bool> invalid{false};
    SpacemitAudio::AudioCapture capture(device);
    capture.SetCallback([&](const uint8_t* data, size_t bytes) {
        if (!data || bytes != frame.size() * 2) { invalid = true; return; }
        for (size_t i = 0; i < frame.size(); ++i) {
            const int value = data[2 * i] | (data[2 * i + 1] << 8);
            frame[i] = (value >= 32768 ? value - 65536 : value) / 32768.0f;
        }
        const auto status = engine.SendAudioFrame(frame.data(), 160);
        if (status != SpacemiT::KwsAudioStatus::ACCEPTED && status != SpacemiT::KwsAudioStatus::QUEUE_FULL)
            invalid = true;
        captured.fetch_add(160, std::memory_order_relaxed);
    });
    if (!capture.Start(16000, channels, 160 * channels * 2)) {
        fprintf(stderr, "Could not start microphone. Use -l to list devices and -i/-c to select input.\n");
        return false;
    }
    printf("microphone listening: device=%d, 16000 Hz, %d channel(s); Ctrl+C to stop\n", device, channels);
    fflush(stdout);
    const auto started = std::chrono::steady_clock::now();
    auto last_audio = started;
    size_t previous = 0;
    bool ok = true;
    while (!stop_requested) {
        const auto now = std::chrono::steady_clock::now();
        if (seconds > 0 && now - started >= std::chrono::seconds(seconds)) break;
        const size_t current = captured.load();
        if (current != previous) { previous = current; last_audio = now; }
        if (invalid || !engine.IsStreaming() || !capture.IsRunning() || now - last_audio > std::chrono::seconds(5)) {
            fprintf(stderr, "Microphone stopped or delivered invalid audio\n");
            ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    capture.Stop();
    capture.Close();
    const auto stats = engine.GetStreamStats();
    printf("captured %zu frames (%.2f s), dropped %llu samples in %llu overruns\n", captured.load(),
        captured.load() / 16000.0, static_cast<unsigned long long>(stats.dropped_samples),
        static_cast<unsigned long long>(stats.input_overruns));
    return ok && (captured > 0 || stop_requested);
}
#endif

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

#ifdef KWS_HAS_AUDIO
    void SetWakeAudio(const std::string& path, int device) {
        player_ = std::make_unique<WakePlayer>(path, device);
    }
#endif
    void SetWakeAction(std::function<void()> action) { wake_action_ = std::move(action); }

    bool Finish() {
#ifdef KWS_HAS_AUDIO
        if (player_) return player_->Finish();
#endif
        return true;
    }

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
        if (wake_action_) wake_action_();
#ifdef KWS_HAS_AUDIO
        if (player_) player_->Play();
#endif
        if (!ack_.empty()) {
            const std::string cmd = ack_ + " &";
            (void)!system(cmd.c_str());
        }
    }

    void OnError(const std::string& message) override {
        fprintf(stderr, "error: %s\n", message.c_str());
    }

private:
#ifdef KWS_HAS_AUDIO
    std::unique_ptr<WakePlayer> player_;
#endif
    std::string ack_;
    bool quiet_;
    std::function<void()> wake_action_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string model_dir, keyword, ack, ack_wav, preset;
    int channels = 0;
    int device = -1, output_device = -1, seconds = 0;
    float threshold = -1.0f;
    bool beam = false, beam_set = false, quiet = false;
    bool stdin_mode = false, list = false, device_set = false, time_set = false;
    bool no_ack = false, ack_set = false, ack_wav_set = false, output_set = false;
    bool aec = false, aec_bypass = false, aec_options = false;
    int aec_delay = 50, playback_channels = 2;
    int holdoff_ms = -1;
    float partial_thr = 0.0f;
    int partial_wait_ms = -1;
    std::string aec_playback, aec_record;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() {
            if (i + 1 == argc) {
                fprintf(stderr, "%s requires a value\n", k.c_str());
                std::exit(1);
            }
            return std::string(argv[++i]);
        };
        if (k == "-h" || k == "--help") { usage(); return 0; }
        else if (k == "-l" || k == "--list") list = true;
        else if (k == "--stdin") stdin_mode = true;
        else if (k == "-i" || k == "--input") {
            device = parseInteger(val(), "--input", -1);
            device_set = true;
        } else if (k == "-o" || k == "--output") {
            output_device = parseInteger(val(), "--output", -1);
            output_set = true;
        } else if (k == "-t" || k == "--time") {
            seconds = parseInteger(val(), "--time", 0);
            time_set = true;
        } else if (k == "-c" || k == "--channels") channels = parseChannels(val());
        else if (k == "--model-dir") model_dir = val();
        else if (k == "--keyword") keyword = val();
        else if (k == "--preset") preset = val();
        else if (k == "--ack") { ack = val(); ack_set = true; }
        else if (k == "--ack-wav") { ack_wav = val(); ack_wav_set = true; }
        else if (k == "--no-ack") no_ack = true;
        else if (k == "--aec") aec = true;
        else if (k == "--aec-bypass") aec_bypass = true;
        else if (k == "--aec-delay-ms") {
            aec_delay = parseInteger(val(), "--aec-delay-ms", 0);
            aec_options = true;
        } else if (k == "--playback-channels") {
            playback_channels = parseChannels(val());
            aec_options = true;
        } else if (k == "--aec-playback") { aec_playback = val(); aec_options = true; }
        else if (k == "--aec-record") { aec_record = val(); aec_options = true; }
        else if (k == "--thr") threshold = parseThreshold(val());
        else if (k == "--holdoff-ms") holdoff_ms = parseInteger(val(), "--holdoff-ms", 0);
        else if (k == "--partial-thr") partial_thr = parseThreshold(val());
        else if (k == "--partial-wait-ms") partial_wait_ms = parseInteger(val(), "--partial-wait-ms", 0);
        else if (k == "--beam") { beam = true; beam_set = true; }
        else if (k == "--no-beam") { beam = false; beam_set = true; }
        else if (k == "--quiet") quiet = true;
        else {
            fprintf(stderr, "Unknown option: %s\n", k.c_str());
            usage();
            return 1;
        }
    }
    if (stdin_mode && (device_set || time_set || list)) {
        fprintf(stderr, "--stdin cannot be combined with microphone options (-i, -t, -l)\n");
        return 1;
    }
    const bool duplex_mode = aec || aec_bypass;
    if ((aec && aec_bypass) || (duplex_mode && (stdin_mode || ack_set)) ||
        (aec_options && !duplex_mode) || aec_delay > 500 || (!aec_record.empty() && seconds == 0)) {
        fprintf(stderr, "AEC requires microphone mode and built-in playback; choose --aec or --aec-bypass. "
                "AEC options require that mode, delay must be <=500 ms, and recordings require -t SECONDS.\n");
        return 1;
    }
#ifndef KWS_HAS_AEC
    if (duplex_mode) {
        fprintf(stderr, "WebRTC AEC support is not built. Enable BUILD_KWS_AEC with the SDK audio and WebRTC dependencies.\n");
        return 1;
    }
#endif
    if ((no_ack && (ack_set || ack_wav_set || (output_set && !duplex_mode))) ||
        (ack_set && (ack_wav_set || output_set))) {
        fprintf(stderr, "Choose one wake response: --no-ack, --ack CMD, or --ack-wav FILE [-o N]\n");
        return 1;
    }
    if (stdin_mode && output_set && !ack_wav_set) {
        fprintf(stderr, "--stdin requires --ack-wav FILE to enable audio output\n");
        return 1;
    }
#ifdef KWS_HAS_AUDIO
    if ((!stdin_mode || ack_wav_set) && !configureAudioDevices()) return 1;
    if (list) {
        const auto devices = SpacemitAudio::AudioCapture::ListDevices();
        printf("Input devices:\n");
        for (const auto& [index, name] : devices) printf("[%d] %s\n", index, name.c_str());
        const auto outputs = SpacemitAudio::AudioPlayer::ListDevices();
        printf("Output devices:\n");
        for (const auto& [index, name] : outputs) printf("[%d] %s\n", index, name.c_str());
        if (devices.empty() && outputs.empty()) fprintf(stderr, "No audio devices found.\n");
        return devices.empty() && outputs.empty() ? 1 : 0;
    }
    if (!stdin_mode && !no_ack && !ack_set && !ack_wav_set) {
        const char* home_dir = std::getenv("HOME");
        if (home_dir) ack_wav = std::string(home_dir) + "/.cache/models/assets/audio/006_im_here.wav";
    }
    if (ack_wav_set || (!stdin_mode && !no_ack && !ack_set)) {
        if (ack_wav.empty() || !std::ifstream(ack_wav, std::ios::binary).good()) {
            fprintf(stderr, "Wake response WAV is not readable: %s\nDownload: %s\n",
                    ack_wav.empty() ? "HOME is unset; use --ack-wav FILE" : ack_wav.c_str(), kAckUrl);
            if (ack_wav_set || output_set) return 1;
            fprintf(stderr, "Continuing without a wake response; use --no-ack to disable it explicitly.\n");
            ack_wav.clear();
        }
    }
#else
    if (ack_wav_set || output_set) {
        fprintf(stderr, "Playback support is not built. Enable BUILD_KWS_MICROPHONE with the SDK audio component.\n");
        return 1;
    }
    if (!stdin_mode) {
        fprintf(stderr, "Microphone support is not built. Enable BUILD_KWS_MICROPHONE with the SDK audio component, or use --stdin.\n");
        return 1;
    }
#endif
    auto config = SpacemiT::KwsConfig::Preset(
        !preset.empty() ? preset : (channels >= 4 ? "xiaojin-4mic" : "xiaojin"));
    if (channels != 0) config.num_channels = channels;
    if (beam_set) config.use_beamforming = beam;
    channels = config.num_channels;
    beam = config.use_beamforming;
    if (!model_dir.empty()) config.model_dir = model_dir;
    if (!keyword.empty()) config = config.withKeyword(keyword);
    if (threshold != -1.0f) config = config.withThreshold(threshold);
    if (holdoff_ms >= 0) config.holdoff_ms = holdoff_ms;
    // Each detection consumes its own history, so live capture needs no
    // cooldown: every call fires on its own.
    else if (!stdin_mode) config.holdoff_ms = 0;
    config.partial_threshold = partial_thr;
    if (partial_wait_ms >= 0) config.partial_wait_ms = partial_wait_ms;

    SpacemiT::KwsEngine engine(config);
    if (!engine.IsInitialized()) {
        std::cerr << "Failed to initialize KWS engine: " << engine.GetLastError() << std::endl;
        return 1;
    }

    auto callback = std::make_shared<StreamCallback>(ack, quiet);
#ifdef KWS_HAS_AUDIO
    if (!stdin_mode) installStopHandlers();
#endif
#ifdef KWS_HAS_AEC
    std::unique_ptr<kws_demo::AecDuplexAudio> duplex;
    if (duplex_mode) {
        kws_demo::AecAudioOptions options;
        options.input_device = device;
        options.output_device = output_device;
        options.channels = channels;
        options.output_channels = playback_channels;
        options.first_microphone = channels == 1 && config.beam_first_channel == 1 ? 0 : config.beam_first_channel;
        options.microphones = beam ? 3 : 1;
        options.delay_ms = aec_delay;
        options.bypass = aec_bypass;
        options.response_wav = ack_wav;
        options.playback_wav = aec_playback;
        options.record_prefix = aec_record;
        duplex = std::make_unique<kws_demo::AecDuplexAudio>(options);
        if (!duplex->Initialize()) {
            fprintf(stderr, "%s\n", duplex->Error().c_str());
            return 1;
        }
        callback->SetWakeAction([&]() { duplex->PlayResponse(); });
    }
#endif
#ifdef KWS_HAS_AUDIO
    if (!ack_wav.empty()) {
        if (!duplex_mode) callback->SetWakeAudio(ack_wav, output_device);
        printf("wake response: %s (output device %d)\n", ack_wav.c_str(), output_device);
    }
#endif
    engine.SetCallback(callback);
    if (!engine.Start()) { std::cerr << engine.GetLastError() << std::endl; return 1; }
    printf("engine: %s, thr %.2f, %d channel(s), beam %s, lookahead %d ms\n",
            engine.GetEngineName().c_str(), config.threshold, channels,
            beam ? "on" : "off", engine.GetLookaheadMs());
    printf("wake cooldown: %d ms; each detection consumes its keyword history\n", config.holdoff_ms);
    if (config.partial_threshold > 0.0f)
        printf("partial match: half keyword accepted at score >= %.2f after %d ms\n",
                config.partial_threshold, config.partial_wait_ms);
    fflush(stdout);

    bool ok = true;
    if (stdin_mode) {
        printf("reading 16000 Hz S16_LE PCM from stdin\n");
        fflush(stdout);
        std::vector<uint8_t> pcm(160 * channels * 2);
        std::vector<float> frame;
        size_t count;
        while ((count = fread(pcm.data(), 1, pcm.size(), stdin)) > 0) {
            if (!sendPcm(engine, pcm.data(), count, channels, frame)) {
                ok = false;
                break;
            }
        }
        if (ferror(stdin)) { perror("stdin"); ok = false; }
    }
#ifdef KWS_HAS_AUDIO
    else {
        printf("opening %s audio devices...\n", duplex_mode ? "full-duplex" : "microphone");
        fflush(stdout);
#ifdef KWS_HAS_AEC
        if (duplex) {
            ok = duplex->Run(engine, seconds, stop_requested);
            if (!ok) fprintf(stderr, "AEC audio failed: %s\n", duplex->Error().c_str());
        } else
#endif
        ok = captureMicrophone(engine, device, channels, seconds);
    }
#endif
    engine.Stop();
    callback->SetWakeAction({});
    const bool playback_ok = callback->Finish();
    return ok && playback_ok && engine.GetLastError().empty() ? 0 : 1;
}
