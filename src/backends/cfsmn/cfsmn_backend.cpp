/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cfsmn_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kws {

namespace {

const char kDefaultModelDir[] = "~/.cache/models/kws/xiaojin";
const char kWeightsFile[] = "cfsmn.bin";
const char kBeamFile[] = "beam_w.bin";
const char kKeywordsFile[] = "keywords.txt";

std::string expandUser(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

std::string join(const std::string& dir, const char* name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

bool exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// keywords.txt: "<文本> <token_ids 逗号分隔> [阈值]"，# 起始为注释。
std::vector<Keyword> readKeywordFile(const std::string& path) {
    std::vector<Keyword> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        Keyword kw;
        std::string ids;
        if (!(ls >> kw.text >> ids)) continue;
        std::replace(ids.begin(), ids.end(), ',', ' ');
        std::istringstream is(ids);
        int id = 0;
        while (is >> id) kw.token_ids.push_back(id);
        ls >> kw.threshold;
        if (!kw.token_ids.empty()) out.push_back(kw);
    }
    return out;
}

}  // namespace

// =============================================================================
// Initialization
// =============================================================================

ErrorInfo CfsmnBackend::loadKeywords(const KwsConfig& config, const std::string& model_dir) {
    const std::vector<Keyword> known = readKeywordFile(join(model_dir, kKeywordsFile));
    std::vector<Keyword> wanted = config.keywords;
    if (wanted.empty()) {
        wanted = known;
        if (wanted.empty()) {
            return ErrorInfo::error(ErrorCode::KEYWORD_NOT_FOUND,
                                    "No keyword configured and no keywords.txt",
                                    join(model_dir, kKeywordsFile));
        }
    }

    slots_.clear();
    for (const Keyword& kw : wanted) {
        Keyword resolved = kw;
        if (resolved.token_ids.empty()) {
            auto it = std::find_if(known.begin(), known.end(),
                                    [&](const Keyword& k) { return k.text == kw.text; });
            if (it == known.end()) {
                return ErrorInfo::error(ErrorCode::KEYWORD_NOT_FOUND,
                                        "Keyword '" + kw.text + "' has no token ids",
                                        "add it to " + join(model_dir, kKeywordsFile));
            }
            resolved.token_ids = it->token_ids;
            if (resolved.threshold <= 0.0f) resolved.threshold = it->threshold;
        }
        Slot slot;
        slot.text = resolved.text;
        slot.threshold = resolved.threshold > 0.0f ? resolved.threshold : config.threshold;
        slot.decoder.set_keyword(resolved.token_ids);
        slot.ring.resize(config.decode_context);
        slot.order.resize(config.decode_context);
        slots_.push_back(std::move(slot));
    }
    return ErrorInfo::ok();
}

ErrorInfo CfsmnBackend::initialize(const KwsConfig& config) {
    config_ = config;
    if (config_.sample_rate != 16000) {
        return ErrorInfo::error(ErrorCode::UNSUPPORTED_SAMPLE_RATE,
                                "cFSMN backend only supports 16 kHz",
                                std::to_string(config_.sample_rate));
    }
    if (config_.num_channels < 1) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "num_channels must be >= 1");
    }
    if (config_.decode_context < 1 || config_.score_interval < 1) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                "decode_context and score_interval must be >= 1");
    }
    if (config_.beam_first_channel < 0) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "beam_first_channel must be >= 0");
    }
    if (config_.use_beamforming &&
        config_.beam_first_channel + cfsmn::Beamformer::kCh > config_.num_channels) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                "beamforming needs 3 channels from beam_first_channel on",
                                std::to_string(config_.num_channels) + " channels given");
    }
    // 波束关闭时只取一路：beam_first_channel 指向的那一路（SPV 的第一支裸麦），
    // 通道数不够就退回 ch0，单声道输入因此不需要额外配置。
    source_channel_ = config_.use_beamforming
        ? config_.beam_first_channel
        : std::min(config_.beam_first_channel, config_.num_channels - 1);

    std::string model_dir = config_.model_dir;
    if (model_dir.empty()) {
        const char* env = std::getenv("KWS_MODEL_DIR");
        model_dir = env ? env : kDefaultModelDir;
    }
    model_dir = expandUser(model_dir);
    config_.model_dir = model_dir;

    const std::string weights = join(model_dir, kWeightsFile);
    if (!exists(weights)) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND, "Model weights not found", weights);
    }
    if (!model_.load(weights.c_str())) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND, "Failed to load model", weights);
    }
    if (config_.use_beamforming) {
        const std::string beam = join(model_dir, kBeamFile);
        if (!exists(beam) || !beam_.load(beam.c_str())) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
                                    "Beamformer weights not found", beam);
        }
    }

    auto err = loadKeywords(config_, model_dir);
    if (!err.isOk()) return err;

    fbank_.init();
    stream_.init(model_);
    chan_.assign((size_t)kHop * cfsmn::Beamformer::kCh, 0.0f);
    beamed_.assign(kHop, 0.0f);
    feat_.assign(model_.idim, 0.0f);
    logits_.assign(model_.odim, 0.0f);
    reset();

    initialized_ = true;
    return ErrorInfo::ok();
}

void CfsmnBackend::shutdown() {
    initialized_ = false;
    slots_.clear();
    pending_.clear();
}

std::vector<std::string> CfsmnBackend::getKeywords() const {
    std::vector<std::string> out;
    out.reserve(slots_.size());
    for (const Slot& slot : slots_) out.push_back(slot.text);
    return out;
}

ErrorInfo CfsmnBackend::setThreshold(float threshold) {
    if (threshold < 0.0f || threshold > 1.0f) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "threshold must be in [0, 1]");
    }
    config_.threshold = threshold;
    for (Slot& slot : slots_) slot.threshold = threshold;
    return ErrorInfo::ok();
}

void CfsmnBackend::reset() {
    pending_.clear();
    fb_new_.clear();
    fb_hist_.clear();
    fb_idx_.clear();
    fb_next_ = nposts_ = model_frames_ = 0;
    audio_time_ = 0.0;
    fbank_.reset();
    stream_.init(model_);
    if (config_.use_beamforming) beam_.reset();
    for (Slot& slot : slots_) {
        slot.last_fire = -1e9;
        std::fill(slot.ring.begin(), slot.ring.end(), cfsmn::FrameCand{});
        std::fill(slot.order.begin(), slot.order.end(), cfsmn::FrameCand{});
    }
}

// =============================================================================
// Audio path
// =============================================================================

ErrorInfo CfsmnBackend::process(const AudioChunk& audio,
                                std::vector<DetectionResult>& results) {
    if (!initialized_) {
        return ErrorInfo::error(ErrorCode::NOT_INITIALIZED, "Backend not initialized");
    }
    if (audio.isEmpty()) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "Empty audio chunk");
    }
    if (audio.sample_rate != config_.sample_rate) {
        return ErrorInfo::error(ErrorCode::UNSUPPORTED_SAMPLE_RATE,
                                "Sample rate does not match the configured one",
                                std::to_string(audio.sample_rate));
    }
    const int channels = audio.num_channels > 0 ? audio.num_channels : config_.num_channels;
    if (channels != config_.num_channels) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER,
                                "Channel count does not match the configured one",
                                std::to_string(channels));
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 调用方可以送任意长度，这里攒够一个 10 ms hop 再往下走。
    pending_.insert(pending_.end(), audio.data,
                    audio.data + audio.num_samples * (size_t)channels);
    const size_t stride = (size_t)kHop * channels;
    size_t offset = 0;
    while (pending_.size() - offset >= stride) {
        processHop(pending_.data() + offset, results);
        offset += stride;
    }
    pending_.erase(pending_.begin(), pending_.begin() + offset);

    const int elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    for (DetectionResult& r : results) r.processing_time_ms = elapsed_ms;
    return ErrorInfo::ok();
}

void CfsmnBackend::processHop(const float* interleaved, std::vector<DetectionResult>& results) {
    const int channels = config_.num_channels;
    const int first = config_.beam_first_channel;
    const int source = source_channel_;
    // 模型是在 int16 量级的 fbank 上训练的，这里把 [-1, 1] 还原回去。
    if (config_.use_beamforming) {
        for (int i = 0; i < kHop; ++i)
            for (int c = 0; c < cfsmn::Beamformer::kCh; ++c)
                chan_[i * cfsmn::Beamformer::kCh + c] =
                    interleaved[(size_t)i * channels + first + c] * 32768.0f;
        beam_.process(chan_.data(), beamed_.data());
    } else {
        for (int i = 0; i < kHop; ++i)
            beamed_[i] = interleaved[(size_t)i * channels + source] * 32768.0f;
    }
    audio_time_ += (double)kHop / 16000.0;

    fb_new_.clear();
    fbank_.feed(beamed_.data(), kHop, fb_new_);
    for (const std::vector<float>& frame : fb_new_) onFbankFrame(frame, results);
}

// 模型帧 i 由 fbank 帧 3i-2 .. 3i+2 拼成（上下文 2/2，抽帧 3），
// 所以 fbank 帧 3i+2 到达时它才齐活。
void CfsmnBackend::onFbankFrame(const std::vector<float>& frame,
                                std::vector<DetectionResult>& results) {
    const long idx = fb_next_++;
    fb_hist_.push_back(frame);
    fb_idx_.push_back(idx);
    if (fb_hist_.size() > 8) {
        fb_hist_.erase(fb_hist_.begin());
        fb_idx_.erase(fb_idx_.begin());
    }
    if (idx < 2 || (idx - 2) % 3 != 0) return;
    const long i = (idx - 2) / 3;
    if (i < 1) return;
    const long need = 3 * i - 2;
    if (fb_idx_.front() > need) return;
    size_t base = 0;
    while (base < fb_idx_.size() && fb_idx_[base] != need) ++base;
    if (base + 5 > fb_hist_.size()) return;
    for (int k = 0; k < 5; ++k)
        std::memcpy(&feat_[(size_t)k * cfsmn::Fbank::kBins], fb_hist_[base + k].data(),
                    sizeof(float) * cfsmn::Fbank::kBins);

    if (!stream_.push(feat_.data(), logits_.data())) return;   // 流水线还在填充
    for (Slot& slot : slots_)
        slot.decoder.candidates(logits_.data(), model_.odim,
                                slot.ring[nposts_ % config_.decode_context]);
    ++nposts_;
    ++model_frames_;
    if (model_frames_ % config_.score_interval == 0) scoreFrame(results);
}

void CfsmnBackend::scoreFrame(std::vector<DetectionResult>& results) {
    const int n = (int)std::min<long>(nposts_, config_.decode_context);
    const long start = nposts_ - n;
    const double t = audio_time_ - model_.lookahead() * (kFrameMs / 1000.0);
    const int64_t timestamp_ms = (int64_t)(t * 1000.0);

    DetectionResult best;
    best.timestamp_ms = timestamp_ms;
    bool fired = false;

    for (size_t s = 0; s < slots_.size(); ++s) {
        Slot& slot = slots_[s];
        for (int k = 0; k < n; ++k)
            slot.order[k] = slot.ring[(start + k) % config_.decode_context];
        const float score = slot.decoder.score(slot.order.data(), n);
        if (score >= slot.threshold && t - slot.last_fire > config_.holdoff_ms / 1000.0) {
            slot.last_fire = t;
            DetectionResult r;
            r.score = score;
            r.is_wake_word = true;
            r.keyword_index = (int)s;
            r.keyword = slot.text;
            r.timestamp_ms = timestamp_ms;
            results.push_back(r);
            fired = true;
        } else if (score > best.score) {
            best.score = score;
            best.keyword_index = (int)s;
            best.keyword = slot.text;
        }
    }
    // 没有命中时也上报一次当前最高分，调阈值和看日志都要靠它。
    if (!fired) results.push_back(best);
}

}  // namespace kws
