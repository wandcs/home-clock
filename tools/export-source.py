"""Export reviewed tracked product files without development history or local inputs.

Run in a clean Git checkout. The output is a new directory beneath .local;
this command never initializes Git, publishes, removes files or opens a device.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FILES = {
    ".gitignore", ".gitattributes", "README.md", "LICENSE",
    "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md",
    "firmware/.gitignore", "firmware/CMakeLists.txt", "firmware/dependencies.lock",
    "firmware/dependency-constraints.txt", "firmware/partitions.csv",
    "firmware/sdkconfig", "firmware/sdkconfig.defaults",
    "firmware/sdkconfig.development", "firmware/toolchain-lock.json",
    "tools/build.ps1", "tools/check-build.py", "tools/check-core.py",
    "tools/generate-home-assets.py",
    "tools/prepare-weather-contact.py", "tools/export-source.py",
    "tools/alarm-audio/generate.py", "tools/alarm-audio/synthesize.py",
    "tools/brightness-policy/test_policy.c", "tools/indoor-comfort/test.c",
    "tools/solar-core/test_core.c", "tools/alarm-foundation/core_test.c",
    "tools/alarm-foundation/fixtures/2026.afy",
    "docs/README.md", "docs/hardware.md", "docs/getting-started.md",
    "docs/building.md", "docs/usage.md", "docs/architecture.md",
    "docs/open-source/README.md", "docs/open-source/data-sources.md",
    "docs/images/home.png", "docs/images/weather-scenes.png",
    "docs/images/forecast.png", "docs/images/alarms.png",
    "docs/images/weather-motion.gif", "docs/images/swipe.gif",
}
PREFIXES = (
    "firmware/main/", "firmware/features/", "firmware/core/",
    "components/waveshare__esp32_p4_wifi6_touch_lcd_7b/",
    "assets/application/", "assets/weather-background/",
    "assets/alarm-audio/", "LICENSES/",
)
BINARY_SUFFIXES = {".bin", ".pcm", ".lco", ".afy", ".png", ".gif"}
PATTERNS = {
    "user directory": re.compile(r"(?i)[a-z]:[/\\]Users[/\\][^/\\\s]+"),
    "local workspace": re.compile(r"(?i)(?:[a-z]:[/\\]repos[/\\]|/mnt/[a-z]/repos/)"),
    "device MAC": re.compile(r"\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "access token": re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{35,}|AKIA[A-Z0-9]{16})\b"),
}
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@([A-Za-z0-9.-]+\.[A-Za-z]{2,})")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def privacy_findings(name, data):
    path = Path(name)
    # Only the known generated binary formats are excluded. Also scan files
    # without extensions, including licenses, Kconfig and ignore rules.
    if path.suffix.lower() in BINARY_SUFFIXES:
        if path.suffix.lower() == ".png" and not data.startswith(b"\x89PNG\r\n\x1a\n"):
            return [(name, "invalid PNG signature")]
        if path.suffix.lower() == ".gif" and not data.startswith((b"GIF87a", b"GIF89a")):
            return [(name, "invalid GIF signature")]
        return []
    try:
        text = data.decode("utf8")
    except UnicodeDecodeError:
        return [(name, "non-UTF8 text requires review")]
    result = [(name, label) for label, pattern in PATTERNS.items() if pattern.search(text)]
    legal = name.startswith("LICENSES/") or "LICENSE" in path.name or path.name in {"OFL.txt", "Location-OGDL.txt"}
    if not legal:
        for match in EMAIL.finditer(text):
            domain = match.group(1).lower()
            if domain not in {"example.com", "example.org", "example.net", "example.invalid"}:
                result.append((name, "contact email outside a license")); break
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(".local/public-source"))
    args = parser.parse_args()
    target = (ROOT / args.output).resolve()
    if not target.is_relative_to((ROOT / ".local").resolve()) or target == (ROOT / ".local").resolve():
        parser.error("output must be a new child directory of this checkout's .local")
    if target.exists():
        parser.error("output already exists; choose a new directory (nothing is deleted)")
    if git("status", "--porcelain").strip():
        parser.error("commit the reviewed changes before exporting")
    tracked = {p.decode("utf8") for p in git("ls-files", "-z").split(b"\0") if p}
    missing = sorted(FILES - tracked)
    if missing:
        parser.error("required public files are not tracked: " + ", ".join(missing))
    selected = sorted(name for name in tracked if name in FILES or name.startswith(PREFIXES))
    findings = []
    for name in selected:
        source = ROOT / name
        if source.is_symlink() or not source.resolve().is_relative_to(ROOT):
            parser.error("symlinks or external files require separate review: " + name)
        findings.extend(privacy_findings(name, source.read_bytes()))
    if findings:
        for name, category in findings:
            print(category + ": " + name, file=sys.stderr)
        parser.error("review these paths before export; matching values were not printed")
    target.mkdir(parents=True)
    files = {}
    for name in selected:
        source = ROOT / name;dest = target / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        # Match the public Git attributes: source/docs use LF; checksum-bound
        # generated assets and known binary formats retain their exact bytes.
        if not name.startswith("assets/") and source.suffix.lower() not in BINARY_SUFFIXES:
            try:
                data.decode("utf8")
                data = data.replace(b"\r\n", b"\n")
            except UnicodeDecodeError:
                pass
        dest.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        if sha(dest) != digest:
            raise RuntimeError("export changed expected bytes: " + name)
        files[name] = {"bytes": len(data), "sha256": digest}
    manifest = {
        "source_commit": git("rev-parse", "HEAD").decode().strip(),
        "file_count": len(files), "files": files,
        "scope": "reviewed product snapshot; no Git history, local config, SDK, build outputs or device evidence",
        "privacy_check": "bounded text patterns; known binary resources excluded; other selected UTF8 text scanned; not a guarantee against every possible secret",
    }
    (target / "SOURCE_MANIFEST.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf8")
    print(f"Exported {len(files)} reviewed files to {target}; no Git history or device action.")


if __name__ == "__main__":
    main()
