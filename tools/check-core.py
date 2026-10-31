"""Compile the four portable production cores with their existing C fixtures.

Requires Python 3.10+ and a C11 GCC/Clang-compatible compiler. No SDK, downloads,
network, device access or historical evidence is required. Run in Linux/WSL.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "firmware/core"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"),
                        help="compiler command; defaults to CC or cc")
    parser.add_argument("--sanitize", action="store_true", help="enable ASan and UBSan")
    parser.add_argument("--output", type=Path, default=Path(".local/evidence/core-check"))
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("a compiler command is required")
    out = (ROOT / args.output).resolve()
    if not out.is_relative_to((ROOT / ".local").resolve()):
        parser.error("output must stay inside this checkout's .local directory")
    out.mkdir(parents=True, exist_ok=True)
    cases = [
        ("brightness", "brightness_policy", "tools/brightness-policy/test_policy.c",
         [str(out / "brightness.csv")]),
        ("comfort", "comfort", "tools/indoor-comfort/test.c", []),
        ("solar", "solar_core", "tools/solar-core/test_core.c", []),
        ("alarm", "alarm_core", "tools/alarm-foundation/core_test.c",
         [str(ROOT / "tools/alarm-foundation/fixtures/2026.afy")]),
    ]
    files = set(CORE.glob("*.c")) | set(CORE.glob("*.h")) | {Path(__file__).resolve()}
    files.update(ROOT / fixture for _, _, fixture, _ in cases)
    files.add(ROOT / "tools/alarm-foundation/fixtures/2026.afy")
    before = {p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(files)}
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    commands = []

    def run(name, command):
        result = subprocess.run(command, input="", text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, cwd=ROOT)
        log = out / (name + ".log")
        log.write_text(result.stdout, encoding="utf-8")
        commands.append({"name": name, "command": command, "exit_code": result.returncode,
                         "log_sha256": sha(log)})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            print(result.stdout, end="")
            result.check_returncode()
        return result.stdout

    version = run("compiler-version", compiler + ["--version"]).splitlines()[0]
    for name, stem, fixture, extra in cases:
        binary = out / name
        run(name + "-compile", compiler + flags + ["-I", str(CORE),
            str(CORE / (stem + ".c")), str(ROOT / fixture), "-lm", "-o", str(binary)])
        text = run(name + "-run", [str(binary)] + extra)
        print(name + ": " + (text.strip().splitlines()[-1] if text.strip() else "PASS"))
    after = {p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(files)}
    if before != after:
        raise RuntimeError("source files changed during this check")
    report = {"passed": True, "groups": [case[0] for case in cases],
              "sanitizers": args.sanitize, "compiler": version, "source_sha256": before,
              "scope": "existing C fixtures only; no SDK, network, device or UI checks"}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS four portable core groups; evidence: " + str(out))


if __name__ == "__main__":
    main()
