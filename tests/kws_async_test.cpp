/* Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kws_service.h"
#include "backends/kws_backend.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>

// Count allocations on the producer only; worker/event allocations are permitted.
thread_local bool watch_allocations = false;
thread_local size_t producer_allocations = 0;
void* operator new(size_t size) {
    if (watch_allocations) ++producer_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
template<class F> void until(F predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    std::atomic<bool> entered{false};
    void wait() { std::unique_lock<std::mutex> lock(mutex); entered = true; cv.wait(lock, [&] { return released; }); }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct State { Gate inference; std::atomic<int> processed{0}, resets{0}; bool block = false; };
State* state;
class Backend : public kws::IKwsBackend {
    State* s_ = state;
    int64_t clock_ = 0;
public:
    kws::ErrorInfo initialize(const kws::KwsConfig& c) override { config_ = c; return kws::ErrorInfo::ok(); }
    void shutdown() override {}
    bool isInitialized() const override { return true; }
    kws::BackendType getType() const override { return kws::BackendType::CUSTOM; }
    std::string getName() const override { return "controlled async test backend"; }
    void reset() override { ++s_->resets; clock_ = 0; }
    kws::ErrorInfo process(const kws::AudioChunk& a, std::vector<kws::DetectionResult>& out) override {
        if (s_->block) s_->inference.wait();
        clock_ += a.num_samples;
        kws::DetectionResult result;
        result.keyword = "test"; result.score = 0.9f; result.is_wake_word = true;
        result.timestamp_ms = clock_ / 16;
        out.push_back(result); ++s_->processed;
        return kws::ErrorInfo::ok();
    }
};
struct Callback : SpacemiT::KwsEngineCallback {
    std::function<void()> open, event;
    std::atomic<int> opens{0}, closes{0}, errors{0}, wakes{0};
    void OnOpen() override { ++opens; if (open) open(); }
    void OnEvent(std::shared_ptr<SpacemiT::KwsResult>) override { if (event) event(); }
    void OnWakeWord(const std::string&, float, int64_t) override { ++wakes; }
    void OnError(const std::string&) override { ++errors; }
    void OnClose() override { ++closes; }
};
using SpacemiT::KwsAudioStatus;
void send(SpacemiT::KwsEngine& engine) {
    float samples[160] = {};
    until([&] { return engine.GetStreamStats().queued_blocks < 128; }, "inference drains its queue");
    require(engine.SendAudioFrame(samples, 160) == KwsAudioStatus::ACCEPTED, "accept frame");
}
void blockedInference() {
    State s; s.block = true; state = &s;
    SpacemiT::KwsEngine engine{SpacemiT::KwsConfig{}};
    auto cb = std::make_shared<Callback>(); engine.SetCallback(cb);
    require(engine.Start(), "start");
    float first[160] = {};
    watch_allocations = true;
    const auto first_status = engine.SendAudioFrame(first, 160);
    watch_allocations = false;
    require(first_status == KwsAudioStatus::ACCEPTED && producer_allocations == 0, "first send also has no allocations");
    until([&] { return s.inference.entered.load(); }, "backend is deliberately blocked");
    float samples[160] = {};
    watch_allocations = true;
    int full = 0;
    for (int i = 0; i < 300; ++i)
        if (engine.SendAudioFrame(samples, 160) == KwsAudioStatus::QUEUE_FULL) ++full;
    const auto empty = engine.SendAudioFrame(nullptr, 0);
    const auto invalid = engine.SendAudioFrame(nullptr, 1);
    watch_allocations = false;
    require(full > 0 && producer_allocations == 0, "producer never waits or allocates, even with blocked inference/full queue");
    require(empty == KwsAudioStatus::EMPTY && invalid == KwsAudioStatus::INVALID_PARAMETER, "input status");
    require(engine.IsStreaming(), "overrun does not kill stream");
    s.inference.release();
    until([&] { return engine.GetStreamStats().queued_blocks == 0; }, "old audio drains");
    send(engine); engine.Stop();
    require(s.resets >= 2 && cb->closes == 1 && cb->errors > 0, "gap resets state and reports overrun; one Close");
    require(engine.GetStreamStats().input_overruns == static_cast<unsigned>(full), "overrun accounting");
    std::cout << "blocked inference: zero producer allocations, bounded queue, gap recovery OK\n";
}
void blockedCallback() {
    State s; state = &s;
    SpacemiT::KwsEngine engine{SpacemiT::KwsConfig{}};
    Gate callback_gate;
    auto cb = std::make_shared<Callback>(); cb->event = [&] { callback_gate.wait(); };
    engine.SetCallback(cb); require(engine.Start(), "start blocked callback"); send(engine);
    until([&] { return callback_gate.entered.load(); }, "callback is deliberately blocked");
    for (int i = 0; i < 400; ++i) send(engine);
    until([&] { return s.processed >= 401; }, "inference progresses while callback remains blocked");
    require(engine.GetStreamStats().event_overruns > 0, "event queue is bounded and overflow observable");
    callback_gate.release(); engine.Stop();
    require(cb->opens == 1 && cb->closes == 1, "lifecycle survives event saturation");
    std::cout << "blocked callback: inference continues, bounded event queue, shutdown OK\n";
}
void reentrantStopAndRace() {
    State s; state = &s;
    SpacemiT::KwsEngine engine{SpacemiT::KwsConfig{}};
    auto cb = std::make_shared<Callback>(); cb->open = [&] { engine.Stop(); };
    engine.SetCallback(cb); require(engine.Start(), "OnOpen Stop cannot turn successful Start into false");
    engine.Stop(); require(cb->closes == 1, "OnOpen Stop closes once");
    cb->open = {}; Gate gate; std::atomic<bool> once{false};
    cb->event = [&] { if (!once.exchange(true)) { gate.wait(); engine.Reset(); engine.Stop(); } };
    require(engine.Start(), "restart"); send(engine);
    until([&] { return gate.entered.load(); }, "entered reentrant callback");
    std::thread stop([&] { engine.Stop(); });
    until([&] { return !engine.IsStreaming(); }, "external Stop finishes inference without waiting under a control lock");
    gate.release(); stop.join();
    require(cb->closes == 2, "concurrent external and reentrant Stop close once");
    for (int round = 0; round < 20; ++round) {
        require(engine.Start(), "stress restart");
        std::thread producer([&] { float x[160] = {}; for (int i = 0; i < 200; ++i) engine.SendAudioFrame(x, 160); });
        for (int i = 0; i < 4; ++i) engine.Reset();
        engine.Stop(); producer.join();
    }
    std::cout << "OnOpen/reentrant/concurrent Stop, Reset and restart: OK\n";
}
}  // namespace
namespace kws {
// Link this controlled factory instead of the production factory for fault injection.
std::unique_ptr<IKwsBackend> KwsBackendFactory::create(BackendType) { return std::make_unique<Backend>(); }
}
int main() { blockedInference(); blockedCallback(); reentrantStopAndRace(); }
