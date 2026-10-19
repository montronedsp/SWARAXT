#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${script_dir}/../.." && pwd)"
workspace_dir="$(cd "${source_dir}/.." && pwd)"
juce_dir="${SWARAXT_JUCE_DIR:-${source_dir}/.cache/JUCE}"
build_dir="${SWARAXT_BUILD_DIR:-${workspace_dir}/build}"
jobs="${SWARAXT_BUILD_JOBS:-1}"
if [[ ! "${jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "SWARAXT_BUILD_JOBS must be a positive integer" >&2
    exit 2
fi
# Dependencies.cmake validates the supported JUCE version at this path.

cmake -S "${source_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWARAXT_JUCE_DIR="${juce_dir}"

cmake --build "${build_dir}" --target \
    SwaraXT_VST3 SwaraXT_Standalone SwaraXtSkinEditor --parallel "${jobs}"
