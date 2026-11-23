#!/usr/bin/env python3
# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pretty-print the sweep CSVs produced by src_metrics."""

import csv
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEASUREMENTS_ROOT = ROOT / "artifacts" / "src-quality" / "measurements"

TONES = ["100.0", "1000.0", "5000.0", "8000.0", "10000.0", "12000.0", "15000.0",
         "18000.0", "19000.0"]


def safe_measurement_path(value: str) -> Path:
    """Resolve a measurement CSV under artifacts/src-quality/measurements."""
    if not value or not str(value).strip():
        raise ValueError("measurement path is empty")
    raw = Path(value)
    if raw.is_absolute() or raw.anchor:
        raise ValueError("measurement path must be relative")
    if any(part == ".." for part in raw.parts):
        raise ValueError("measurement path must not contain '..'")
    prefix = Path("artifacts") / "src-quality" / "measurements"
    try:
        raw = raw.relative_to(prefix)
    except ValueError:
        pass
    if raw.suffix.lower() != ".csv" or not raw.name or raw.name in {".", ".."}:
        raise ValueError("measurement must be a .csv file")
    root = MEASUREMENTS_ROOT.resolve()
    candidate = (root / raw).resolve()
    if not candidate.is_relative_to(root):
        raise ValueError("measurement path escapes artifacts/src-quality/measurements")
    if not candidate.is_file() or candidate.is_dir():
        raise ValueError("measurement must be a regular .csv file under the measurements root")
    return candidate


def table(rows, rate, column, fmt, title):
    cands = list(OrderedDict.fromkeys(r["candidate"] for r in rows))
    index = {(r["candidate"], r["host_rate"], r["tone_hz"]): r for r in rows}
    print(f"\n{title}  [host {rate} Hz]")
    header = "".join(f"{t.split('.')[0]:>9s}" for t in TONES)
    print(f"{'candidate':24s}{header}")
    for c in cands:
        line = f"{c:24s}"
        for t in TONES:
            r = index.get((c, rate, t))
            line += fmt.format(float(r[column])) if r else "        -"
        print(line)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: pivot.py <measurement.csv> [host_rate ...]")
    try:
        path = safe_measurement_path(sys.argv[1])
    except ValueError as exc:
        raise SystemExit(f"invalid measurement path: {exc}") from exc
    with path.open(encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))
    rates = sys.argv[2:] or list(OrderedDict.fromkeys(r["host_rate"] for r in rows))
    for rate in rates:
        table(rows, rate, "passband_err_db", "{:9.2f}", "PASSBAND ERROR dB")
        table(rows, rate, "worst_image_dbc", "{:9.1f}", "WORST IMAGE dBc")
        table(rows, rate, "thdn_dbc", "{:9.1f}", "THD+N dBc")


if __name__ == "__main__":
    main()
