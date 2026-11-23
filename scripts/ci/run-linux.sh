#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 MontroneDSP
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
[[ "${GITHUB_ACTIONS:-}" == true && -n "${DISPLAY:-}" ]]
openbox > "$RUNNER_TEMP/swara-wm.log" 2>&1 &
wm_pid=$!
trap 'kill "$wm_pid" 2>/dev/null || true; wait "$wm_pid" 2>/dev/null || true' EXIT
ready=0
for attempt in {1..100}; do
    state=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null || true)
    if [[ "$state" =~ window\ id\ \#\ 0x([[:xdigit:]]+) ]] && (( 16#${BASH_REMATCH[1]} > 0 )); then
        ready=1
        break
    fi
    kill -0 "$wm_pid"
    sleep 0.1
done
if [[ "$ready" != 1 ]]; then
    echo 'X11 window manager did not become ready' >&2
    exit 1
fi
"$@"
