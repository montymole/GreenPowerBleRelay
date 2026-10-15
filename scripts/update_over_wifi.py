#!/usr/bin/env python3
"""Build and upload relay firmware/dashboard over the authenticated HTTP API."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parent.parent
CHIPS = {"esp32-c3-devkitm-1": "ESP32-C3", "esp32-c6-lcd-147": "ESP32-C6"}


def request(base, key, path, data=None, headers=None):
    req = urllib.request.Request(base + path, data=data,
                                 headers={"X-Api-Key": key, **(headers or {})})
    # Never forward credentials via redirects or a machine-wide HTTP proxy.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect)
    with opener.open(req, timeout=45) as response:
        return json.load(response)


def upload(base, key, kind, image):
    payload = image.read_bytes()
    boundary = "relay-" + uuid.uuid4().hex
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; "
            f"filename=\"{image.name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
    body += payload + f"\r\n--{boundary}--\r\n".encode()
    print(f"Uploading {kind}: {len(payload)} bytes", flush=True)
    result = request(base, key, f"/api/v1/update/{kind}", body, {
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "X-Update-Size": str(len(payload)),
        "X-Update-MD5": hashlib.md5(payload).hexdigest(),
    })
    if result.get("ok") is not True:
        raise RuntimeError(f"Update rejected: {result}")
    print(f"{kind}: verified and installed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="Relay URL, e.g. http://192.168.1.50")
    parser.add_argument("--environment", "-e", choices=CHIPS, default="esp32-c3-devkitm-1")
    parser.add_argument("--target", choices=("dashboard", "firmware", "all"), default="all")
    parser.add_argument("--no-build", action="store_true", help="Use existing PlatformIO images")
    args = parser.parse_args()
    base = args.url.rstrip("/")
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.path or parsed.query or parsed.fragment or parsed.username:
        parser.error("URL must be an http(s) origin without a path or credentials")
    key = os.environ.get("RELAY_API_KEY") or getpass.getpass("Relay API key: ")
    if not key:
        parser.error("An API key is required")
    info = request(base, key, "/api/v1/update")
    if info.get("chip") != CHIPS[args.environment] or info.get("filesystem") != "spiffs":
        raise RuntimeError(f"Device does not match {args.environment}: {info}")
    if not args.no_build:
        if args.target in ("firmware", "all"):
            subprocess.run(["pio", "run", "-e", args.environment], cwd=ROOT, check=True)
        if args.target in ("dashboard", "all"):
            subprocess.run(["pio", "run", "-e", args.environment, "-t", "buildfs"], cwd=ROOT, check=True)
    build = ROOT / ".pio" / "build" / args.environment
    if args.target in ("firmware", "all"):
        upload(base, key, "firmware", build / "firmware.bin")
        print("Waiting for reboot...", flush=True)
        time.sleep(3)
        deadline = time.monotonic() + 120
        while True:
            try:
                info = request(base, key, "/api/v1/update")
                if info.get("chip") != CHIPS[args.environment]:
                    raise RuntimeError("Unexpected device after reboot")
                break
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Firmware accepted, but relay did not return within 120 seconds")
                time.sleep(2)
    if args.target in ("dashboard", "all"):
        upload(base, key, "dashboard", build / "spiffs.bin")
    print(f"Done. Dashboard: {base}/")


if __name__ == "__main__":
    try:
        main()
    except urllib.error.HTTPError as error:
        raise SystemExit(f"HTTP {error.code}: {error.read().decode(errors='replace')}")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
