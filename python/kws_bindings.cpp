/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Python Bindings
 *
 * 用 pybind11 暴露 KwsEngine，音频用 numpy 数组传入（float32，[-1, 1]，多通道交织）。
 */

#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

#include "kws_service.h"

namespace py = pybind11;
using SpacemiT::KwsBackendType;
using SpacemiT::KwsConfig;
using SpacemiT::KwsEngine;
using SpacemiT::KwsEngineCallback;
using SpacemiT::KwsKeyword;
using SpacemiT::KwsResult;

namespace {

// Destruction joins native threads which may be acquiring the Python GIL.
struct EngineDeleter {
    void operator()(KwsEngine* engine) const {
        if (PyGILState_Check()) { py::gil_scoped_release release; delete engine; }
        else delete engine;
    }
};
using EngineHolder = std::unique_ptr<KwsEngine, EngineDeleter>;

// numpy → (指针, 单通道采样点数)。接受 (N,) 与 (N, C) 两种形状。
std::pair<const float*, size_t> asAudio(const py::array_t<float, py::array::c_style |
                                                            py::array::forcecast>& audio,
                                        int channels) {
    auto buf = audio.request();
    if (buf.ndim != 1 && buf.ndim != 2) {
        throw std::invalid_argument("audio must be 1-D (interleaved) or 2-D (frames, channels)");
    }
    const size_t total = (size_t)buf.size;
    if (channels < 1) throw std::invalid_argument("configured channel count must be positive");
    if (buf.ndim == 2 && buf.shape[1] != channels) {
        throw std::invalid_argument("audio.shape[1] must equal the configured channel count");
    }
    if (total % (size_t)channels != 0) {
        throw std::invalid_argument("audio length is not a multiple of the channel count");
    }
    return {static_cast<const float*>(buf.ptr), total / (size_t)channels};
}

// 回调蹦床：Python 侧继承 KwsCallback 覆盖 on_* 方法。
class PyKwsCallback : public KwsEngineCallback {
public:
    using KwsEngineCallback::KwsEngineCallback;

    void OnOpen() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_open", OnOpen);
    }

    void OnEvent(std::shared_ptr<KwsResult> result) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_event", OnEvent, result);
    }

    void OnWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_wake_word", OnWakeWord,
                                keyword, score, timestamp_ms);
    }

    void OnComplete() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_complete", OnComplete);
    }

    void OnError(const std::string& message) override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_error", OnError, message);
    }

    void OnClose() override {
        py::gil_scoped_acquire gil;
        PYBIND11_OVERRIDE_NAME(void, KwsEngineCallback, "on_close", OnClose);
    }
};

}  // namespace

PYBIND11_MODULE(_spacemit_kws, m) {
    m.doc() = "SpacemiT KWS (keyword spotting) Python bindings";

    // -------------------------------------------------------------------------
    // Enums
    // -------------------------------------------------------------------------
    py::enum_<KwsBackendType>(m, "KwsBackendType")
        .value("CFSMN", KwsBackendType::CFSMN)
        .value("CUSTOM", KwsBackendType::CUSTOM);

    py::enum_<SpacemiT::KwsAudioStatus>(m, "KwsAudioStatus")
        .value("ACCEPTED", SpacemiT::KwsAudioStatus::ACCEPTED)
        .value("EMPTY", SpacemiT::KwsAudioStatus::EMPTY)
        .value("NOT_STARTED", SpacemiT::KwsAudioStatus::NOT_STARTED)
        .value("INVALID_PARAMETER", SpacemiT::KwsAudioStatus::INVALID_PARAMETER)
        .value("QUEUE_FULL", SpacemiT::KwsAudioStatus::QUEUE_FULL)
        .value("BUSY", SpacemiT::KwsAudioStatus::BUSY);
    py::class_<SpacemiT::KwsStreamStats>(m, "KwsStreamStats")
        .def_readonly("accepted_samples", &SpacemiT::KwsStreamStats::accepted_samples)
        .def_readonly("dropped_samples", &SpacemiT::KwsStreamStats::dropped_samples)
        .def_readonly("input_overruns", &SpacemiT::KwsStreamStats::input_overruns)
        .def_readonly("event_overruns", &SpacemiT::KwsStreamStats::event_overruns)
        .def_readonly("queued_blocks", &SpacemiT::KwsStreamStats::queued_blocks);

    // -------------------------------------------------------------------------
    // KwsKeyword
    // -------------------------------------------------------------------------
    py::class_<KwsKeyword>(m, "KwsKeyword")
        .def(py::init<>())
        .def(py::init([](const std::string& text, std::vector<int> ids, float threshold) {
                return KwsKeyword{text, std::move(ids), threshold};
            }),
            py::arg("text"), py::arg("token_ids") = std::vector<int>{},
            py::arg("threshold") = 0.0f)
        .def_readwrite("text", &KwsKeyword::text)
        .def_readwrite("token_ids", &KwsKeyword::token_ids)
        .def_readwrite("threshold", &KwsKeyword::threshold)
        .def("__repr__", [](const KwsKeyword& k) {
            return "<KwsKeyword '" + k.text + "'>";
        });

    // -------------------------------------------------------------------------
    // KwsConfig
    // -------------------------------------------------------------------------
    py::class_<KwsConfig>(m, "KwsConfig")
        .def(py::init<>())
        .def_readwrite("backend", &KwsConfig::backend)
        .def_readwrite("model_dir", &KwsConfig::model_dir)
        .def_readwrite("keywords", &KwsConfig::keywords)
        .def_readwrite("sample_rate", &KwsConfig::sample_rate)
        .def_readwrite("num_channels", &KwsConfig::num_channels)
        .def_readwrite("frame_size", &KwsConfig::frame_size)
        .def_readwrite("use_beamforming", &KwsConfig::use_beamforming)
        .def_readwrite("beam_first_channel", &KwsConfig::beam_first_channel)
        .def_readwrite("threshold", &KwsConfig::threshold)
        .def_readwrite("holdoff_ms", &KwsConfig::holdoff_ms)
        .def_readwrite("decode_context", &KwsConfig::decode_context)
        .def_readwrite("partial_threshold", &KwsConfig::partial_threshold)
        .def_readwrite("partial_wait_ms", &KwsConfig::partial_wait_ms)
        .def_readwrite("score_interval", &KwsConfig::score_interval)
        .def_readwrite("num_threads", &KwsConfig::num_threads)
        .def_static("preset", &KwsConfig::Preset, py::arg("name"))
        .def_static("available_presets", &KwsConfig::AvailablePresets)
        .def("with_model_dir", &KwsConfig::withModelDir, py::arg("model_dir"))
        .def("with_keyword", &KwsConfig::withKeyword, py::arg("text"))
        .def("with_keywords", &KwsConfig::withKeywords, py::arg("keywords"))
        .def("with_threshold", &KwsConfig::withThreshold, py::arg("threshold"))
        .def("with_holdoff", &KwsConfig::withHoldoff, py::arg("holdoff_ms"))
        .def("with_channels", &KwsConfig::withChannels, py::arg("channels"))
        .def("with_beamforming", &KwsConfig::withBeamforming, py::arg("enable"))
        .def("with_score_interval", &KwsConfig::withScoreInterval, py::arg("frames"))
        .def("with_decode_context", &KwsConfig::withDecodeContext, py::arg("frames"))
        .def("with_sample_rate", &KwsConfig::withSampleRate, py::arg("rate"))
        .def("with_num_threads", &KwsConfig::withNumThreads, py::arg("threads"));

    // -------------------------------------------------------------------------
    // KwsResult
    // -------------------------------------------------------------------------
    py::class_<KwsResult, std::shared_ptr<KwsResult>>(m, "KwsResult")
        .def_property_readonly("score", &KwsResult::GetScore)
        .def_property_readonly("is_wake_word", &KwsResult::IsWakeWord)
        .def_property_readonly("keyword", &KwsResult::GetKeyword)
        .def_property_readonly("keyword_index", &KwsResult::GetKeywordIndex)
        .def_property_readonly("timestamp_ms", &KwsResult::GetTimestampMs)
        .def_property_readonly("processing_time_ms", &KwsResult::GetProcessingTimeMs)
        .def_property_readonly("success", &KwsResult::IsSuccess)
        .def_property_readonly("code", &KwsResult::GetCode)
        .def_property_readonly("message", &KwsResult::GetMessage)
        .def("__repr__", [](const KwsResult& r) {
            return "<KwsResult '" + r.GetKeyword() + "' score=" +
                    std::to_string(r.GetScore()) + (r.IsWakeWord() ? " WAKE>" : ">");
        });

    // -------------------------------------------------------------------------
    // KwsCallback
    // -------------------------------------------------------------------------
    py::class_<KwsEngineCallback, PyKwsCallback, std::shared_ptr<KwsEngineCallback>>(
        m, "KwsCallback")
        .def(py::init<>())
        .def("on_open", &KwsEngineCallback::OnOpen)
        .def("on_event", &KwsEngineCallback::OnEvent, py::arg("result"))
        .def("on_wake_word", &KwsEngineCallback::OnWakeWord,
            py::arg("keyword"), py::arg("score"), py::arg("timestamp_ms"))
        .def("on_complete", &KwsEngineCallback::OnComplete)
        .def("on_error", &KwsEngineCallback::OnError, py::arg("message"))
        .def("on_close", &KwsEngineCallback::OnClose);

    // -------------------------------------------------------------------------
    // KwsEngine
    // -------------------------------------------------------------------------
    py::class_<KwsEngine, EngineHolder>(m, "KwsEngine")
        .def(py::init([](const KwsConfig& config) {
                return EngineHolder(new KwsEngine(config));
            }),
            py::arg("config") = KwsConfig::Preset("xiaojin"))
        .def(py::init([](const std::string& preset, const std::string& model_dir) {
                auto config = KwsConfig::Preset(preset);
                if (!model_dir.empty()) config.model_dir = model_dir;
                return EngineHolder(new KwsEngine(config));
            }),
            py::arg("preset"), py::arg("model_dir") = "")
        .def("detect",
            [](KwsEngine& self, const py::array_t<float, py::array::c_style |
                                                    py::array::forcecast>& audio,
                int sample_rate) {
                const auto [ptr, samples] = asAudio(audio, self.GetNumChannels());
                py::gil_scoped_release release;
                return self.Detect(ptr, samples, sample_rate);
            },
            py::arg("audio"), py::arg("sample_rate") = 16000,
            "整段检测，返回其中最高的一次打分")
        .def("set_callback", &KwsEngine::SetCallback, py::arg("callback"), py::keep_alive<1, 2>())
        .def("start", &KwsEngine::Start, py::call_guard<py::gil_scoped_release>())
        .def("send_audio_frame",
            [](KwsEngine& self, const py::array_t<float, py::array::c_style |
                                                    py::array::forcecast>& audio) {
                const auto [ptr, samples] = asAudio(audio, self.GetNumChannels());
                return self.SendAudioFrame(ptr, samples);
            },
            py::arg("audio"))
        .def("stop", &KwsEngine::Stop, py::call_guard<py::gil_scoped_release>())
        .def("reset", &KwsEngine::Reset, py::call_guard<py::gil_scoped_release>())
        .def("set_threshold", &KwsEngine::SetThreshold, py::arg("threshold"))
        .def("get_stream_stats", &KwsEngine::GetStreamStats)
        .def("get_config", &KwsEngine::GetConfig)
        .def("get_keywords", &KwsEngine::GetKeywords)
        .def_property_readonly("initialized", &KwsEngine::IsInitialized)
        .def_property_readonly("streaming", &KwsEngine::IsStreaming)
        .def_property_readonly("last_error", &KwsEngine::GetLastError)
        .def_property_readonly("engine_name", &KwsEngine::GetEngineName)
        .def_property_readonly("backend_type", &KwsEngine::GetBackendType)
        .def_property_readonly("last_score", &KwsEngine::GetLastScore)
        .def_property_readonly("lookahead_ms", &KwsEngine::GetLookaheadMs);

    // -------------------------------------------------------------------------
    // 快捷函数
    // -------------------------------------------------------------------------
    m.def("detect",
        [](const py::array_t<float, py::array::c_style | py::array::forcecast>& audio,
            const std::string& keyword, const std::string& model_dir, float threshold,
            int channels, bool beamforming, int sample_rate) {
            auto config = KwsConfig::Preset(beamforming ? "xiaojin-4mic" : "xiaojin");
            if (!keyword.empty()) config = config.withKeyword(keyword);
            if (!model_dir.empty()) config = config.withModelDir(model_dir);
            config = config.withThreshold(threshold).withChannels(channels);
            config.use_beamforming = beamforming;
            auto engine = std::make_shared<KwsEngine>(config);
            if (!engine->IsInitialized()) {
                throw std::runtime_error("KWS engine not initialized: " + engine->GetLastError());
            }
            const auto [ptr, samples] = asAudio(audio, channels);
            py::gil_scoped_release release;
            return engine->Detect(ptr, samples, sample_rate);
        },
        py::arg("audio"), py::arg("keyword") = "", py::arg("model_dir") = "",
        py::arg("threshold") = 0.3f, py::arg("channels") = 1,
        py::arg("beamforming") = false, py::arg("sample_rate") = 16000,
        "在一段音频里找关键词，返回 KwsResult");

    m.attr("__version__") = "1.0.0";
    m.attr("__author__") = "SpacemiT";
}
