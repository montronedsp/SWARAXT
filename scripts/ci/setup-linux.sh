#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
compiler="${1:?compiler required}"
[[ "$compiler" == gcc || "$compiler" == clang ]]
[[ "${GITHUB_ACTIONS:-}" == true ]]
sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential pkg-config ninja-build \
    libasound2-dev libfreetype6-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
    libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev libxfixes-dev \
    libgl1-mesa-dev libgtk-3-dev libgtkmm-3.0-dev ladspa-sdk xvfb xauth x11-utils openbox ca-certificates curl gnupg
[[ -f /usr/include/ladspa.h ]]
pkg-config --modversion gtk+-3.0 gtkmm-3.0
if [[ "$compiler" == clang ]]; then
    curl --fail --silent --show-error https://apt.llvm.org/llvm-snapshot.gpg.key \
        --output "$RUNNER_TEMP/swara-llvm.asc"
    gpg --show-keys --with-colons "$RUNNER_TEMP/swara-llvm.asc" \
        | awk -F: '$1 == "fpr" { print $10 }' \
        | grep -Fx 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
    sudo gpg --dearmor --yes --output /usr/share/keyrings/swara-llvm.gpg "$RUNNER_TEMP/swara-llvm.asc"
    echo 'deb [signed-by=/usr/share/keyrings/swara-llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' \
        | sudo tee /etc/apt/sources.list.d/swara-llvm.list
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends clang-21 libclang-rt-21-dev lld-21 llvm-21
    dpkg-query -W clang-21 libclang-rt-21-dev lld-21 llvm-21
fi
