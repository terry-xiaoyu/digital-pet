#!/usr/bin/env bash
set -euo pipefail

module_dir="components/multimedia/audio"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/audio-pr-test.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror \
  "${module_dir}/tests/audio_pr_contract_test.cpp" \
  "${module_dir}/src/resampler.cpp" \
  -I"${module_dir}/include" \
  -o "${build_dir}/audio_pr_contract_test"

"${build_dir}/audio_pr_contract_test" --resampler-contract
