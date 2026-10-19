#!/usr/bin/env python3
"""Inspect generated Linux qualification commands; exit nonzero on mismatch."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--expect-build-type", required=True, choices=("Release", "Debug", "RelWithDebInfo", "MinSizeRel"))
parser.add_argument("--require-sanitizer", choices=("address,undefined", "thread", "realtime"))
args = parser.parse_args()
cache = {}
for line in (args.build / "CMakeCache.txt").read_text().splitlines():
    if not line or line.startswith(("//", "#")) or "=" not in line:
        continue
    key, value = line.split("=", 1)
    cache[key.split(":", 1)[0]] = value
if cache.get("CMAKE_BUILD_TYPE") != args.expect_build_type:
    raise SystemExit("FAIL: CMAKE_BUILD_TYPE differs from requested mode")
commands_file = args.build / "compile_commands.json"
if not commands_file.is_file():
    raise SystemExit("NOT RUN: compile_commands.json required; inspect MSVC generated rules separately")
commands = json.loads(commands_file.read_text())
core = cache.get("CMAKE_PROJECT_NAME") == "SwaraXTCoreQualification"
selected = [entry for entry in commands if core or "Tests/" in entry.get("command", "")]
if not selected:
    raise SystemExit("FAIL: no test/production-library commands selected")
errors = []
optimizations = set()
definitions = set()
for entry in selected:
    words = entry.get("arguments") or shlex.split(entry["command"])
    optimization = [word for word in words if re.fullmatch(r"-O(?:[0-3sgz]|fast)", word)]
    optimization = optimization[-1] if optimization else "compiler-default"
    optimizations.add(optimization)
    definitions.update(word for word in words if word in ("-DNDEBUG", "-DNDEBUG=1", "-DDEBUG=1", "-D_DEBUG=1"))
    if args.expect_build_type != "Debug" and optimization in ("compiler-default", "-O0"):
        errors.append((entry["file"], "missing effective optimization"))
    if args.expect_build_type != "Debug" and not any(word in ("-DNDEBUG", "-DNDEBUG=1") for word in words):
        errors.append((entry["file"], "optimized assertion policy lacks NDEBUG"))
    if args.expect_build_type == "Debug" and any(word in ("-DNDEBUG", "-DNDEBUG=1") for word in words):
        errors.append((entry["file"], "Debug unexpectedly disables assertions"))
    if args.expect_build_type == "Debug" and not any(word.startswith("-g") and word != "-g0" for word in words):
        errors.append((entry["file"], "missing debug symbols"))
    if args.require_sanitizer and "-fsanitize=" + args.require_sanitizer not in words:
        errors.append((entry["file"], "production/test TU lacks required sanitizer"))
    if args.require_sanitizer and "-fno-omit-frame-pointer" not in words:
        errors.append((entry["file"], "missing sanitizer frame pointers"))
compiler = cache.get("CMAKE_CXX_COMPILER")
version = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
links_checked = 0
if args.require_sanitizer:
    if cache.get("CMAKE_GENERATOR") != "Ninja":
        errors.append((str(args.build), "sanitizer linkage verification requires the Ninja qualification generator"))
    else:
        generated = subprocess.check_output(
            ["ninja", "-C", str(args.build), "-t", "commands"], text=True)
        for command in generated.splitlines():
            words = shlex.split(command)
            if compiler not in words or "-c" in words or (not core and "Tests/" not in command):
                continue
            links_checked += 1
            if "-fsanitize=" + args.require_sanitizer not in words:
                errors.append((command, "test executable lacks required sanitizer linkage"))
        if links_checked == 0:
            errors.append((str(args.build), "no executable sanitizer link commands found"))
print(json.dumps({"status": "FAIL" if errors else "PASS", "compiler": compiler,
                  "version": version, "build_type": args.expect_build_type,
                  "commands_checked": len(selected), "optimization": sorted(optimizations),
                  "sanitizer_links_checked": links_checked,
                  "assertion_definitions": sorted(definitions), "errors": errors}, indent=2))
raise SystemExit(1 if errors else 0)
