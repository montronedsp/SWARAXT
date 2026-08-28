#!/usr/bin/env bash
set -euo pipefail

repo="montronedsp/swara-xt"
github="https://github.com/${repo}"

echo "SWARA XT Linux Installer"
echo "------------------------"
echo

require_cmd() {
    local name="$1"
    if ! command -v "${name}" >/dev/null 2>&1; then
        echo "Required command not found: ${name}" >&2
        exit 1
    fi
}

require_cmd curl
require_cmd tar
require_cmd sha256sum

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This installer supports Linux only." >&2
    exit 1
fi

arch="$(uname -m)"
case "${arch}" in
    x86_64|amd64)
        arch="x86_64"
        ;;
    *)
        echo "Unsupported architecture: ${arch} (supported: x86_64)" >&2
        exit 1
        ;;
esac

echo "Finding latest release..."
latest_url="$(curl -fsSL --proto '=https' --tlsv1.2 \
    -o /dev/null -w '%{url_effective}' "${github}/releases/latest")"
tag="${latest_url##*/}"

if [[ -z "${tag}" || "${tag}" == "latest" ]]; then
    echo "Unable to resolve the latest GitHub release tag." >&2
    exit 1
fi

if [[ ! "${tag}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Unexpected release tag: ${tag}" >&2
    exit 1
fi

version="${tag#v}"
archive="SwaraXT-Linux-${arch}-v${version}.tar.gz"
checksums="checksums-v${version}.txt"
asset_base="${github}/releases/download/${tag}"

echo "Latest version: ${version}"
echo

tmp="$(mktemp -d)"
cleanup() {
    rm -rf "${tmp}"
}
trap cleanup EXIT

echo "Downloading ${archive}..."
curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 1 \
    -o "${tmp}/${archive}" "${asset_base}/${archive}"
curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 1 \
    -o "${tmp}/${checksums}" "${asset_base}/${checksums}"

echo "Verifying SHA-256..."
expected=""
while IFS= read -r line || [[ -n "${line}" ]]; do
    line="${line%$'\r'}"
    [[ -z "${line}" ]] && continue
    [[ "${line}" == \#* ]] && continue
    hash="${line%% *}"
    rest="${line#"${hash}"}"
    rest="${rest#"${rest%%[![:space:]]*}"}"
    rest="${rest%$'\r'}"
    if [[ "${hash}" =~ ^[0-9a-fA-F]{64}$ && "${rest}" == "${archive}" ]]; then
        expected="${hash}"
        break
    fi
done < "${tmp}/${checksums}"

if [[ -z "${expected}" ]]; then
    echo "Checksum entry not found for ${archive}." >&2
    exit 1
fi

actual="$(sha256sum "${tmp}/${archive}")"
actual="${actual%% *}"

if [[ "${actual,,}" != "${expected,,}" ]]; then
    echo "SHA-256 mismatch for ${archive}." >&2
    echo "Expected: ${expected}" >&2
    echo "Actual:   ${actual}" >&2
    exit 1
fi

echo "Checksum verified."
echo

tar -xzf "${tmp}/${archive}" -C "${tmp}"

vst3_src="${tmp}/Swara XT/VST3/Swara XT.vst3"
standalone_src="${tmp}/Swara XT/Standalone/Swara XT"

if [[ ! -d "${vst3_src}" ]]; then
    echo "VST3 bundle not found in the release archive." >&2
    exit 1
fi
if [[ ! -f "${standalone_src}" ]]; then
    echo "Standalone executable not found in the release archive." >&2
    exit 1
fi

vst3_dest="${HOME}/.vst3/Swara XT.vst3"
standalone_dest="${HOME}/.local/bin/swara-xt"

echo "Installing VST3..."
mkdir -p "${HOME}/.vst3"
rm -rf "${vst3_dest}"
cp -a "${vst3_src}" "${vst3_dest}"

echo "Installing standalone..."
mkdir -p "${HOME}/.local/bin"
cp -f "${standalone_src}" "${standalone_dest}"
chmod +x "${standalone_dest}"

echo
echo "SWARA XT ${version} installed successfully."
echo
echo "VST3:"
echo "${vst3_dest}"
echo
echo "Standalone:"
echo "${standalone_dest}"
echo

local_bin="${HOME}/.local/bin"
case ":${PATH}:" in
    *:"${local_bin}":*)
        echo "Launch with:"
        echo "  swara-xt"
        ;;
    *)
        echo "Standalone installed at:"
        echo "${standalone_dest}"
        echo "Launch with:"
        echo "  ${standalone_dest}"
        echo "${local_bin} is not currently in PATH; it was not modified."
        ;;
esac

echo
echo "Rescan VST3 plug-ins in your DAW if necessary."
