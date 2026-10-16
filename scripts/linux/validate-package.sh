#!/bin/bash
# Shared shipping identity checks; source before staging and call after copying.
validate_package_identity() {
    local version="$1" stage="$2" filename="$3"
    [[ "$version" == "1.2.3" ]] || { echo "This release requires PROJECT_VERSION 1.2.3" >&2; return 1; }
    [[ "$filename" == "SwaraXT-Linux-"*"-v${version}.tar.gz" ]] || return 1
    [[ "$(head -n 1 "$stage/README.txt" | tr -d '\r')" == "Swara XT $version" ]] || return 1
    grep -Fxq 'https://github.com/montronedsp/SWARAXT' <(tr -d '\r' < "$stage/README.txt") || return 1
    [[ -f "${stage}/Skin Editor/SWARA XT Skin Editor" ]] || {
        echo "Staged Skin Editor binary missing" >&2
        return 1
    }
    for required in \
        manufacturer_mark_example.svg \
        decoration_example.svg \
        background_example.svg \
        background_example.png \
        colours_example.txt \
        README.md
    do
        [[ -f "${stage}/Skin Examples/${required}" ]] || {
            echo "Staged Skin Example missing: ${required}" >&2
            return 1
        }
    done
    if grep -rE --include='*.txt' --include='*.md' 'github\.com/montronedsp/swara-xt' "$stage"; then
        echo "Legacy repository URL in staged documentation" >&2
        return 1
    fi
}
