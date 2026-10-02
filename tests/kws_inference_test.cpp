/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/cfsmn/cfsmn_backend.hpp"
#include "kws_service.h"

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    std::string dir;
    std::vector<char> original;

    Fixture() {
        char pattern[] = "/tmp/kws-test-XXXXXX";
        require(mkdtemp(pattern) != nullptr, "create temporary model directory");
        dir = pattern;
        const int dims[] = {400, 6, 5, 3, 3, 2, 4, 4, 3};
        const int count = 800 + 6 * 400 + 6 + 5 * 6 + 5 + 4 * (3 * 5 + 3 * 3 + 3 * 2 + 5 * 3 + 5)
                            + 4 * 5 + 4 + 3 * 4 + 3;
        std::ofstream out(dir + "/cfsmn.bin", std::ios::binary);
        out.write("KWSF", 4);
        out.write(reinterpret_cast<const char*>(dims), sizeof(dims));
        std::vector<float> weights(count);
        for (int i = 0; i < count; ++i) weights[i] = 0.03f * std::sin(i * 0.17f);
        std::fill(weights.begin(), weights.begin() + 400, 0.0f);
        std::fill(weights.begin() + 400, weights.begin() + 800, 1.0f);
        weights[count - 3] = 0.0f;
        weights[count - 2] = 4.0f;
        weights[count - 1] = -4.0f;
        out.write(reinterpret_cast<const char*>(weights.data()), weights.size() * sizeof(float));
        out.close();
        std::ifstream in(dir + "/cfsmn.bin", std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), {});
        std::ofstream(dir + "/keywords.txt") << "test 1\n";
        // Identity beam on the first raw microphone.
        const int beam_dims[] = {257, 3};
        std::ofstream beam(dir + "/beam_w.bin", std::ios::binary);
        beam.write(reinterpret_cast<const char*>(beam_dims), sizeof(beam_dims));
        std::vector<float> w(257 * 3 * 2, 0.0f);
        for (int i = 0; i < 257; ++i) w[i * 6] = 1.0f;
        beam.write(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(float));
    }

    ~Fixture() {
        for (const auto* file : {"cfsmn.bin", "beam_w.bin", "keywords.txt"})
            std::remove((dir + "/" + file).c_str());
        rmdir(dir.c_str());
    }

    void write(const std::vector<char>& bytes) const {
        std::ofstream out(dir + "/cfsmn.bin", std::ios::binary);
        out.write(bytes.data(), bytes.size());
    }

    SpacemiT::KwsConfig config() const {
        SpacemiT::KwsConfig c;
        c.model_dir = dir;
        return c;
    }
};

struct Recording : SpacemiT::KwsEngineCallback {
    std::vector<std::string> order;
    std::vector<std::shared_ptr<SpacemiT::KwsResult>> results;
    std::function<void()> action;
    std::atomic<int> wake_count{0}, error_count{0};
    void OnOpen() override { order.push_back("open"); }
    void OnEvent(std::shared_ptr<SpacemiT::KwsResult> r) override {
        order.push_back("event");
        results.push_back(r);
        if (action) {
            auto once = std::move(action);
            action = nullptr;
            once();
        }
    }
    void OnWakeWord(const std::string&, float, int64_t) override { order.push_back("wake"); ++wake_count; }
    void OnComplete() override { order.push_back("complete"); }
    void OnClose() override { order.push_back("close"); }
    void OnError(const std::string&) override { order.push_back("error"); ++error_count; }
};

template<class Predicate> void waitFor(Predicate ready, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void feed(SpacemiT::KwsEngine& e, const float* data, size_t samples) {
    const size_t channels = e.GetConfig().num_channels;
    while (samples) {
        const size_t count = std::min<size_t>(samples, 16000);
        waitFor([&] { return e.GetStreamStats().queued_blocks + (count + 159) / 160 <= 128; }, "audio queue drains");
        require(e.SendAudioFrame(data, count) == SpacemiT::KwsAudioStatus::ACCEPTED, "queue accepts complete input");
        samples -= count;
        data += count * channels;
    }
}
void feed(SpacemiT::KwsEngine& e, const std::vector<float>& audio) {
    feed(e, audio.data(), audio.size() / e.GetConfig().num_channels);
}

void testCtc() {
    using kws::cfsmn::is_sublist;
    require(is_sublist({2, 1, 2, 1, 2}, {1, 2, 1, 2}) == 1, "CTC must match terminal suffix");
    require(is_sublist({1}, {}) == -1, "empty keyword must not match");
    kws::cfsmn::CtcKeywordDecoder d;
    d.set_keyword({1, 2, 1, 2});
    kws::cfsmn::FrameCand frames[5]{};
    for (int i = 0; i < 5; ++i) {
        frames[i].n = 1;
        frames[i].idx[0] = i % 2 == 0 ? 2 : 1;
        frames[i].prob[0] = 0.99f;
    }
    int end = -1;
    require(std::fabs(d.score(frames, 5, &end) - 0.9801f) < 1e-6f && end == 4,
            "CTC score and end frame for terminal suffix");

    // A weak call followed by a strong one, separated by blanks: the weak one
    // must not mask the strong one while both are in the window.
    kws::cfsmn::FrameCand two[9]{};
    const int ids[9] = {1, 2, 1, 2, 0, 1, 2, 1, 2};
    for (int i = 0; i < 9; ++i) {
        two[i].n = 1;
        two[i].idx[0] = ids[i];
        two[i].prob[0] = i < 4 ? 0.3f : 0.99f;
    }
    const float later = d.score(two, 9, 0.3f, -1, &end);
    require(end == 8 && std::fabs(later - 0.9801f) < 1e-5f,
            "strong later call must fire past a weak earlier one with its own score");
    require(d.score(two, 9, 0.99f, 8, &end) < 0.99f && end == -1,
            "occurrences ending at or before min_end are ignored");

    // Confusable word 1 3 1 3: token 3 rivals keyword token 2.
    require(d.set_rivals({{1, 3, 1, 3}, {1, 2}}), "rival groups");
    const float logits[4] = {0.0f, 0.0f, 1.0f, 2.0f};
    kws::cfsmn::FrameCand c;
    d.candidates(logits, 4, c);
    const double z = 2.0 + std::exp(1.0) + std::exp(2.0);
    require(c.rival_idx[0] == 3 && std::fabs(c.rival[0] - std::exp(2.0) / z) < 1e-6,
            "rival posterior is taken from the full softmax, outside the keyword token set");
    kws::cfsmn::FrameCand rival[5]{};
    for (int i = 0; i < 5; ++i) {
        rival[i].n = 1;
        rival[i].idx[0] = i % 2 == 0 ? 2 : 1;
        rival[i].prob[0] = i % 2 == 0 ? 0.5f : 0.99f;
    }
    rival[1].rival[0] = 0.9f;                      // next to the node of the first 2 (frame 2) only
    rival[1].rival_idx[0] = 3;
    kws::cfsmn::Rivalry why;
    int contested = 0;
    require(d.score(rival, 5, 0.3f, -1, &end) >= 0.3f && d.rivaled(rival, 5, &why, &contested) == 1 &&
            contested == 2 && why.token == 2 && why.rival == 3 && why.frame == 1,
            "a rival beating a keyword token next to its node rejects the detection");
    rival[1].rival[0] = 0.4f;
    require(d.score(rival, 5, 0.3f, -1, &end) >= 0.3f && d.rivaled(rival, 5, &why) == 0,
            "weaker rivals do not reject");
    kws::cfsmn::CtcKeywordDecoder wide;
    wide.set_keyword({1, 2, 3, 4, 5});
    require(!wide.set_rivals({{6, 7, 8, 9, 10}}), "more than kMaxRivalGroups rival groups is rejected");
}

void testModel(const std::string& dir) {
    kws::cfsmn::Model m;
    require(m.load((dir + "/cfsmn.bin").c_str()), "load model");
    float max_error = 0;
    for (int frames : {0, 1, 2, 3, 7, 8, 9, 32}) {
        std::vector<float> x(frames * m.idim), reference(frames * m.odim), actual, one(m.odim);
        for (size_t i = 0; i < x.size(); ++i) x[i] = 10 + 3 * std::sin(i * 0.013f);
        kws::cfsmn::forward_window(m, x.data(), frames, reference.data());
        kws::cfsmn::Stream stream;
        stream.init(m);
        for (int i = 0; i < frames; ++i) {
            if (stream.push(x.data() + i * m.idim, one.data()))
                actual.insert(actual.end(), one.begin(), one.end());
        }
        stream.finish(actual);
        const size_t size = actual.size();
        stream.finish(actual);
        require(actual.size() == size && size == reference.size(), "flush must return exactly T frames once");
        for (size_t i = 0; i < actual.size(); ++i) {
            const float error = std::fabs(actual[i] - reference[i]);
            max_error = std::max(max_error, error);
            require(error <= 1e-5f + std::fabs(reference[i]) * 1e-5f, "stream/window logit mismatch");
        }
    }
    std::cout << "stream/window max logit error: " << max_error << '\n';
}

// Optional independently generated PyTorch features/logits pair (legacy exporter).
void testTorchReference(const std::string& dir, const char* path) {
    kws::cfsmn::Model m;
    require(m.load((dir + "/cfsmn.bin").c_str()), "load reference model");
    std::ifstream in(path, std::ios::binary);
    int shape[2] = {};
    in.read(reinterpret_cast<char*>(shape), sizeof(shape));
    require(in.good() && shape[0] > 0 && shape[0] < 10000 && shape[1] == m.idim, "reference dimensions");
    std::vector<float> x(shape[0] * m.idim), ref(shape[0] * m.odim), actual, frame(m.odim);
    in.read(reinterpret_cast<char*>(x.data()), x.size() * 4);
    in.read(reinterpret_cast<char*>(ref.data()), ref.size() * 4);
    require(in.good() && in.peek() == EOF, "reference size");
    kws::cfsmn::Stream stream;
    stream.init(m);
    for (int i = 0; i < shape[0]; ++i) {
        if (stream.push(x.data() + i * m.idim, frame.data()))
            actual.insert(actual.end(), frame.begin(), frame.end());
    }
    stream.finish(actual);
    require(actual.size() == ref.size(), "PyTorch frame count");
    float max_error = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const float error = std::fabs(actual[i] - ref[i]);
        max_error = std::max(max_error, error);
        require(error <= 5e-4f + std::fabs(ref[i]) * 2e-5f, "PyTorch logit mismatch");
    }
    std::cout << "PyTorch max logit error: " << max_error << " over " << shape[0] << " frames\n";
}

void testInvalid(Fixture& f) {
    const char* env = std::getenv("KWS_MODEL_DIR");
    const bool had_env = env != nullptr;
    const std::string previous = env ? env : "";
    setenv("KWS_MODEL_DIR", f.dir.c_str(), 1);
    require(SpacemiT::KwsEngine(SpacemiT::KwsConfig::Preset("xiaojin").withKeyword("test")).IsInitialized(),
            "preset honors KWS_MODEL_DIR");
    if (had_env) setenv("KWS_MODEL_DIR", previous.c_str(), 1);
    else unsetenv("KWS_MODEL_DIR");
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto bytes = f.original;
        if (mutation == 0) bytes.resize(20);
        if (mutation == 1) bytes.push_back(0);
        if (mutation == 2) std::fill(bytes.begin() + 4, bytes.begin() + 8, '\xff');
        if (mutation == 3) std::fill(bytes.begin() + 40, bytes.begin() + 44, '\xff');
        if (mutation == 4) bytes.resize(bytes.size() - 4);
        f.write(bytes);
        SpacemiT::KwsEngine e(f.config());
        require(!e.IsInitialized(), "malformed weights must fail");
    }
    f.write(f.original);
    for (int invalid = 0; invalid < 9; ++invalid) {
        auto c = f.config();
        switch (invalid) {
            case 0: c.threshold = std::numeric_limits<float>::quiet_NaN(); break;
            case 1: c.threshold = 1.01f; break;
            case 2: c.holdoff_ms = -1; break;
            case 3: c.num_threads = 2; break;
            case 4: c.decode_context = std::numeric_limits<int>::max(); break;
            case 5: c.beam_first_channel = std::numeric_limits<int>::max(); c.use_beamforming = true; break;
            case 6: c.keywords = {{"blank", {0}, 0}}; break;
            case 7: c.keywords = {{"out-of-range", {3}, 0}}; break;
            case 8: c.backend = static_cast<SpacemiT::KwsBackendType>(99); break;
        }
        SpacemiT::KwsEngine e(c);
        require(!e.IsInitialized(), "invalid configuration must fail");
    }
    std::ofstream(f.dir + "/keywords.txt") << "test 1, 2 0.3 # spaced separator\n";
    require(SpacemiT::KwsEngine(f.config()).IsInitialized(), "comma separators permit surrounding whitespace");
    std::ofstream(f.dir + "/keywords.txt") << "test 1\nbroken\n";
    auto explicit_config = f.config();
    explicit_config.keywords = {{"test", {1}, 0}};
    require(SpacemiT::KwsEngine(explicit_config).IsInitialized(), "explicit IDs ignore unrelated malformed file");
    SpacemiT::KwsEngine malformed(f.config());
    require(!malformed.IsInitialized() && malformed.GetLastError().find(":2:") != std::string::npos,
            "malformed requested file reports its line");
    std::ofstream(f.dir + "/keywords.txt") << "bad 1,oops\n";
    require(!SpacemiT::KwsEngine(f.config()).IsInitialized(), "malformed token must fail");
    std::ofstream(f.dir + "/keywords.txt") << "test 1\n!rival 2\n";
    require(SpacemiT::KwsEngine(f.config()).IsInitialized(), "confusable words load");
    std::ofstream(f.dir + "/keywords.txt") << "test 1\n!rival 2 0.3\n";
    require(!SpacemiT::KwsEngine(f.config()).IsInitialized(), "confusable words take no threshold");
    std::ofstream(f.dir + "/keywords.txt") << "test 1\n!rival 7\n";
    require(!SpacemiT::KwsEngine(f.config()).IsInitialized(), "confusable word tokens must be in the vocabulary");
    std::ofstream(f.dir + "/keywords.txt") << "test 1\n";
    SpacemiT::KwsEngine e(f.config());
    e.SetThreshold(std::numeric_limits<float>::infinity());
    require(e.GetConfig().threshold == 0.3f && !e.GetLastError().empty(), "invalid threshold preserves state");
    e.SetThreshold(0.5f);
    require(e.GetConfig().threshold == 0.5f && e.GetLastError().empty(), "valid threshold clears error");
    auto cb = std::make_shared<Recording>();
    e.SetCallback(cb);
    require(e.Start(), "start");
    e.SendAudioFrame(std::vector<float>{std::numeric_limits<float>::quiet_NaN()});
    waitFor([&] { return cb->error_count > 0; }, "bad sample reports an asynchronous error");
    require(e.IsStreaming(), "bad audio must leave the stream usable");
    require(e.SendAudioFrame(std::vector<float>{}) == SpacemiT::KwsAudioStatus::EMPTY, "empty audio is a no-op");
    feed(e, std::vector<float>(1600, 0.1f));
    require(!e.Detect(std::vector<float>(1600)).get()->IsSuccess() && e.IsStreaming(),
            "Detect must not reset an active stream");
    e.Stop();
    SpacemiT::KwsEngine multi(f.config().withChannels(4));
    require(!multi.Detect(std::vector<float>(641))->IsSuccess(), "interleaved remainder rejected");
    auto multi_cb = std::make_shared<Recording>();
    multi.SetCallback(multi_cb);
    multi.Start();
    require(multi.SendAudioFrame(std::vector<float>(641)) == SpacemiT::KwsAudioStatus::INVALID_PARAMETER,
            "bad vector has a precise immediate status");
    require(multi.IsStreaming(), "bad vector preserves stream");
    multi.Stop();
}

void testEngine(const SpacemiT::KwsConfig& config, const std::vector<float>& audio) {
    SpacemiT::KwsEngine whole(config), split(config);
    require(whole.IsInitialized() && split.IsInitialized(), "engines initialize");
    auto result = whole.Detect(audio);
    require(result->IsSuccess(), "whole detection");
    auto a = std::make_shared<Recording>(), b = std::make_shared<Recording>();
    whole.SetCallback(a);
    split.SetCallback(b);
    whole.Start();
    split.Start();
    feed(whole, audio);
    const size_t frames = audio.size() / config.num_channels;
    for (size_t i = 0; i < frames;) {
        const size_t n = std::min(frames - i, (i * 17) % 521 + 1);
        feed(split, audio.data() + i * config.num_channels, n);
        i += n;
    }
    whole.Stop();
    split.Stop();
    const size_t count = a->results.size();
    whole.Stop();
    require(a->results.size() == count && count == b->results.size(), "idempotent stop/chunk invariance");
    require(a->order.front() == "open" && a->order.back() == "close" &&
            a->order[a->order.size() - 2] == "complete", "callback lifecycle");
    const size_t samples = frames + (config.use_beamforming ? 352 : 0);
    const size_t fb_frames = samples >= 400 ? (samples - 400) / 160 + 1 : 0;
    const size_t model_frames = fb_frames > 2 ? (fb_frames - 3) / 3 + 1 : 0;
    require(count == model_frames, "feature and output frame count includes first and drained frames");
    float best = 0;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        const auto& x = a->results[i];
        const auto& y = b->results[i];
        require(x->GetScore() == y->GetScore() && x->IsWakeWord() == y->IsWakeWord() &&
                x->GetTimestampMs() == y->GetTimestampMs(), "chunking must not affect results");
        require(x->GetTimestampMs() >= 0 && x->GetTimestampMs() <= (int64_t)(frames / 16), "timestamp bounds");
        if (!x->IsWakeWord()) require(x->GetKeywordIndex() == -1, "nonwake index");
        if (x->IsWakeWord() && !found) { found = true; best = 0; }
        if (x->IsWakeWord() == found) best = std::max(best, x->GetScore());
    }
    require(result->GetScore() == best && result->IsWakeWord() == found, "Detect agrees with complete stream");
    std::cout << "audio frames=" << frames << " model frames=" << count << " score=" << result->GetScore()
                << " wake=" << result->IsWakeWord() << " timestamp=" << result->GetTimestampMs() << '\n';
}

void testReentrant(Fixture& f) {
    const std::vector<float> audio(16123, 0.1f);
    for (bool reset : {false, true}) {
        SpacemiT::KwsEngine e(f.config());
        auto cb = std::make_shared<Recording>();
        cb->action = [&]() {
            e.SetThreshold(0.6f);
            e.SetCallback(cb);
            require(e.GetConfig().threshold == 0.6f, "reentrant getter/setter");
            if (reset) e.Reset();
            e.Stop();
        };
        e.SetCallback(cb);
        require(e.Start(), "start reentrant test");
        feed(e, audio);
        e.Stop();
        require(!e.IsStreaming(), "callback Stop returns");
        require(cb->order.back() == "close" && std::count(cb->order.begin(), cb->order.end(), "close") == 1,
                "reentrant close exactly once after results");
        if (reset) require(cb->results.size() == 1, "Reset cancels queued results");
    }
    // Keep the posterior below the trigger threshold so this checks score
    // delivery without consuming the fixture's constant keyword detection.
    SpacemiT::KwsEngine e(f.config().withThreshold(1.0f));
    e.Start();
    feed(e, audio);
    e.Stop();
    require(e.GetLastScore() > 0, "last_score works without callbacks");
}

void testSustainedToken(Fixture& f) {
    auto config = f.config();
    config.holdoff_ms = 0;
    SpacemiT::KwsEngine engine(config);
    auto cb = std::make_shared<Recording>();
    engine.SetCallback(cb);
    require(engine.Start(), "start sustained-token test");
    const std::vector<float> block(160, 0.1f);
    for (int i = 0; i < 600; ++i) feed(engine, block);
    waitFor([&] { return engine.GetStreamStats().queued_blocks == 0 && cb->wake_count == 1; }, "stream processes input");
    require(cb->wake_count == 1,
            "a sustained token must not retrigger as the history window slides");
    engine.Reset();
    for (int i = 0; i < 100; ++i) feed(engine, block);
    engine.Stop();
    require(std::count(cb->order.begin(), cb->order.end(), "wake") == 2,
            "explicit Reset must rearm consumed keyword history");
}

}  // namespace

int main() {
    try {
        Fixture fixture;
        testCtc();
        testModel(fixture.dir);
        testInvalid(fixture);
        testReentrant(fixture);
        testSustainedToken(fixture);
        std::vector<float> audio(16123);
        for (size_t i = 0; i < audio.size(); ++i) audio[i] = 0.1f * std::sin(i * 0.03f);
        testEngine(fixture.config(), audio);
        testEngine(fixture.config(), std::vector<float>(720, 0.1f));
        testEngine(fixture.config(), std::vector<float>(399, 0.1f));
        std::vector<float> multi(audio.size() * 4);
        for (size_t i = 0; i < audio.size(); ++i) multi[i * 4 + 1] = audio[i];
        testEngine(fixture.config().withChannels(4).withBeamforming(true), multi);
        if (const char* model = std::getenv("KWS_TEST_MODEL_DIR")) {
            testModel(model);
            if (const char* ref = std::getenv("KWS_TEST_REFERENCE")) testTorchReference(model, ref);
            if (const char* pcm = std::getenv("KWS_TEST_AUDIO_F32")) {
                std::ifstream in(pcm, std::ios::binary | std::ios::ate);
                require(in.good() && in.tellg() > 0 && in.tellg() % 4 == 0, "open reference audio");
                audio.resize(static_cast<size_t>(in.tellg()) / 4);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(audio.data()), audio.size() * 4);
            }
            SpacemiT::KwsConfig c;
            c.model_dir = model;
            testEngine(c, audio);
        }
        std::cout << "kws inference regression: OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
