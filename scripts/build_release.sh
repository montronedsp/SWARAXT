#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${repo_root}"

host="$(uname -s)"
case "${host}" in
    Linux*)  configure_preset="${1:-linux-release}"; build_preset="${2:-linux-release}" ;;
    Darwin*) configure_preset="${1:-mac-release}"; build_preset="${2:-mac-release}" ;;
    *)
        echo "Unsupported host '${host}'. Use CMake presets directly." >&2
        exit 1
        ;;
esac

echo "Configuring preset: ${configure_preset}"
cmake --preset "${configure_preset}"

echo "Building preset: ${build_preset}"
cmake --build --preset "${build_preset}"

echo "Build complete. Artifacts under: ${repo_root}/build/${configure_preset}/SwaraXT_artefacts"
