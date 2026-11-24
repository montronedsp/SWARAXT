#!/usr/bin/env bash
set -euo pipefail

repo="montronedsp/SWARAXT"
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
asset_base="${github}/releases/download/${tag}"

echo "Latest version: ${version}"
echo

tmp="$(mktemp -d)"
cleanup() {
    rm -rf "${tmp}"
    if [[ "${own_stages:-false}" == true ]]; then
        rm -rf "${vst3_stage}" "${standalone_stage}"
    fi
    if [[ -n "${install_lock:-}" ]]; then
        rmdir "${install_lock}" || true
    fi
}
trap cleanup EXIT

parse_checksum_manifest() {
    local file="$1"
    local line hash rest
    while IFS= read -r line || [[ -n "${line}" ]]; do
        line="${line%$'\r'}"
        [[ -z "${line}" ]] && continue
        [[ "${line}" == \#* ]] && continue
        hash="${line%% *}"
        rest="${line#"${hash}"}"
        rest="${rest#"${rest%%[![:space:]]*}"}"
        rest="${rest#$'\r'}"
        # Accept both "HASH  name" and "HASH *name" (binary mode) forms.
        rest="${rest#\*}"
        if [[ "${hash}" =~ ^[0-9a-fA-F]{64}$ && "${rest}" == "${archive}" ]]; then
            printf '%s\n' "${hash}"
            return 0
        fi
    done < "${file}"
    return 1
}

resolve_expected_checksum() {
    local name expected_local=""
    # Official published manifests use SHA256SUMS-v*.txt; older releases used checksums-v*.txt.
    for name in "SHA256SUMS-v${version}.txt" "checksums-v${version}.txt"; do
        if curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 1 \
            -o "${tmp}/${name}" "${asset_base}/${name}" 2>/dev/null; then
            if expected_local="$(parse_checksum_manifest "${tmp}/${name}")"; then
                printf '%s\n' "${expected_local}"
                return 0
            fi
            echo "Checksum manifest ${name} did not list ${archive}." >&2
        fi
    done

    echo "Checksum file unavailable; verifying via GitHub release metadata..." >&2
    local release_json
    release_json="$(curl -fsSL --proto '=https' --tlsv1.2 \
        "https://api.github.com/repos/${repo}/releases/tags/${tag}")" || return 1

    if command -v python3 >/dev/null 2>&1; then
        expected_local="$(printf '%s' "${release_json}" | python3 -c '
import json, re, sys
name = sys.argv[1]
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for asset in data.get("assets") or []:
    if asset.get("name") != name:
        continue
    digest = asset.get("digest") or ""
    match = re.fullmatch(r"sha256:([0-9a-fA-F]{64})", digest)
    if match:
        print(match.group(1))
        sys.exit(0)
sys.exit(1)
' "${archive}")" && {
            printf '%s\n' "${expected_local}"
            return 0
        }
    else
        # Dependency-free fallback: scan name/digest pairs from the assets array only.
        local assets_json asset_name
        assets_json="$(printf '%s\n' "${release_json}" | sed -n '/"assets": \[/,/\]/p')"
        asset_name=""
        while IFS= read -r line; do
            if [[ "${line}" =~ \"name\":[[:space:]]*\"([^\"]+)\" ]]; then
                asset_name="${BASH_REMATCH[1]}"
            elif [[ -n "${asset_name}" && "${line}" =~ \"digest\":[[:space:]]*\"sha256:([0-9a-fA-F]{64})\" ]]; then
                if [[ "${asset_name}" == "${archive}" ]]; then
                    printf '%s\n' "${BASH_REMATCH[1]}"
                    return 0
                fi
                asset_name=""
            fi
        done <<< "${assets_json}"
    fi
    return 1
}

echo "Downloading ${archive}..."
curl -fsSL --proto '=https' --tlsv1.2 --retry 3 --retry-delay 1 \
    -o "${tmp}/${archive}" "${asset_base}/${archive}"

expected=""
if ! expected="$(resolve_expected_checksum)"; then
    echo "Unable to resolve SHA-256 checksum for ${archive}." >&2
    exit 1
fi
if [[ -z "${expected}" || ! "${expected}" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "Unable to resolve SHA-256 checksum for ${archive}." >&2
    exit 1
fi

echo "Verifying SHA-256..."

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

# Reject path traversal / absolute members before extraction.
while IFS= read -r member; do
    case "${member}" in
        /*|*../*|*/..|*/../*)
            echo "Archive member escapes staging directory: ${member}" >&2
            exit 1
            ;;
    esac
done < <(tar -tzf "${tmp}/${archive}")

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

vst3_stage="${vst3_dest}.stage"
vst3_backup="${vst3_dest}.backup"
standalone_stage="${standalone_dest}.stage"
standalone_backup="${standalone_dest}.backup"

# Refuse stale recovery data rather than deleting a previous valid install.
if [[ -z "${HOME:-}" || "${HOME}" != /* || "${HOME}" == / ]]; then
    echo "HOME must name an absolute user directory." >&2
    exit 1
fi
lock_path="${HOME}/.swara-xt-install.lock"
if ! mkdir "${lock_path}"; then
    echo "An installation is active or interrupted. Inspect ${lock_path} before retrying." >&2
    exit 1
fi
install_lock="${lock_path}"
for recovery_path in "${vst3_stage}" "${vst3_backup}" "${standalone_stage}" "${standalone_backup}"; do
    if [[ -e "${recovery_path}" || -L "${recovery_path}" ]]; then
        echo "Interrupted installation data retained at ${recovery_path}; recover it before retrying." >&2
        exit 1
    fi
done
own_stages=true
mkdir -p "${HOME}/.vst3" "${HOME}/.local/bin"
if ! cp -a "${vst3_src}" "${vst3_stage}" \
    || ! cp -f "${standalone_src}" "${standalone_stage}" \
    || ! chmod +x "${standalone_stage}"; then
    echo "Could not stage the installation; existing files are unchanged." >&2
    exit 1
fi

vst3_backed=false
standalone_backed=false
vst3_published=false
standalone_published=false
rollback() {
    # Renames are per destination, not a globally atomic two-file transaction.
    if [[ "${standalone_published}" == true ]]; then rm -f "${standalone_dest}"; fi
    if [[ "${vst3_published}" == true ]]; then rm -rf "${vst3_dest}"; fi
    if [[ "${standalone_backed}" == true ]]; then
        mv "${standalone_backup}" "${standalone_dest}" \
            || echo "Restore failed; previous standalone retained at ${standalone_backup}." >&2
    fi
    if [[ "${vst3_backed}" == true ]]; then
        mv "${vst3_backup}" "${vst3_dest}" \
            || echo "Restore failed; previous VST3 retained at ${vst3_backup}." >&2
    fi
}
if [[ -e "${vst3_dest}" || -L "${vst3_dest}" ]]; then
    if ! mv "${vst3_dest}" "${vst3_backup}"; then
        echo "Could not back up VST3; publication cancelled." >&2
        exit 1
    fi
    vst3_backed=true
fi
if [[ -e "${standalone_dest}" || -L "${standalone_dest}" ]]; then
    if ! mv "${standalone_dest}" "${standalone_backup}"; then
        rollback
        exit 1
    fi
    standalone_backed=true
fi
if ! mv "${vst3_stage}" "${vst3_dest}"; then
    rollback
    echo "VST3 publication failed; inspect any retained backup if recovery failed." >&2
    exit 1
fi
vst3_published=true
if ! mv "${standalone_stage}" "${standalone_dest}"; then
    rollback
    echo "Standalone publication failed; inspect any retained backup if recovery failed." >&2
    exit 1
fi
standalone_published=true
rm -rf "${vst3_backup}" "${standalone_backup}"

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
