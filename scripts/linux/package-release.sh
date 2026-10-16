#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "${script_dir}/../.." && pwd)"
workspace_dir="$(cd "${source_dir}/.." && pwd)"
build_dir="${SWARAXT_BUILD_DIR:-${workspace_dir}/build}"
artifacts_dir="${SWARAXT_OUTPUT_DIR:-${workspace_dir}/artifacts}"
package_dir="${artifacts_dir}/linux-package"
stage_dir="${package_dir}/Swara XT"
version="$(sed -nE 's/^project\(SwaraXT VERSION ([0-9.]+).*/\1/p' "${source_dir}/CMakeLists.txt")"
arch="$(uname -m)"
package_name="SwaraXT-Linux-${arch}-v${version}.tar.gz"
package_path="${artifacts_dir}/${package_name}"
artefacts="${build_dir}/SwaraXT_artefacts/Release"
skin_editor="${build_dir}/SwaraXtSkinEditor_artefacts/Release/SWARA XT Skin Editor"
skin_examples="${source_dir}/resources/Skin/Examples"

if [[ -z "${version}" ]]; then
    echo "Unable to read project version from CMakeLists.txt" >&2
    exit 1
fi
source "${script_dir}/validate-package.sh"
[[ "$version" == "1.2.3" ]] || { echo "This release requires PROJECT_VERSION 1.2.3" >&2; exit 1; }
[[ -x "${skin_editor}" || -f "${skin_editor}" ]] || {
    echo "Release Skin Editor not found at ${skin_editor} (build SwaraXtSkinEditor)" >&2
    exit 1
}
[[ -d "${skin_examples}" ]] || {
    echo "Skin Examples folder missing at ${skin_examples}" >&2
    exit 1
}

rm -rf "${stage_dir}"
mkdir -p \
    "${stage_dir}/VST3" \
    "${stage_dir}/Standalone" \
    "${stage_dir}/Skin Editor" \
    "${artifacts_dir}"

cp -a "${artefacts}/VST3/Swara XT.vst3" "${stage_dir}/VST3/Swara XT.vst3"
cp -a "${artefacts}/Standalone/Swara XT" "${stage_dir}/Standalone/Swara XT"
chmod +x "${stage_dir}/Standalone/Swara XT"
cp -a "${skin_editor}" "${stage_dir}/Skin Editor/SWARA XT Skin Editor"
chmod +x "${stage_dir}/Skin Editor/SWARA XT Skin Editor"
cp -a "${skin_examples}" "${stage_dir}/Skin Examples"

vst3_so="$(find "${stage_dir}/VST3/Swara XT.vst3" -type f -name 'Swara XT.so' | head -n 1)"
if [[ -z "${vst3_so}" ]]; then
    echo "VST3 ELF binary not found in staged bundle" >&2
    exit 1
fi
chmod +x "${vst3_so}"

cp "${source_dir}/packaging/linux/README.txt" "${stage_dir}/README.txt"
cp "${source_dir}/LICENSE" "${stage_dir}/LICENSE.txt"
cp "${source_dir}/COPYRIGHT" "${stage_dir}/COPYRIGHT.txt"
cp "${source_dir}/THIRD_PARTY_NOTICES.md" "${stage_dir}/THIRD_PARTY_NOTICES.txt"
cp "${source_dir}/resources/Skin/ATTRIBUTION.md" "${stage_dir}/PANEL_ARTWORK_ATTRIBUTION.txt"
cp "${source_dir}/resources/Skin/CC-BY-SA-3.0.txt" "${stage_dir}/PANEL_ARTWORK_LICENSE.txt"
cp "${source_dir}/resources/Fonts/OFL-1.1.txt" "${stage_dir}/FONT_OFL-1.1.txt"
validate_package_identity "$version" "$stage_dir" "$package_name"

tar -C "${package_dir}" --owner=0 --group=0 --numeric-owner -czf "${package_path}" "Swara XT"

(
    cd "${artifacts_dir}"
    sha256sum "${package_name}" > "checksums-v${version}.txt"
)

echo "Wrote ${package_path}"
echo "Wrote ${artifacts_dir}/checksums-v${version}.txt"
