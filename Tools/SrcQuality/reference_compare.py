#!/usr/bin/env python3
# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the in-plugin converters against external high-quality resamplers.

The external libraries are measurement instruments only: nothing here is linked
into Swara XT. Run with the local package directory on PYTHONPATH.
"""

import csv
import os
import sys

import numpy as np
import samplerate
import scipy
import scipy.signal
import soxr

NATIVE_RATE = 20000000.0 / 510.0
ROOT = os.environ.get(
    "SWARAXT_SRC_ARTIFACT_DIR",
    os.path.join(os.getcwd(), "artifacts", "src-quality"),
)
RENDERS = os.path.join(ROOT, "renders", "native")
MEASUREMENTS = os.path.join(ROOT, "measurements")

CANDIDATES = ["Hermite", "FIR32-P256-lin-A100", "FIR64-P256-lin-A100", "FIR128-P256-lin-A100"]
# Impulse and step are covered by the dedicated impulse report; a null test on
# them measures almost nothing because the aligned region is silent or DC.
SOURCES = ["noise", "sine10k", "sine15k"]
RATES = [44100, 96000]


def read_f32(path):
    return np.fromfile(path, dtype=np.float32).astype(np.float64)


def reference_soxr(native, rate):
    return soxr.resample(native, NATIVE_RATE, rate, quality="VHQ").astype(np.float64)


def reference_libsamplerate(native, rate):
    return samplerate.resample(native, rate / NATIVE_RATE, "sinc_best").astype(np.float64)


def fractional_shift(x, delta):
    """Delay x by `delta` samples (may be fractional) with an FFT phase ramp."""
    n = len(x)
    spectrum = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(n, d=1.0)
    return np.fft.irfft(spectrum * np.exp(-2j * np.pi * freqs * delta), n)


def align(reference, candidate, search=64):
    """Shift the reference onto the candidate to sub-sample accuracy.

    The external references carry their own group delay, and at 15 kHz even a
    tenth of a sample of misalignment would dominate the error, so the integer
    lag from cross-correlation is refined over a fractional grid.
    """
    n = min(len(reference), len(candidate))
    lo = 20000
    span = min(n - lo - search - 1, 262144)
    reference = reference[:n]
    candidate = candidate[:n]

    a = candidate[lo:lo + span]
    best_lag, best_score = 0, -np.inf
    for lag in range(-search, search + 1):
        b = reference[lo + lag:lo + lag + span]
        if len(b) != span:
            continue
        score = float(np.dot(a, b))
        if score > best_score:
            best_score, best_lag = score, lag

    best_frac, best_err = 0.0, np.inf
    for frac in np.arange(-1.0, 1.0, 1.0 / 128.0):
        shifted = fractional_shift(reference, -(best_lag + frac))
        err = float(np.mean((shifted[lo:lo + span] - a) ** 2))
        if err < best_err:
            best_err, best_frac = err, frac

    return fractional_shift(reference, -(best_lag + best_frac)), best_lag + best_frac


def error_metrics(reference, candidate, rate):
    reference, lag = align(reference, candidate)
    n = min(len(reference), len(candidate))
    # Skip start-up, tail and the FFT-shift wrap region at both ends.
    lo, hi = 30000, n - 30000
    if hi <= lo:
        lo, hi = 0, n
    a = reference[lo:hi]
    b = candidate[lo:hi]
    err = b - a

    rms_err = float(np.sqrt(np.mean(err ** 2)))
    peak_err = float(np.max(np.abs(err)))
    rms_ref = float(np.sqrt(np.mean(a ** 2)))
    snr = 20.0 * np.log10(rms_ref / rms_err) if rms_err > 0 and rms_ref > 0 else np.inf

    # Where the error lives, band by band.
    freqs, psd = scipy.signal.welch(err, fs=rate, nperseg=8192)
    bands = {}
    for name, (f0, f1) in {"0-5k": (0, 5000), "5-10k": (5000, 10000),
                           "10-16k": (10000, 16000), "16k-nyq": (16000, rate / 2)}.items():
        mask = (freqs >= f0) & (freqs < f1)
        bands[name] = float(np.sqrt(np.trapezoid(psd[mask], freqs[mask]))) if mask.any() else 0.0

    return {
        "lag": lag,
        "rms_error": rms_err,
        "peak_error": peak_err,
        "snr_vs_reference_db": float(snr),
        "err_0_5k": bands["0-5k"],
        "err_5_10k": bands["5-10k"],
        "err_10_16k": bands["10-16k"],
        "err_16k_nyq": bands["16k-nyq"],
    }


def main():
    out_path = os.path.join(MEASUREMENTS, "reference-comparison.csv")
    rows = []
    for source in SOURCES:
        for rate in RATES:
            native_path = os.path.join(RENDERS, f"native_{source}_{rate}.f32")
            if not os.path.exists(native_path):
                continue
            native = read_f32(native_path)

            references = {
                "soxr-VHQ": reference_soxr(native, rate),
                "libsamplerate-sinc_best": reference_libsamplerate(native, rate),
            }

            for candidate in CANDIDATES:
                host_path = os.path.join(RENDERS, f"host_{source}_{rate}_{candidate}.f32")
                if not os.path.exists(host_path):
                    continue
                host = read_f32(host_path)
                for ref_name, ref in references.items():
                    metrics = error_metrics(ref, host, rate)
                    rows.append({"source": source, "host_rate": rate, "candidate": candidate,
                                 "reference": ref_name, **metrics})
                    print(f"{source:8s} {rate:6d} {candidate:22s} vs {ref_name:24s} "
                          f"rms={metrics['rms_error']:.3e} peak={metrics['peak_error']:.3e} "
                          f"snr={metrics['snr_vs_reference_db']:7.2f} dB")

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print("wrote", out_path)
    print("versions:", "scipy", scipy.__version__, "soxr", soxr.__version__,
          "libsamplerate", samplerate.__libsamplerate_version__)


if __name__ == "__main__":
    sys.exit(main())
