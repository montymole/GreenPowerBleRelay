#!/usr/bin/env python3
"""Build deterministic, compressed dashboard assets using only Python's stdlib."""
import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ("index.html", "styles.css", "app.js")


def build():
    output = ROOT / "data" / "dashboard"
    output.mkdir(parents=True, exist_ok=True)
    total = 0
    for name in ASSETS:
        source = (ROOT / "dashboard" / name).read_bytes()
        packed = gzip.compress(source, compresslevel=9, mtime=0)
        target = output / (name + ".gz")
        if not target.exists() or target.read_bytes() != packed:
            target.write_bytes(packed)
        total += len(packed)
        print(f"Dashboard: {name}: {len(source)} -> {len(packed)} bytes")
    # Leave ample room for SPIFFS metadata in the existing 128 KiB partition.
    if total > 64 * 1024:
        raise SystemExit("Dashboard exceeds the 64 KiB compressed asset budget")
    print(f"Dashboard assets: {output} ({total} bytes)")


if __name__ == "__main__":
    build()
