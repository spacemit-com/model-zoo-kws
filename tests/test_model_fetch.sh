#!/usr/bin/env bash
#
# Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Runtime model download against a local fixture package (file://, no network), plus a check
# that the runtime default release matches the configure-time pin in cmake/FetchKwsModel.cmake.

set -euo pipefail

module_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_dir="${SROBOTIS_TEST_ARTIFACT_DIR:-${module_dir}/build/test-artifacts}"
binary="${artifact_dir}/model_fetch_test"
work="$(mktemp -d "${TMPDIR:-/tmp}/kws-model-fetch.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

cmake_file="${module_dir}/cmake/FetchKwsModel.cmake"
cmake_name="$(sed -n 's/^set(_KWS_DEFAULT_MODEL_NAME "\(.*\)")$/\1/p' "${cmake_file}")"
cmake_sha="$(sed -n 's/^set(_KWS_DEFAULT_MODEL_SHA256 "\(.*\)")$/\1/p' "${cmake_file}")"
if [[ -z "${cmake_name}" || -z "${cmake_sha}" ]] ||
    ! grep -q "release.name = \"${cmake_name}\";" "${module_dir}/src/model_fetch.cpp" ||
    ! grep -q "release.sha256 = \"${cmake_sha}\";" "${module_dir}/src/model_fetch.cpp"; then
  echo "defaultModelRelease() in src/model_fetch.cpp must match ${cmake_file}" >&2
  exit 1
fi

mkdir -p "${artifact_dir}"
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Werror \
  -I"${module_dir}/src" \
  "${module_dir}/tests/model_fetch_test.cpp" \
  "${module_dir}/src/model_fetch.cpp" \
  -o "${binary}"

package="${work}/pkg/fixture-v1"
mkdir -p "${package}" "${work}/run"
printf 'weights\n' > "${package}/cfsmn.bin"
printf 'beam\n' > "${package}/beam_w.bin"
printf 'keywords\n' > "${package}/keywords.txt"
printf 'notice\n' > "${package}/NOTICE"
COPYFILE_DISABLE=1 tar -czf "${work}/fixture-v1.tar.gz" -C "${work}/pkg" fixture-v1
if command -v sha256sum >/dev/null 2>&1; then
  sha="$(sha256sum "${work}/fixture-v1.tar.gz" | cut -d' ' -f1)"
else
  sha="$(shasum -a 256 "${work}/fixture-v1.tar.gz" | cut -d' ' -f1)"
fi

"${binary}" "${work}/fixture-v1.tar.gz" "${sha}" "${work}/run"
