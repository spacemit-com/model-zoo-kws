#!/usr/bin/env bash
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#

set -euo pipefail

module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-${module_dir}/build/test-artifacts}"
build_dir="${artifact_dir}/python"

cmake -S "${module_dir}" -B "${build_dir}" \
  -DBUILD_KWS_PYTHON=ON -DBUILD_KWS_EXAMPLES=OFF -DBUILD_KWS_TESTS=ON \
  -DBUILD_KWS_MICROPHONE=OFF \
  -DKWS_MODEL_FETCH_OFF=ON -DCMAKE_BUILD_TYPE=Release
# Without pybind11 the configure step drops the bindings; the explicit target
# and --no-tests=error make that a failure instead of an empty pass.
cmake --build "${build_dir}" --target _spacemit_kws --parallel 2
ctest --test-dir "${build_dir}" -R '^kws-python-bindings$' --no-tests=error --output-on-failure
