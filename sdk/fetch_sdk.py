#!/usr/bin/env python3
"""Download the overlay-app SDK for firmware v6.3.0 (ABI 1, API level 3).

The build expects `app_api.h`, `app_overlay.h` and `pack_app.py` in this
directory.  This script fetches those files, plus the two optional upstream
helpers, directly from the pinned firmware commit.  A fresh clone can then
build without cloning the whole firmware.

Files that are already present with the expected SHA-256 hash are left alone.

Usage:
    python3 sdk/fetch_sdk.py
    python3 sdk/fetch_sdk.py --force       # refresh every file
    python3 sdk/fetch_sdk.py --dest DIR    # write somewhere else

Only the Python 3 standard library is required.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

FIRMWARE_TAG = "v6.3.0"
COMMIT = "ca3a4271386049e7b459b215cef0ad0eea1a30ca"
REPO = "armel/uv-k1-k5v3-firmware-custom"
URL_BASE = f"https://raw.githubusercontent.com/{REPO}/{COMMIT}/App/apps"

# name -> SHA-256 of the file at the pinned commit
FILES = {
    "app_api.h": "eb3e2c2d94e7ae6eaeb0fa289e15b832b0db13f7242772f92aa7571785882cdf",
    "app_overlay.h": "bf4701f5e71c14ba19498cd6f895740e857cf1b6eb622214c17dbf55e6e058c4",
    "pack_app.py": "8c40e1eeb212af2973b6bce8de00d51d18405e45c5b6927dd3129094535a238a",
    "app_assets.py": "856198603091f8fb121b66531f0edeb7905989d958c9e5a12046f18a923be39e",
    "app_art.py": "5c1db4b1ee6f80718d4df2541a3ce84aca651c40794c160c2fc2cc8be4c305da",
}

RETRIES = 3
TIMEOUT = 30
USER_AGENT = "MegaMarkdown-fetch-sdk/1.0"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, target: Path, expected: str, timeout: float) -> None:
    """Download *url* to *target*, replacing it only after the hash matches."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        fd, temp_name = tempfile.mkstemp(
            dir=target.parent, prefix=f".{target.name}.", suffix=".part")
        temp_path = Path(temp_name)
        try:
            digest = hashlib.sha256()
            with os.fdopen(fd, "wb") as out:
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
                    digest.update(chunk)
                out.flush()
                os.fsync(out.fileno())

            actual = digest.hexdigest()
            if actual != expected:
                raise ValueError(
                    f"{target.name}: SHA-256 mismatch\n"
                    f"  expected {expected}\n"
                    f"  received {actual}")
            os.chmod(temp_path, 0o644)
            os.replace(temp_path, target)
        except BaseException:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
            raise


def fetch_one(name: str, expected: str, dest: Path, force: bool,
              timeout: float) -> None:
    target = dest / name

    if target.is_file() and not force:
        if sha256_file(target) == expected:
            print(f"  {name:<16} up to date")
            return
        print(f"  {name:<16} differs, downloading")

    url = f"{URL_BASE}/{name}"
    last_error: Exception | None = None
    for attempt in range(1, RETRIES + 1):
        try:
            download(url, target, expected, timeout)
            print(f"  {name:<16} downloaded")
            return
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
            last_error = exc
            if attempt < RETRIES:
                time.sleep(attempt)

    raise RuntimeError(f"could not download {url}: {last_error}")


def cdefine(path: Path, name: str) -> int:
    pattern = re.compile(
        r"^\s*#define\s+" + re.escape(name) +
        r"\s+(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b")
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.match(line)
            if match:
                return int(match.group(1), 0)
    raise ValueError(f"{path.name}: #define {name} not found")


def verify(dest: Path) -> tuple[int, int, int]:
    abi = cdefine(dest / "app_api.h", "APP_ABI_MAJOR")
    api = cdefine(dest / "app_api.h", "APP_API_LEVEL")
    asset_max = cdefine(dest / "app_overlay.h", "APP_ASSET_MAX")

    if abi != 1 or api != 3:
        raise ValueError(
            f"expected ABI 1 / API level 3, found ABI {abi} / API level {api}")
    return abi, api, asset_max


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Download the firmware v6.3.0 overlay-app SDK "
                    "(ABI 1, API level 3).")
    parser.add_argument(
        "--dest",
        default=str(Path(__file__).resolve().parent),
        help="destination directory (default: the directory holding this script)")
    parser.add_argument(
        "--force", action="store_true",
        help="download every file again, even if its hash already matches")
    parser.add_argument(
        "--timeout", type=float, default=TIMEOUT,
        help=f"per-request timeout in seconds (default: {TIMEOUT})")
    args = parser.parse_args(argv)

    dest = Path(args.dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)

    print(f"Firmware {FIRMWARE_TAG} SDK, commit {COMMIT[:12]}")
    try:
        for name, expected in FILES.items():
            fetch_one(name, expected, dest, args.force, args.timeout)
        abi, api, asset_max = verify(dest)
    except (urllib.error.URLError, TimeoutError, OSError,
            ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        print("Check your internet connection and try again.", file=sys.stderr)
        return 1

    print(f"OK: ABI {abi}, API level {api}, {asset_max} B asset area")
    print(f"SDK files are in {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
