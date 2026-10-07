#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused path-boundary checks for audit and SRC pivot tools."""

from __future__ import annotations

import csv
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools"))
sys.path.insert(0, str(ROOT / "Tools" / "SrcQuality"))

import audit_shruthi_factory_presets as audit  # noqa: E402
import pivot  # noqa: E402


def expect_error(label: str, fn, value) -> None:
    try:
        fn(value)
    except ValueError:
        print(f"PASS reject {label}")
        return
    raise SystemExit(f"FAIL expected rejection: {label}")


def main() -> int:
    report = audit.safe_report_path(Path("factory-audit.json"))
    expected_report = (ROOT / "artifacts" / "audits" / "factory-audit.json").resolve()
    if report != expected_report:
        raise SystemExit(f"FAIL accept factory-audit.json -> {report}")
    print("PASS accept factory-audit.json")

    nested = audit.safe_report_path(Path("preset/factory-audit.json"))
    if not nested.is_relative_to(audit.REPORT_ROOT.resolve()):
        raise SystemExit("FAIL nested report escaped")
    print("PASS accept preset/factory-audit.json")

    expect_error("../../escape.json", audit.safe_report_path, Path("../../escape.json"))
    expect_error("../escape.json", audit.safe_report_path, Path("../escape.json"))
    expect_error("absolute report", audit.safe_report_path, Path("C:/Windows/Temp/escape.json"))
    expect_error("non-json", audit.safe_report_path, Path("factory-audit.txt"))

    measurements = pivot.MEASUREMENTS_ROOT
    measurements.mkdir(parents=True, exist_ok=True)
    sample = measurements / "pure-tone.csv"
    with sample.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "candidate",
                "host_rate",
                "tone_hz",
                "passband_err_db",
                "worst_image_dbc",
                "thdn_dbc",
            ],
        )
        writer.writeheader()
        writer.writerow(
            {
                "candidate": "test",
                "host_rate": "48000",
                "tone_hz": "1000.0",
                "passband_err_db": "0.0",
                "worst_image_dbc": "-90.0",
                "thdn_dbc": "-90.0",
            }
        )

    resolved = pivot.safe_measurement_path("pure-tone.csv")
    if resolved != sample.resolve():
        raise SystemExit(f"FAIL accept pure-tone.csv -> {resolved}")
    print("PASS accept pure-tone.csv")

    prefixed = pivot.safe_measurement_path("artifacts/src-quality/measurements/pure-tone.csv")
    if prefixed != sample.resolve():
        raise SystemExit(f"FAIL prefixed measurement path -> {prefixed}")
    print("PASS accept artifacts/src-quality/measurements/pure-tone.csv")

    expect_error("../../outside.csv", pivot.safe_measurement_path, "../../outside.csv")
    expect_error("absolute csv", pivot.safe_measurement_path, str(Path("C:/Windows/Temp/outside.csv")))
    expect_error("non-csv", pivot.safe_measurement_path, "pure-tone.txt")

    outside = Path(tempfile.gettempdir()) / "swaraxt-path-boundary-escape.csv"
    outside.write_text("x\n", encoding="utf-8")
    report_root = audit.REPORT_ROOT
    report_root.mkdir(parents=True, exist_ok=True)
    report_link = report_root / "symlink-escape.json"
    measure_link = measurements / "symlink-escape.csv"
    for link in (report_link, measure_link):
        if link.exists() or link.is_symlink():
            link.unlink()
    try:
        report_link.symlink_to(outside)
        measure_link.symlink_to(outside)
    except OSError as exc:
        print(f"SKIP symlink escape ({exc})")
    else:
        expect_error("report symlink escape", audit.safe_report_path, Path("symlink-escape.json"))
        expect_error("pivot symlink escape", pivot.safe_measurement_path, "symlink-escape.csv")
    finally:
        for link in (report_link, measure_link):
            if link.exists() or link.is_symlink():
                link.unlink()
        if outside.exists():
            outside.unlink()

    print("path boundary checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
