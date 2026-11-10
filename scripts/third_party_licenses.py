#!/usr/bin/env python3
"""Writes THIRD_PARTY_LICENSES.txt: the license and notice files of every Rust
crate compiled into the extension (rust/, for any platform it is built for).

    python3 scripts/third_party_licenses.py           # rewrite the file
    python3 scripts/third_party_licenses.py --check   # fail if it is out of date

Only normal dependencies ship: build scripts and dev-dependencies are left out.
Identical license texts are printed once and referred to afterwards.
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "rust" / "Cargo.toml"
OUTPUT = ROOT / "THIRD_PARTY_LICENSES.txt"
PACKAGE = "orkidea-duckdb"
TARGETS = (
    "x86_64-unknown-linux-gnu",
    "aarch64-unknown-linux-gnu",
    "x86_64-apple-darwin",
    "aarch64-apple-darwin",
    "x86_64-pc-windows-msvc",
)
LICENSE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "NOTICE", "COPYRIGHT")
RULE = "=" * 78


def bundled_packages(target: str) -> dict[str, dict]:
    metadata = json.loads(
        subprocess.run(
            [
                "cargo",
                "metadata",
                "--format-version",
                "1",
                "--locked",
                "--manifest-path",
                str(MANIFEST),
                "--filter-platform",
                target,
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )
    packages = {p["id"]: p for p in metadata["packages"]}
    nodes = {n["id"]: n for n in metadata["resolve"]["nodes"]}
    root = next(p["id"] for p in metadata["packages"] if p["name"] == PACKAGE)
    seen, stack = set(), [root]
    while stack:
        for dep in nodes[stack.pop()]["deps"]:
            normal = any(kind["kind"] is None for kind in dep["dep_kinds"])
            if normal and dep["pkg"] not in seen:
                seen.add(dep["pkg"])
                stack.append(dep["pkg"])
    return {i: packages[i] for i in seen}


def render() -> str:
    packages: dict[str, dict] = {}
    for target in TARGETS:
        packages.update(bundled_packages(target))
    ordered = sorted(packages.values(), key=lambda p: (p["name"], p["version"]))
    lines = [
        "Third-party software compiled into the orkidea DuckDB extension",
        "",
        "The extension statically links the Rust crates below. Each is listed with",
        "its declared license, followed by the license and notice files it ships.",
        "Crates that ship none are under their declared license; the Apache-2.0",
        "text is in LICENSE at the root of this repository.",
        "",
    ]
    for package in ordered:
        lines.append(f"  {package['name']} {package['version']}: {package.get('license') or 'see crate'}")
    printed: dict[str, str] = {}
    for package in ordered:
        name = f"{package['name']} {package['version']}"
        source = Path(package["manifest_path"]).parent
        files = sorted(
            f for f in source.iterdir() if f.is_file() and f.name.upper().startswith(LICENSE_PREFIXES)
        )
        lines += ["", RULE, name, RULE]
        if not files:
            lines.append(f"No license file; declared license: {package.get('license') or 'see crate'}.")
        for f in files:
            text = f.read_text(encoding="utf-8", errors="replace").strip()
            digest = hashlib.sha256(text.encode()).hexdigest()
            if digest in printed:
                lines += ["", f"{f.name}: identical to {printed[digest]}."]
                continue
            printed[digest] = f"{f.name} of {name}"
            lines += ["", f"--- {f.name} ---", "", text]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the file is out of date")
    args = parser.parse_args()
    content = render()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
            print(f"{OUTPUT.name} is out of date; run scripts/third_party_licenses.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
