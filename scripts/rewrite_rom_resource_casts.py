#!/usr/bin/env python3
"""Rewrite Shruthi ROM table negative literals as explicit unsigned casts.

Upstream tables store signed intervals as AVR unsigned bit patterns.
Explicit casts preserve the exact two's-complement values for host builds.
"""
from __future__ import annotations

import re
import sys


ARRAY_START = re.compile(
    r"const\s+prog_uint(8|16)_t\s+\w+\[\]\s+PROGMEM\s*=\s*\{",
    re.MULTILINE,
)
NEG = re.compile(r"(?<![\w.])-(\d+)\b")


def rewrite(text: str) -> str:
    out: list[str] = []
    pos = 0
    for match in ARRAY_START.finditer(text):
        out.append(text[pos : match.end()])
        width = match.group(1)
        cast = f"prog_uint{width}_t"
        close = text.find("};", match.end())
        if close < 0:
            raise SystemExit("unclosed ROM array initializer")
        body = text[match.end() : close]
        body = NEG.sub(rf"static_cast<{cast}>(-\1)", body)
        out.append(body)
        out.append("};")
        pos = close + 2
    out.append(text[pos:])
    return "".join(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <resources.cc>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8", newline="") as fh:
        original = fh.read()
    updated = rewrite(original)
    if updated != original:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(updated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
