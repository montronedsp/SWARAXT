#!/usr/bin/env python3
# Copyright 2026 MontroneDSP.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Engineering plots for the SRC quality study."""

import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.environ.get(
    "SWARAXT_SRC_ARTIFACT_DIR",
    os.path.join(os.getcwd(), "artifacts", "src-quality"),
)
MEAS = os.path.join(ROOT, "measurements")
PLOTS = os.path.join(ROOT, "plots")
NATIVE_RATE = 20000000.0 / 510.0

SHOW = [("Hermite", "Hermite (fallback)", "tab:red"),
        ("FIR32-P256-lin-A100", "FIR 32", "tab:orange"),
        ("FIR64-P256-lin-A100", "FIR 64 (selected)", "tab:green"),
        ("FIR96-P256-lin-A100", "FIR 96", "tab:blue"),
        ("FIR128-P256-lin-A100", "FIR 128", "tab:purple")]


def rows(name):
    return list(csv.DictReader(open(os.path.join(MEAS, name))))


def tone_plots():
    data = rows("pure-tone.csv")
    for column, title, ylabel, fname in [
            ("passband_err_db", "Passband error (native-rate sine, host 44.1 kHz)",
             "magnitude error (dB)", "passband.png"),
            ("worst_image_dbc", "Worst reconstruction image (native-rate sine, host 44.1 kHz)",
             "worst image (dBc)", "images.png")]:
        fig, ax = plt.subplots(figsize=(9, 5))
        for key, label, colour in SHOW:
            pts = sorted((float(r["tone_hz"]), float(r[column]))
                         for r in data if r["candidate"] == key and r["host_rate"] == "44100.0")
            ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-", label=label, color=colour)
        ax.axvline(NATIVE_RATE / 2, ls="--", c="grey", lw=1)
        ax.annotate("native Nyquist", (NATIVE_RATE / 2, ax.get_ylim()[0]),
                    rotation=90, va="bottom", ha="right", fontsize=8, color="grey")
        ax.set_xscale("log")
        ax.set_xlabel("tone frequency (Hz)")
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        ax.grid(True, which="both", alpha=0.3)
        ax.legend()
        fig.tight_layout()
        fig.savefig(os.path.join(PLOTS, fname), dpi=130)
        plt.close(fig)


def impulse_plot():
    fig, ax = plt.subplots(figsize=(9, 5))
    rate = 44100
    for key, label, colour in SHOW:
        path = os.path.join(ROOT, "renders", "native", f"host_impulse_{rate}_{key}.f32")
        if not os.path.exists(path):
            continue
        y = np.fromfile(path, dtype=np.float32)
        peak = int(np.argmax(np.abs(y)))
        window = 80
        seg = y[peak - window:peak + window]
        t = (np.arange(len(seg)) - window) / rate * 1000.0
        ax.plot(t, seg, label=label, color=colour, lw=1.2)
    ax.set_xlabel("time relative to peak (ms)")
    ax.set_ylabel("amplitude")
    ax.set_title("Impulse response at 44.1 kHz (native-rate unit impulse)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, "impulse.png"), dpi=130)
    plt.close(fig)


def spectrum_plot():
    """Output spectrum for a 10 kHz native sine: the images are the whole story."""
    rate = 44100
    fig, ax = plt.subplots(figsize=(9, 5))
    for key, label, colour in [SHOW[0], SHOW[2]]:
        path = os.path.join(ROOT, "renders", "native", f"host_sine10k_{rate}_{key}.f32")
        if not os.path.exists(path):
            continue
        y = np.fromfile(path, dtype=np.float32)[20000:20000 + 131072].astype(np.float64)
        w = np.kaiser(len(y), 20.0)
        spectrum = np.abs(np.fft.rfft(y * w))
        spectrum /= spectrum.max()
        freqs = np.fft.rfftfreq(len(y), 1.0 / rate)
        ax.plot(freqs / 1000.0, 20 * np.log10(np.maximum(spectrum, 1e-14)),
                label=label, color=colour, lw=0.8)
    ax.set_xlabel("frequency (kHz)")
    ax.set_ylabel("level relative to fundamental (dB)")
    ax.set_ylim(-160, 5)
    ax.set_title("Host output spectrum, 10 kHz native sine, 44.1 kHz")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, "spectrum_10k.png"), dpi=130)
    plt.close(fig)


def cpu_plot():
    data = rows("cpu.csv")
    fig, ax = plt.subplots(figsize=(9, 5))
    labels, values = [], []
    for key, label, _ in SHOW:
        pts = [float(r["ns_per_output_sample_median"])
               for r in data if r["candidate"] == key and r["host_rate"] == "44100.0"]
        if pts:
            labels.append(label)
            values.append(pts[0])
    ax.barh(labels, values, color=[c for _, _, c in SHOW][:len(labels)])
    for i, v in enumerate(values):
        ax.text(v + 1, i, f"{v:.1f} ns", va="center", fontsize=9)
    ax.set_xlabel("nanoseconds per output sample (median, Release, 44.1 kHz)")
    ax.set_title("Isolated converter cost")
    ax.grid(True, axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, "cpu.png"), dpi=130)
    plt.close(fig)


def main():
    os.makedirs(PLOTS, exist_ok=True)
    tone_plots()
    impulse_plot()
    spectrum_plot()
    cpu_plot()
    print("wrote plots to", PLOTS)


if __name__ == "__main__":
    main()
