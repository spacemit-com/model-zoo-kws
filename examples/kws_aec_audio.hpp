/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef KWS_AEC_AUDIO_HPP
#define KWS_AEC_AUDIO_HPP

#include <csignal>
#include <memory>
#include <string>
#include "kws_service.h"

namespace kws_demo {

// One independent AEC3 filter per consumed microphone, before the existing beamformer.
// The caller supplies aligned 10 ms input/render blocks, both normalized float PCM.
class WebRtcAec {
public:
    WebRtcAec(int channels, int first, int microphones, int delay_ms);
    ~WebRtcAec();
    bool Initialize();
    bool Process(const float* capture, const float* render, float* output);
    const std::string& Error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AecAudioOptions {
    int input_device = -1;
    int output_device = -1;
    int channels = 4;
    int output_channels = 2;
    int first_microphone = 1;
    int microphones = 3;
    int delay_ms = 50;
    bool bypass = false;
    std::string response_wav;
    std::string playback_wav;  // Optional finite test playback, started with capture.
    std::string record_prefix; // Optional raw/processed/reference WAV diagnostics.
};

class AecDuplexAudio {
public:
    explicit AecDuplexAudio(const AecAudioOptions& options);
    ~AecDuplexAudio();
    bool Initialize();
    bool Run(SpacemiT::KwsEngine& engine, int seconds,
                const volatile std::sig_atomic_t& stop_requested);
    void PlayResponse();
    const std::string& Error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kws_demo
#endif  // KWS_AEC_AUDIO_HPP
