/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * KWS Backend Factory Implementation
 */

#include <iostream>
#include <memory>
#include <vector>

#include "backends/cfsmn/cfsmn_backend.hpp"
#include "backends/kws_backend.hpp"

namespace kws {

std::unique_ptr<IKwsBackend> KwsBackendFactory::create(BackendType type) {
    switch (type) {
        case BackendType::CFSMN:
            return std::make_unique<CfsmnBackend>();

        case BackendType::CUSTOM:
            std::cerr << "Custom backend requires user implementation" << std::endl;
            return nullptr;

        default:
            std::cerr << "Unknown backend type: " << static_cast<int>(type) << std::endl;
            return nullptr;
    }
}

bool KwsBackendFactory::isAvailable(BackendType type) {
    switch (type) {
        case BackendType::CFSMN:
            return true;   // 纯 C++，无外部依赖，始终可用
        case BackendType::CUSTOM:
        default:
            return false;
    }
}

std::vector<BackendType> KwsBackendFactory::getAvailableBackends() {
    return {BackendType::CFSMN};
}

int KwsBackendFactory::getDefaultSampleRate(BackendType type) {
    (void)type;
    return 16000;
}

int KwsBackendFactory::getRecommendedFrameSize(BackendType type) {
    switch (type) {
        case BackendType::CFSMN:
            return CfsmnBackend::kHop;   // 160 样本 = 10 ms
        default:
            return 160;
    }
}

}  // namespace kws
