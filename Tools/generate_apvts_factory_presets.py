#!/usr/bin/env python3
"""Generate APVTS factory overrides and user-designed factory presets.

Rules:
  * ends with FINALFIX  -> Mutable factory correction (wins over FIXED)
  * ends with FIXED     -> Mutable factory correction
  * everything else     -> new factory preset
  * exclusions: LFO MOD, duo pong, voweano (case-insensitive; optional CA prefix)

Pass the read-only preset directory via --preset-dir. Do not commit raw presets.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FACTORY_HEADER = REPO / "Source/Plugin/ShruthiFactoryPresetData.h"
OUT = REPO / "Source/Plugin/ApvtsFactoryPresetData.h"

EXCLUSIONS = {"lfo mod", "duo pong", "voweano"}


def decode_preset(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] != b"VC2!":
        raise ValueError(f"Not a JUCE binary XML preset: {path}")
    size = struct.unpack_from("<I", data, 4)[0]
    xml = data[8 : 8 + size].decode("utf-8")
    name_m = re.search(r'presetName="([^"]*)"', xml)
    ver_m = re.search(r'stateVersion="(\d+)"', xml)
    params = [(i, float(v)) for i, v in re.findall(r'<PARAM id="([^"]+)" value="([^"]+)"/>', xml)]
    return {
        "file": path.name,
        "name": name_m.group(1) if name_m else path.stem,
        "version": int(ver_m.group(1)) if ver_m else 0,
        "params": params,
    }


def load_factory_names() -> list[str]:
    text = FACTORY_HEADER.read_text(encoding="utf-8")
    return re.findall(r'\{\s*"([^"]+)",\s*"[^"]+",\s*"[^"]+"', text)


def normalize(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", s.lower())


def strip_correction_suffix(name: str) -> tuple[str | None, str | None]:
    """Return (base_name, kind) where kind is FINALFIX|FIXED|None."""
    m = re.search(r"(?i)[\s_]*finalfix$", name)
    if m:
        return name[: m.start()].rstrip(), "FINALFIX"
    m = re.search(r"(?i)[\s_]*fixed$", name)
    if m:
        return name[: m.start()].rstrip(), "FIXED"
    return None, None


def is_excluded(name: str) -> bool:
    lower = name.strip().lower()
    bare = re.sub(r"(?i)^ca\s+", "", lower).strip()
    return bare in EXCLUSIONS or lower in EXCLUSIONS


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit_params(params: list[tuple[str, float]]) -> str:
    return "\n".join(f'        {{ "{c_escape(pid)}", {val!r} }},' for pid, val in params)


def match_factory(base: str, factory_names: list[str]) -> str | None:
    exact = [f for f in factory_names if f == base]
    if len(exact) == 1:
        return exact[0]
    ci = [f for f in factory_names if f.lower() == base.lower()]
    if len(ci) == 1:
        return ci[0]
    norm = normalize(base)
    soft = [f for f in factory_names if normalize(f) == norm]
    if len(soft) == 1:
        return soft[0]
    return None


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate APVTS factory preset data from saved .swaraxtpreset files."
    )
    parser.add_argument(
        "--preset-dir",
        type=Path,
        required=True,
        help="Read-only directory of saved .swaraxtpreset files (not committed).",
    )
    args = parser.parse_args()
    preset_dir: Path = args.preset_dir
    if not preset_dir.is_dir():
        raise SystemExit(f"Preset directory not found: {preset_dir}")

    factory_names = load_factory_names()
    if len(factory_names) != 40:
        raise SystemExit(f"Expected 40 Mutable factory names, got {len(factory_names)}")

    presets = [decode_preset(p) for p in sorted(preset_dir.glob("*.swaraxtpreset"))]

    # Mutable corrections: FINALFIX wins over FIXED for same target.
    corrections: dict[str, dict] = {}
    unmatched = []
    for p in presets:
        base, kind = strip_correction_suffix(p["name"])
        if kind is None:
            continue
        target = match_factory(base, factory_names)
        if target is None:
            unmatched.append((p["name"], base, kind))
            continue
        existing = corrections.get(target)
        if existing is None or (kind == "FINALFIX" and existing["kind"] != "FINALFIX"):
            corrections[target] = {**p, "kind": kind, "target": target, "base": base}
        elif kind == "FIXED" and existing["kind"] == "FINALFIX":
            pass  # keep FINALFIX
        elif kind == existing["kind"]:
            unmatched.append((p["name"], base, f"duplicate {kind}"))

    if unmatched:
        raise SystemExit(f"Unmatched/ambiguous corrections: {unmatched}")

    # New user-designed presets: everything else except exclusions and corrections.
    user_presets = []
    excluded = []
    for p in presets:
        base, kind = strip_correction_suffix(p["name"])
        if kind is not None:
            continue
        if is_excluded(p["name"]):
            excluded.append(p["name"])
            continue
        user_presets.append(p)

    # Stable order: alphabetical by shipping name for reproducibility of new bank,
    # but user asked NOT to alphabetically reshuffle - "Do not alphabetically reshuffle
    # older factory programs". For NEW presets, filesystem/name order from inventory
    # is fine. Use the order they appeared in sorted(filename) which is what we have.
    # Keep sorted-by-filename order (already sorted).

    # Deduplicate shipping names.
    seen = set()
    for p in user_presets:
        if p["name"] in seen:
            raise SystemExit(f"Duplicate user factory name: {p['name']}")
        seen.add(p["name"])

    lines = [
        "// Copyright 2026 MontroneDSP.",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by Tools/generate_apvts_factory_presets.py - do not edit by hand.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace swaraxt {",
        "",
        "struct ApvtsFactoryParam {",
        "    const char* id;",
        "    float value;",
        "};",
        "",
        "struct ApvtsFactoryPreset {",
        "    const char* displayName;",
        "    const char* author;",
        "    const ApvtsFactoryParam* params;",
        "    std::size_t paramCount;",
        "};",
        "",
        f"inline constexpr std::size_t kMutableFactoryOverrideCount = {len(corrections)};",
        f"inline constexpr std::size_t kUserFactoryPresetCount = {len(user_presets)};",
        # Keep old alias so transitional includes compile if needed.
        "inline constexpr std::size_t kCaFactoryPresetCount = kUserFactoryPresetCount;",
        "",
    ]

    # Emit Mutable overrides in Mutable bank order for stable indexing.
    ordered_targets = [f for f in factory_names if f in corrections]
    for idx, target in enumerate(ordered_targets):
        p = corrections[target]
        lines.append(f"inline constexpr ApvtsFactoryParam kMutableOverrideParams{idx}[] = {{")
        lines.append(emit_params(p["params"]))
        lines.append("};")
        lines.append("")

    for idx, p in enumerate(user_presets):
        lines.append(f"inline constexpr ApvtsFactoryParam kUserFactoryParams{idx}[] = {{")
        lines.append(emit_params(p["params"]))
        lines.append("};")
        lines.append("")

    lines.append("inline constexpr ApvtsFactoryPreset kMutableFactoryOverrides[kMutableFactoryOverrideCount] = {")
    for idx, target in enumerate(ordered_targets):
        lines.append("    {")
        lines.append(f'        "{c_escape(target)}",')
        lines.append('        "",')
        lines.append(f"        kMutableOverrideParams{idx},")
        lines.append(f"        sizeof(kMutableOverrideParams{idx}) / sizeof(kMutableOverrideParams{idx}[0]),")
        lines.append("    },")
    lines.append("};")
    lines.append("")

    if user_presets:
        lines.append(
            "inline constexpr ApvtsFactoryPreset kUserFactoryPresets[kUserFactoryPresetCount] = {"
        )
        for idx, p in enumerate(user_presets):
            lines.append("    {")
            lines.append(f'        "{c_escape(p["name"])}",')
            lines.append('        "",')
            lines.append(f"        kUserFactoryParams{idx},")
            lines.append(f"        sizeof(kUserFactoryParams{idx}) / sizeof(kUserFactoryParams{idx}[0]),")
            lines.append("    },")
        lines.append("};")
    else:
        lines.append(
            "inline constexpr ApvtsFactoryPreset kUserFactoryPresets[1] = {\n"
            '    { "", "", nullptr, 0 },\n'
            "};"
        )
    lines.append("")
    # Alias for previous naming used by PluginProcessor.
    lines.append("inline constexpr ApvtsFactoryPreset* kCaFactoryPresets =")
    lines.append("    const_cast<ApvtsFactoryPreset*>(kUserFactoryPresets);")
    lines.append("")
    # Actually const_cast in constexpr header is bad. Use reference alias instead.
    # Better: just #define style - use kUserFactoryPresets everywhere in C++.

    lines.append("}  // namespace swaraxt")
    lines.append("")

    # Remove the bad alias lines and write cleanly
    clean = "\n".join(lines)
    clean = clean.replace(
        "inline constexpr ApvtsFactoryPreset* kCaFactoryPresets =\n"
        "    const_cast<ApvtsFactoryPreset*>(kUserFactoryPresets);\n\n",
        "",
    )
    # Add proper alias as array reference via macro-like using:
    clean = clean.replace(
        "inline constexpr std::size_t kCaFactoryPresetCount = kUserFactoryPresetCount;\n",
        "inline constexpr std::size_t kCaFactoryPresetCount = kUserFactoryPresetCount;\n",
    )
    # Append after kUserFactoryPresets definition:
    if "kUserFactoryPresets[" in clean:
        clean = clean.replace(
            "}  // namespace swaraxt",
            "// Compatibility alias used by existing program-index helpers.\n"
            "#define kCaFactoryPresets kUserFactoryPresets\n"
            "\n"
            "}  // namespace swaraxt",
        )

    OUT.write_text(clean, encoding="utf-8", newline="\n")

    print("=== MUTABLE CORRECTIONS ===")
    for target in ordered_targets:
        p = corrections[target]
        print(f"{p['file']} -> {target} [{p['kind']}] params={len(p['params'])}")
    print(f"count={len(ordered_targets)}")
    print("=== NEW USER FACTORY ===")
    for p in user_presets:
        print(f"{p['file']} -> {p['name']} params={len(p['params'])}")
    print(f"count={len(user_presets)}")
    print("=== EXCLUDED ===")
    for n in excluded:
        print(n)
    print(f"Wrote {OUT}")


if __name__ == "__main__":
    main()
