#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${script_dir}/../.." && pwd)"
workspace_dir="$(cd "${source_dir}/.." && pwd)"
juce_dir="${workspace_dir}/JUCE"
build_dir="${workspace_dir}/build"
juce_sha="4f43011b96eb0636104cb3e433894cda98243626"

if [[ "$(git -C "${juce_dir}" rev-parse HEAD)" != "${juce_sha}" ]]; then
    echo "JUCE must be checked out at ${juce_sha}" >&2
    exit 1
fi

cmake -S "${source_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWARAXT_JUCE_DIR="${juce_dir}"

cmake --build "${build_dir}" --target \
    SwaraXT_VST3 SwaraXT_Standalone --parallel "$(nproc)"
