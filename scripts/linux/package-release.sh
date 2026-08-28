#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${script_dir}/../.." && pwd)"
workspace_dir="$(cd "${source_dir}/.." && pwd)"
build_dir="${workspace_dir}/build"
package_dir="${workspace_dir}/package"
artifacts_dir="${workspace_dir}/artifacts"
stage_dir="${package_dir}/Swara XT"
version="$(sed -nE 's/^project\(SwaraXT VERSION ([0-9.]+).*/\1/p' "${source_dir}/CMakeLists.txt")"
arch="$(uname -m)"
package_name="SwaraXT-Linux-${arch}-v${version}.tar.gz"
package_path="${artifacts_dir}/${package_name}"
artefacts="${build_dir}/SwaraXT_artefacts/Release"

if [[ -z "${version}" ]]; then
    echo "Unable to read project version from CMakeLists.txt" >&2
    exit 1
fi

rm -rf "${stage_dir}"
mkdir -p \
    "${stage_dir}/VST3" \
    "${stage_dir}/Standalone" \
    "${artifacts_dir}"

cp -a "${artefacts}/VST3/Swara XT.vst3" "${stage_dir}/VST3/Swara XT.vst3"
cp -a "${artefacts}/Standalone/Swara XT" "${stage_dir}/Standalone/Swara XT"
chmod +x "${stage_dir}/Standalone/Swara XT"

vst3_so="$(find "${stage_dir}/VST3/Swara XT.vst3" -type f -name 'Swara XT.so' | head -n 1)"
if [[ -z "${vst3_so}" ]]; then
    echo "VST3 ELF binary not found in staged bundle" >&2
    exit 1
fi
chmod +x "${vst3_so}"

cp "${source_dir}/packaging/linux/README.txt" "${stage_dir}/README.txt"
cp "${source_dir}/LICENSE" "${stage_dir}/LICENSE.txt"
cp "${source_dir}/THIRD_PARTY_NOTICES.md" "${stage_dir}/THIRD_PARTY_NOTICES.txt"
cp "${source_dir}/resources/Skin/ATTRIBUTION.md" "${stage_dir}/PANEL_ARTWORK_ATTRIBUTION.txt"
cp "${source_dir}/resources/Skin/CC-BY-SA-3.0.txt" "${stage_dir}/PANEL_ARTWORK_LICENSE.txt"
cp "${source_dir}/resources/Fonts/OFL-1.1.txt" "${stage_dir}/FONT_OFL-1.1.txt"

tar -C "${package_dir}" --owner=0 --group=0 --numeric-owner -czf "${package_path}" "Swara XT"

(
    cd "${artifacts_dir}"
    sha256sum "${package_name}" > "checksums-v${version}.txt"
)

echo "Wrote ${package_path}"
echo "Wrote ${artifacts_dir}/checksums-v${version}.txt"
