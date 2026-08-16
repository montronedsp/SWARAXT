#!/usr/bin/env python3
# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Analyse the deterministic production renders.

Two independent questions are answered per case:

  1. How much energy sits between native Nyquist and host Nyquist? The native
     39.2 kHz stream cannot represent anything up there, so every dB found in
     that band was invented by the sample-rate converter. This separates SRC
     images from the Shruthi's own aliasing without needing a reference.

  2. How far is each converter from an offline very-high-quality resampling of
     the same native stream? That catches the images which fold back down into
     the audible band and would otherwise be invisible to test 1.
"""

import csv
import os
import sys

import numpy as np
import soxr
from scipy.io import wavfile

ROOT = os.environ.get(
    "SWARAXT_SRC_ARTIFACT_DIR",
    os.path.join(os.getcwd(), "artifacts", "src-quality"),
)
RENDERS = os.path.join(ROOT, "renders")
NATIVE_RATE = 20000000.0 / 510.0
NATIVE_NYQUIST = NATIVE_RATE / 2.0


def load(path):
    rate, audio = wavfile.read(path)
    audio = np.asarray(audio, dtype=np.float64)
    if audio.ndim > 1:
        audio = audio[:, 0]
    return audio, float(rate)


def image_band_dbc(x, rate):
    """Energy between native Nyquist and host Nyquist, relative to total."""
    if rate / 2.0 <= NATIVE_NYQUIST + 100.0:
        return None
    seg = x[len(x) // 4:len(x) // 4 + 131072]
    if len(seg) < 8192:
        return None
    spectrum = np.abs(np.fft.rfft(seg * np.kaiser(len(seg), 24.0))) ** 2
    freqs = np.fft.rfftfreq(len(seg), 1.0 / rate)
    band = (freqs > NATIVE_NYQUIST) & (freqs < rate / 2.0 * 0.995)
    total = spectrum.sum()
    if total <= 0.0:
        return None
    return 10.0 * np.log10(max(spectrum[band].sum() / total, 1e-30))


def fractional_shift(x, delta):
    spectrum = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(len(x), d=1.0)
    return np.fft.irfft(spectrum * np.exp(-2j * np.pi * freqs * delta), len(x))


def snr_vs_reference(candidate, reference):
    """Best-case SNR after sub-sample alignment, over a steady middle window.

    The integer lag comes from a full cross-correlation rather than a small
    local search: these are periodic waveforms, so a narrow search can lock
    onto the wrong period and report a misalignment figure instead of the
    converter's error.
    """
    n = min(len(candidate), len(reference))
    start, length = n // 4, min(131072, n // 2)
    cand = candidate[start:start + length]

    size = 1 << int(np.ceil(np.log2(2 * length)))
    correlation = np.fft.irfft(np.fft.rfft(cand, size) *
                               np.conj(np.fft.rfft(reference[start:start + length], size)), size)
    lag = int(np.argmax(correlation))
    if lag > size // 2:
        lag -= size

    lo = start + lag
    if lo < 0 or lo + length > len(reference):
        return None
    seg = reference[lo:lo + length]

    def score(frac):
        shifted = fractional_shift(seg, frac) if frac else seg
        err = cand - shifted
        return (10.0 * np.log10(np.sum(shifted ** 2) / max(np.sum(err ** 2), 1e-30)),
                float(np.sqrt(np.mean(err ** 2))), float(np.max(np.abs(err))))

    best, centre = None, 0.0
    for step in (0.1, 0.01):
        candidates = np.arange(centre - 10 * step, centre + 10.5 * step, step)
        for frac in candidates:
            result = score(frac)
            if best is None or result[0] > best[0]:
                best, centre = result, frac
    return best


def fundamental_hz(x, rate):
    seg = x[len(x) // 4:len(x) // 4 + 262144]
    if len(seg) < 8192:
        return float("nan")
    spectrum = np.abs(np.fft.rfft(seg * np.kaiser(len(seg), 12.0)))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / rate)
    lo = np.searchsorted(freqs, 30.0)
    peak = lo + int(np.argmax(spectrum[lo:np.searchsorted(freqs, 5000.0)]))
    # Parabolic interpolation around the bin for sub-bin accuracy.
    a, b, c = (np.log(max(spectrum[peak + d], 1e-30)) for d in (-1, 0, 1))
    offset = 0.5 * (a - c) / (a - 2 * b + c)
    return float((peak + offset) * rate / len(seg))


def main():
    converters = ["hermite", "fir64", "fir96"]
    natives = sorted(f for f in os.listdir(os.path.join(RENDERS, "native"))
                     if f.endswith("_native.wav"))
    out = open(os.path.join(ROOT, "measurements", "render-analysis.csv"), "w", newline="")
    writer = csv.writer(out)
    writer.writerow(["case", "host_rate", "converter", "image_band_dbc", "snr_vs_ref_db",
                     "rms_err", "peak_err", "dc_mean", "rms", "peak", "fundamental_hz"])

    for native_file in natives:
        tag = native_file[:-len("_native.wav")]
        native, _ = load(os.path.join(RENDERS, "native", native_file))
        for rate in (44100, 96000):
            reference = soxr.resample(native, NATIVE_RATE, rate, quality="VHQ")
            for conv in converters:
                path = os.path.join(RENDERS, conv, f"{tag}_{rate}_{conv}.wav")
                if not os.path.exists(path):
                    continue
                y, sr = load(path)
                snr = snr_vs_reference(y, reference)
                band = image_band_dbc(y, sr)
                writer.writerow([
                    tag, rate, conv,
                    f"{band:.2f}" if band is not None else "",
                    f"{snr[0]:.2f}" if snr else "",
                    f"{snr[1]:.3e}" if snr else "",
                    f"{snr[2]:.3e}" if snr else "",
                    f"{np.mean(y):.3e}", f"{np.sqrt(np.mean(y ** 2)):.6f}",
                    f"{np.max(np.abs(y)):.6f}", f"{fundamental_hz(y, sr):.4f}"])
        print("analysed", tag, flush=True)
    out.close()


if __name__ == "__main__":
    sys.exit(main())
