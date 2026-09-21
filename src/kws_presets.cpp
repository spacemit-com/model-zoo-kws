/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kws_service.h"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace SpacemiT {

static const std::map<std::string, std::function<KwsConfig()>>& getPresets() {
    static const std::map<std::string, std::function<KwsConfig()>> presets = {
        // 单通道：调用方自己送 16 kHz 单声道（例如 AEC 之后的信号）。
        {"xiaojin", []() {
            KwsConfig config;
            config.backend = KwsBackendType::CFSMN;
            config.model_dir = "~/.cache/models/kws/xiaojin";
            config.keywords = {KwsKeyword{"小进小进", {}, 0.0f}};
            return config;
        }},
        // SPV 复合设备：4 通道交织，ch0 为板端处理结果，ch1~ch3 走固定波束。
        {"xiaojin-4mic", []() {
            KwsConfig config;
            config.backend = KwsBackendType::CFSMN;
            config.model_dir = "~/.cache/models/kws/xiaojin";
            config.keywords = {KwsKeyword{"小进小进", {}, 0.0f}};
            config.num_channels = 4;
            config.use_beamforming = true;
            config.beam_first_channel = 1;
            return config;
        }},
    };
    return presets;
}

KwsConfig KwsConfig::Preset(const std::string& name) {
    const auto& presets = getPresets();
    auto it = presets.find(name);
    if (it == presets.end()) {
        throw std::invalid_argument("Unknown KWS preset: '" + name + "'");
    }
    return it->second();
}

std::vector<std::string> KwsConfig::AvailablePresets() {
    const auto& presets = getPresets();
    std::vector<std::string> names;
    names.reserve(presets.size());
    for (const auto& [name, _] : presets) {
        names.push_back(name);
    }
    return names;
}

}  // namespace SpacemiT
