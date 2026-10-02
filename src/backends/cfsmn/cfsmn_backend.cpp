/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cfsmn_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace kws {

namespace {

const char kDefaultModelDir[] = "~/.cache/models/kws/xiaojin-v1";
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
// "!<文本> <token_ids>" 声明易混词（如 !小姐小姐）：与关键词等长、逐位不同处的 token
// 在检测时和关键词 token 比后验，易混词更像就不上报。
bool readKeywordFile(const std::string& path, std::vector<Keyword>& out, std::vector<Keyword>& rivals,
        std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "Cannot open " + path; return false; }
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = line.substr(0, line.find('#'));
        std::istringstream ls(line);
        Keyword kw;
        if (!(ls >> kw.text)) continue;
        const auto malformed = [&]() {
            error = path + ":" + std::to_string(number) + ": expected text token_id[, token_id...] [threshold]";
            return false;
        };
        while (true) {
            int id;
            if (!(ls >> id)) return malformed();
            kw.token_ids.push_back(id);
            ls >> std::ws;
            if (ls.peek() != ',') break;
            ls.get();
        }
        const bool rival = kw.text[0] == '!';
        if (!ls.eof()) {
            if (rival || !(ls >> kw.threshold)) return malformed();
            ls >> std::ws;
            if (!ls.eof()) return malformed();
        }
        if (rival) {
            kw.text.erase(0, 1);
            rivals.push_back(std::move(kw));
            continue;
        }
        out.push_back(std::move(kw));
        if (out.size() > 64) { error = path + ": more than 64 keywords"; return false; }
    }
    return true;
}

}  // namespace

// =============================================================================
// Initialization
// =============================================================================

ErrorInfo CfsmnBackend::loadKeywords(const KwsConfig& config, const std::string& model_dir) {
    std::vector<Keyword> known, rivals;
    const bool needs_file = config.keywords.empty() || std::any_of(
        config.keywords.begin(), config.keywords.end(), [](const Keyword& kw) { return kw.token_ids.empty(); });
    std::string error;
    if (needs_file && !readKeywordFile(join(model_dir, kKeywordsFile), known, rivals, error)) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Malformed keywords.txt", error);
    }
    std::vector<Keyword> wanted = config.keywords;
    if (wanted.empty()) {
        wanted = known;
        if (wanted.empty()) {
            return ErrorInfo::error(ErrorCode::KEYWORD_NOT_FOUND,
                                    "No keyword configured and no keywords.txt",
                                    join(model_dir, kKeywordsFile));
        }
    }

    if (wanted.size() > 64)
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "At most 64 keywords are supported");
    slots_.clear();
    for (const Keyword& kw : wanted) {
        Keyword resolved = kw;
        if (kw.text.empty() || !std::isfinite(kw.threshold) || kw.threshold < 0 || kw.threshold > 1)
            return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Invalid keyword or threshold");
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
        if (resolved.token_ids.empty() || resolved.token_ids.size() > (size_t)config.decode_context ||
            !std::isfinite(resolved.threshold) || resolved.threshold < 0 || resolved.threshold > 1 ||
            std::any_of(resolved.token_ids.begin(), resolved.token_ids.end(),
                        [&](int id) { return id <= 0 || id >= model_.odim; })) {
            return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                    "Keyword token ids must be nonblank vocabulary indices");
        }
        Slot slot;
        slot.text = resolved.text;
        slot.threshold = resolved.threshold > 0.0f ? resolved.threshold : config.threshold;
        slot.decoder.set_keyword(resolved.token_ids);
        std::vector<std::vector<int>> rival_ids;
        for (const Keyword& r : rivals) {
            if (r.token_ids.size() != resolved.token_ids.size()) continue;
            if (std::any_of(r.token_ids.begin(), r.token_ids.end(),
                            [&](int id) { return id <= 0 || id >= model_.odim; })) {
                return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                        "Confusable word token ids must be nonblank vocabulary indices", r.text);
            }
            rival_ids.push_back(r.token_ids);
            slot.rivals.push_back(r);
        }
        if (!slot.decoder.set_rivals(rival_ids)) {
            return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                    "Confusable words differ from '" + resolved.text + "' in more than " +
                                    std::to_string(cfsmn::kMaxRivalGroups) + " distinct tokens");
        }
        slot.ring.resize(config.decode_context);
        slot.order.resize(config.decode_context);
        slots_.push_back(std::move(slot));
    }
    return ErrorInfo::ok();
}

ErrorInfo CfsmnBackend::initialize(const KwsConfig& config) {
    shutdown();
    config_ = config;
    if (config_.sample_rate != 16000) {
        return ErrorInfo::error(ErrorCode::UNSUPPORTED_SAMPLE_RATE,
                                "cFSMN backend only supports 16 kHz",
                                std::to_string(config_.sample_rate));
    }
    if (config_.num_channels < 1 || config_.num_channels > 64) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "num_channels must be in [1, 64]");
    }
    if (config_.decode_context < 1 || config_.decode_context > 4096 || config_.score_interval < 1) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                "decode_context must be in [1, 4096], score_interval must be >= 1");
    }
    if (!std::isfinite(config_.threshold) || config_.threshold < 0 || config_.threshold > 1 ||
        config_.holdoff_ms < 0 || config_.frame_size < 1 || config_.num_threads != 1) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                "Invalid threshold, holdoff, frame_size or num_threads (only 1 supported)");
    }
    if (!std::isfinite(config_.partial_threshold) || config_.partial_threshold < 0 ||
        config_.partial_threshold > 1 || config_.partial_wait_ms < 0 || config_.partial_min_tokens < 1 ||
        config_.partial_holdoff_ms < 0) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Invalid partial-match settings");
    }
    if (config_.beam_first_channel < 0) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "beam_first_channel must be >= 0");
    }
    if (config_.use_beamforming &&
        config_.beam_first_channel > config_.num_channels - frontend::Beamformer::kCh) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                                "beamforming needs 3 channels from beam_first_channel on",
                                std::to_string(config_.num_channels) + " channels given");
    }
    // The default first raw microphone (ch1) maps to ch0 for mono input only.
    source_channel_ = config_.num_channels == 1 && config_.beam_first_channel == 1
        ? 0 : config_.beam_first_channel;
    if (source_channel_ >= config_.num_channels)
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Source channel is out of range");

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
    if (model_.idim != 5 * frontend::Fbank::kBins) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Model input dimension must be 400");
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
    chan_.assign((size_t)kHop * frontend::Beamformer::kCh, 0.0f);
    beamed_.assign(kHop, 0.0f);
    feat_.assign(model_.idim, 0.0f);
    logits_.assign(model_.odim, 0.0f);
    reset();

    initialized_ = true;
    return ErrorInfo::ok();
}

void CfsmnBackend::shutdown() {
    initialized_ = false;
    streaming_.store(false);
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
    if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 1.0f) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "threshold must be in [0, 1]");
    }
    config_.threshold = threshold;
    for (Slot& slot : slots_) slot.threshold = threshold;
    return ErrorInfo::ok();
}

void CfsmnBackend::reset() {
    finished_ = false;
    pending_.clear();
    fb_new_.clear();
    fb_hist_.clear();
    fb_idx_.clear();
    fb_next_ = nposts_ = model_frames_ = 0;
    input_samples_ = 0;
    fbank_.reset();
    stream_.init(model_);
    if (config_.use_beamforming) beam_.reset();
    for (Slot& slot : slots_) {
        slot.last_fire = -1e9;
        slot.last_end = -1;
        slot.decode_start = 0;
        slot.waiting_for_boundary = false;
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
    if (finished_) return ErrorInfo::error(ErrorCode::NOT_STARTED, "Input already finished; reset first");
    if (audio.isEmpty()) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "Empty audio chunk");
    }
    if (audio.sample_rate != config_.sample_rate) {
        return ErrorInfo::error(ErrorCode::UNSUPPORTED_SAMPLE_RATE,
                                "Sample rate does not match the configured one",
                                std::to_string(audio.sample_rate));
    }
    const int channels = audio.num_channels;
    if (channels != config_.num_channels) {
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER,
                                "Channel count does not match the configured one",
                                std::to_string(channels));
    }
    if (audio.num_samples > std::numeric_limits<size_t>::max() / sizeof(float) / channels ||
        audio.num_samples > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - input_samples_))
        return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "Audio length overflows");
    const size_t count = audio.num_samples * channels;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(audio.data[i]) || std::fabs(audio.data[i]) > 1.0f)
            return ErrorInfo::error(ErrorCode::INVALID_PARAMETER, "Audio must be finite float in [-1, 1]");
    }

    const auto t0 = std::chrono::steady_clock::now();
    const size_t result_start = results.size();
    input_samples_ += audio.num_samples;
    // 调用方可以送任意长度，这里攒够一个 10 ms hop 再往下走。
    const size_t stride = (size_t)kHop * channels;
    size_t offset = 0;
    while (offset < count) {
        if (pending_.empty() && count - offset >= stride) {
            processHop(audio.data + offset, results);
            offset += stride;
        } else {
            const size_t take = std::min(stride - pending_.size(), count - offset);
            pending_.insert(pending_.end(), audio.data + offset, audio.data + offset + take);
            offset += take;
            if (pending_.size() == stride) {
                processHop(pending_.data(), results);
                pending_.clear();
            }
        }
    }

    const int elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    for (size_t i = result_start; i < results.size(); ++i) results[i].processing_time_ms = elapsed_ms;
    return ErrorInfo::ok();
}

ErrorInfo CfsmnBackend::finish(std::vector<DetectionResult>& results) {
    if (!initialized_) return ErrorInfo::error(ErrorCode::NOT_INITIALIZED, "Backend not initialized");
    if (finished_) return ErrorInfo::ok();
    finished_ = true;
    // Drain only the beamformer's known delay. Fbank keeps snip_edges=True and
    // context expansion drops the last two fbank frames, as in training.
    size_t remaining = pending_.size() / config_.num_channels;
    if (config_.use_beamforming && input_samples_ > 0)
        remaining += frontend::Beamformer::kN - frontend::Beamformer::kHop;
    while (remaining > 0) {
        pending_.resize((size_t)kHop * config_.num_channels, 0.0f);
        const int n = static_cast<int>(std::min<size_t>(remaining, kHop));
        processHop(pending_.data(), results, n);
        pending_.clear();
        remaining -= n;
    }
    std::vector<float> tail;
    stream_.finish(tail);
    for (size_t i = 0; i < tail.size(); i += model_.odim) onLogits(tail.data() + i, results);
    if (nposts_ > 0 && model_frames_ % config_.score_interval != 0) scoreFrame(results);
    return ErrorInfo::ok();
}

void CfsmnBackend::processHop(const float* interleaved, std::vector<DetectionResult>& results,
                            int output_samples) {
    const int channels = config_.num_channels;
    const int first = config_.beam_first_channel;
    const int source = source_channel_;
    // 模型是在 int16 量级的 fbank 上训练的，这里把 [-1, 1] 还原回去。
    if (config_.use_beamforming) {
        for (int i = 0; i < kHop; ++i)
            for (int c = 0; c < frontend::Beamformer::kCh; ++c)
                chan_[i * frontend::Beamformer::kCh + c] =
                    interleaved[(size_t)i * channels + first + c] * 32768.0f;
        beam_.process(chan_.data(), beamed_.data());
    } else {
        for (int i = 0; i < kHop; ++i)
            beamed_[i] = interleaved[(size_t)i * channels + source] * 32768.0f;
    }
    fb_new_.clear();
    fbank_.feed(beamed_.data(), output_samples, fb_new_);
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
    const long need = 3 * i - 2;
    for (int k = 0; k < 5; ++k) {
        const size_t base = static_cast<size_t>(std::max<long>(0, need + k) - fb_idx_.front());
        std::memcpy(&feat_[(size_t)k * frontend::Fbank::kBins], fb_hist_[base].data(),
                    sizeof(float) * frontend::Fbank::kBins);
    }

    if (!stream_.push(feat_.data(), logits_.data())) return;   // 流水线还在填充
    onLogits(logits_.data(), results);
}

void CfsmnBackend::onLogits(const float* logits, std::vector<DetectionResult>& results) {
    for (Slot& slot : slots_) {
        auto& frame = slot.ring[nposts_ % config_.decode_context];
        slot.decoder.candidates(logits, model_.odim, frame);
        if (slot.waiting_for_boundary) {
            int best = 0;
            for (int k = 1; k < frame.n; ++k)
                if (frame.prob[k] > frame.prob[best]) best = k;
            // A sustained final token still belongs to the consumed detection.
            // Rearm on blank or another token, without resetting the acoustic model.
            if (frame.n > 0 && frame.idx[best] == slot.decoder.keyword().back()) frame.n = 0;
            else slot.waiting_for_boundary = false;
        }
    }
    ++nposts_;
    ++model_frames_;
    if (model_frames_ % config_.score_interval == 0) scoreFrame(results);
}

namespace {

// Characters [start, start + len) of a keyword whose characters map 1:1 to its
// tokens; the whole text otherwise.
std::string utf8Slice(const std::string& text, int start, int len, size_t tokens) {
    std::vector<size_t> at;
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) at.push_back(i);
    if (at.size() != tokens) return text;
    at.push_back(text.size());
    return text.substr(at[start], at[start + len] - at[start]);
}

}  // namespace

void CfsmnBackend::scoreFrame(std::vector<DetectionResult>& results) {
    const int n = (int)std::min<long>(nposts_, config_.decode_context);
    const long start = nposts_ - n;
    const int beam_delay_ms = config_.use_beamforming ? 22 : 0;
    const auto frameTime = [&](long frame) {
        return std::clamp<int64_t>(frame * kFrameMs + 25 - beam_delay_ms,
                                    0, input_samples_ / 16);
    };
    const double t = (nposts_ - 1) * (kFrameMs / 1000.0);

    DetectionResult best;
    best.timestamp_ms = frameTime(nposts_ - 1);
    bool fired = false;

    for (size_t s = 0; s < slots_.size(); ++s) {
        Slot& slot = slots_[s];
        const long begin = std::max(start, slot.decode_start);
        const int count = static_cast<int>(nposts_ - begin);
        for (int k = 0; k < count; ++k)
            slot.order[k] = slot.ring[(begin + k) % config_.decode_context];
        int end = -1;
        const int min_end = (int)std::max<int64_t>(-1, slot.last_end - begin);
        const float score = slot.decoder.score(slot.order.data(), count, slot.threshold, min_end, &end);
        // just_ended: the last token may still be sounding, so skip its tail. A
        // settled partial fires long after its end, when new frames already
        // belong to the next call.
        // Consume the detected tokens so they cannot mask a new occurrence or
        // fire again when their alignment shifts in the sliding window. Stop
        // at the last detected token: frames after it may already hold the
        // start of the next call, which fast callers begin right away.
        const auto consume = [&](int64_t end_at, bool just_ended) {
            slot.last_end = end_at;
            slot.decode_start = end_at + 1;
            slot.waiting_for_boundary = just_ended;
        };
        // A confusable word fits the detected stretch better: drop it, without
        // the holdoff, so a real call right after it still fires.
        const auto rivaled = [&](float value, const std::string& text) {
            cfsmn::Rivalry why;
            int contested = 0;
            const int beaten = slot.decoder.rivaled(slot.order.data(), count, &why, &contested);
            if (beaten == 0) return false;
            std::string word = "?";
            for (const Keyword& r : slot.rivals) {
                const auto& kw = slot.decoder.keyword();
                for (size_t i = 0; i < kw.size(); ++i)
                    if (kw[i] == why.token && r.token_ids[i] == why.rival) word = r.text;
            }
            fprintf(stderr, "[KWS] %s %.3f rejected: %s fits better at %d/%d (token %d %.2f > %d %.2f)\n",
                    text.c_str(), value, word.c_str(), beaten, contested, why.rival, why.rival_prob, why.token,
                    why.prob);
            return true;
        };
        const auto fire = [&](int64_t end_at, float value, std::string text, bool just_ended) {
            consume(end_at, just_ended);
            if (rivaled(value, text)) return;
            slot.last_fire = t;
            DetectionResult r;
            r.score = value;
            r.is_wake_word = true;
            r.keyword_index = (int)s;
            r.keyword = std::move(text);
            r.timestamp_ms = frameTime(end_at);
            results.push_back(r);
            fired = true;
        };
        const int64_t end_frame = begin + end;
        if (end >= 0 && score >= slot.threshold && end_frame > slot.last_end &&
            t - slot.last_fire >= config_.holdoff_ms / 1000.0) {
            fire(end_frame, score, slot.text, true);
            continue;
        }
        if (score > best.score) best.score = score;
        if (config_.partial_threshold <= 0.0f) continue;
        // Fast calls often decode as only part of the keyword (one "小进" of
        // "小进小进"). Once such a part has settled without the full keyword
        // following, accept it against its own, stricter threshold.
        int pend = -1, kw_start = 0, len = 0;
        const int settle = config_.partial_wait_ms / kFrameMs;
        const float part = slot.decoder.partial(config_.partial_min_tokens, min_end, count - 1 - settle,
                                                &pend, &kw_start, &len);
        // The other half of an already reported call ends shortly after it:
        // drop it for good rather than just delaying it.
        const int64_t part_end = begin + pend;
        if (pend >= 0 && part >= config_.partial_threshold &&
            part_end - slot.last_end >= config_.partial_holdoff_ms / kFrameMs &&
            t - slot.last_fire >= config_.holdoff_ms / 1000.0) {
            fire(part_end, part, utf8Slice(slot.text, kw_start, len, slot.decoder.keyword().size()), false);
        }
    }
    // 没有命中时也上报一次当前最高分，调阈值和看日志都要靠它。
    if (!fired) results.push_back(best);
}

}  // namespace kws
