#!/bin/bash
# Shared shipping identity checks; source before staging and call after copying.
validate_package_identity() {
    local version="$1" stage="$2" filename="$3"
    [[ "$version" == "1.3.1" ]] || { echo "This release requires PROJECT_VERSION 1.3.1" >&2; return 1; }
    [[ "$filename" == "SwaraXT-Linux-"*"-v${version}.tar.gz" ]] || return 1
    [[ "$(head -n 1 "$stage/README.txt" | tr -d '\r')" == "Swara XT $version" ]] || return 1
    grep -Fxq 'https://github.com/montronedsp/SWARAXT' <(tr -d '\r' < "$stage/README.txt") || return 1
    if find "$stage" -iname '*skin*editor*' -print -quit | grep -q .; then
        echo "Separate Skin Editor application is not part of the release package" >&2
        return 1
    fi
    [[ -d "$stage/VST3/Swara XT.vst3" && -f "$stage/Standalone/Swara XT" ]] || return 1
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
    for decoration in pcb_trace_separator greek_key_1turn greek_key_4turns roman_plait roman_intertwined roman_guilloche roman_interlace roman_wicker roman_meander roman_key_border; do
        [[ -f "$stage/Skin Examples/Decorations/$decoration.svg" ]] || return 1
    done
    for notice in SOURCE.txt LICENSE.txt COPYRIGHT.txt THIRD_PARTY_NOTICES.txt PANEL_ARTWORK_ATTRIBUTION.txt PANEL_ARTWORK_LICENSE.txt FONT_OFL-1.1.txt; do
        [[ -f "$stage/$notice" ]] || return 1
    done
    [[ -f "$stage/Skin Examples/ATTRIBUTION.md" && -f "$stage/Skin Examples/CC-BY-SA-3.0.txt" ]] || return 1
    if grep -rE --include='*.txt' --include='*.md' 'github\.com/montronedsp/swara-xt' "$stage"; then
        echo "Legacy repository URL in staged documentation" >&2
        return 1
    fi
}
