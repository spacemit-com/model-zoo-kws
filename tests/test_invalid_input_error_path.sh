#!/usr/bin/env bash
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#

set -euo pipefail

module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-${module_dir}/build/test-artifacts}"
binary="${artifact_dir}/kws_pr_contract_test"
cxx="${CXX:-c++}"

mkdir -p "${artifact_dir}"

"${cxx}" -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-parameter \
  -I"${module_dir}/include" \
  -I"${module_dir}/src" \
  "${module_dir}/tests/kws_pr_contract_test.cpp" \
  "${module_dir}/src/kws_engine.cpp" \
  "${module_dir}/src/kws_presets.cpp" \
  "${module_dir}/src/kws_backend_factory.cpp" \
  "${module_dir}/src/backends/cfsmn/cfsmn_backend.cpp" \
  "${module_dir}/src/backends/cfsmn/fft.cpp" \
  "${module_dir}/src/backends/cfsmn/fbank.cpp" \
  "${module_dir}/src/backends/cfsmn/beam.cpp" \
  "${module_dir}/src/backends/cfsmn/fsmn.cpp" \
  "${module_dir}/src/backends/cfsmn/ctc.cpp" \
  -o "${binary}"

"${binary}" --invalid-input-error-path
