#!/usr/bin/env python3
# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pretty-print the sweep CSVs produced by src_metrics."""

import csv
import sys
from collections import OrderedDict

TONES = ["100.0", "1000.0", "5000.0", "8000.0", "10000.0", "12000.0", "15000.0",
         "18000.0", "19000.0"]


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
    path = sys.argv[1]
    rows = list(csv.DictReader(open(path)))
    rates = sys.argv[2:] or list(OrderedDict.fromkeys(r["host_rate"] for r in rows))
    for rate in rates:
        table(rows, rate, "passband_err_db", "{:9.2f}", "PASSBAND ERROR dB")
        table(rows, rate, "worst_image_dbc", "{:9.1f}", "WORST IMAGE dBc")
        table(rows, rate, "thdn_dbc", "{:9.1f}", "THD+N dBc")


if __name__ == "__main__":
    main()
