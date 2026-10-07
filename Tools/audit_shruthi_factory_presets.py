#!/usr/bin/env python3
"""Compare every embedded Shruthi record with the pinned official factory bank."""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPORT_ROOT = ROOT / "artifacts" / "audits"


def safe_report_path(value: Path) -> Path:
    """Resolve --report under artifacts/audits; reject absolute and traversal paths."""
    raw = Path(value)
    if not str(value).strip() or not raw.parts:
        raise ValueError("report path is empty")
    if raw.is_absolute() or raw.anchor:
        raise ValueError("report path must be relative")
    if any(part == ".." for part in raw.parts):
        raise ValueError("report path must not contain '..'")
    if raw.suffix.lower() != ".json" or not raw.name or raw.name in {".", ".."}:
        raise ValueError("report must be a .json file name")
    root = REPORT_ROOT.resolve()
    candidate = (root / raw).resolve()
    if not candidate.is_relative_to(root):
        raise ValueError("report path escapes artifacts/audits")
    return candidate

def compare():
    upstream = ROOT / "third_party/shruthi-1"
    remote = subprocess.check_output(["git", "-C", str(upstream), "remote", "get-url", "origin"], text=True).strip()
    if remote.removesuffix(".git").rstrip("/") not in {
        "https://github.com/pichenettes/shruthi-1", "git@github.com:pichenettes/shruthi-1"
    }:
        raise ValueError(f"Unrecognized upstream: {remote}")
    revision = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    patches = {}
    for line in (upstream / "shruthi/data/factory_data/factory_data.txt").read_text().splitlines():
        match = re.match(r"^patch\s+(.+?)\s+([0-9a-fA-F]+)\s*$", line.strip())
        if match:
            patches[match[1]] = bytes.fromhex(match[2])
    header = (ROOT / "Source/Plugin/ShruthiFactoryPresetData.h").read_text()
    records = re.findall(r'\{\s*"([^"\n]+)",\s*"[^"\n]+",\s*"([^"\n]+)",\s*\{([^}]+)\}', header)
    if len(records) != 40:
        raise ValueError(f"Expected 40 imported records, found {len(records)}")
    results = []
    for name, source, text in records:
        actual = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", text))
        expected = patches[source]
        if len(actual) != 92 or len(expected) != 92:
            raise ValueError(f"Invalid patch length: {name}")
        differences = [{"byte": i, "swara": a, "official": b}
                       for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
        results.append({"preset": name, "upstream_name": source, "match": not differences,
                        "differences": differences})
    return {"remote": remote, "revision": revision, "compared": len(results),
            "matches": sum(row["match"] for row in results), "presets": results}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--report",
        type=Path,
        help="Optional JSON report name under artifacts/audits/",
    )
    args = parser.parse_args()
    report_path = None
    if args.report is not None:
        try:
            report_path = safe_report_path(args.report)
        except ValueError as exc:
            raise SystemExit(f"invalid --report: {exc}") from exc
    result = compare()
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for row in result["presets"]:
        print(f"{row['preset']}: {'MATCH' if row['match'] else 'DIFF'} ({row['upstream_name']})")
    print(f"{result['matches']}/{result['compared']} exact 92-byte matches; official {result['revision']}")
    raise SystemExit(0 if result["matches"] == result["compared"] else 1)
