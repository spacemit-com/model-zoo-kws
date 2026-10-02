#!/usr/bin/env bash
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-${module_dir}/build/test-artifacts}"
cmake -S "${module_dir}" -B "${artifact_dir}/inference" \
  -DBUILD_KWS_PYTHON=OFF -DBUILD_KWS_EXAMPLES=ON -DBUILD_KWS_TESTS=ON \
  -DBUILD_KWS_MICROPHONE=OFF \
  -DKWS_MODEL_FETCH_OFF=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "${artifact_dir}/inference" --parallel 2
ctest --test-dir "${artifact_dir}/inference" --output-on-failure
