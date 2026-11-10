#!/usr/bin/env python3
"""Download app README files from the UV-K1/K5 v3 firmware repository.

The script lists the immediate folders in App/apps on the main branch, probes
each one for a README.md, and saves it in this folder as <folder>.md.  The
top-level App/apps/README.md is intentionally ignored.

Usage:
    python3 "markdown files/download_armel_app_readmes.py"
    python3 "markdown files/download_armel_app_readmes.py" --overwrite
    python3 "markdown files/download_armel_app_readmes.py" --skip-existing

By default, if a destination file already exists with different contents, the
script asks before overwriting it.  Files whose contents are already identical
are reported as unchanged.  Only the Python 3 standard library is required.

The GitHub directory listing is subject to the unauthenticated API rate limit.
If that becomes a problem, set GITHUB_TOKEN to a personal access token.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "armel/uv-k1-k5v3-firmware-custom"
BRANCH = "main"
APPS_PATH = "App/apps"
API_URL = f"https://api.github.com/repos/{REPO}/contents/{APPS_PATH}?ref={BRANCH}"
RAW_URL = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/{APPS_PATH}"
USER_AGENT = "MegaMarkdown-fetch-app-readmes/1.0"
TIMEOUT = 30
RETRIES = 3


class NotFound(Exception):
    """The server answered 404 for this URL."""


def request_bytes(url: str, timeout: float, accept: str | None = None) -> bytes:
    """Fetch *url* as bytes, retrying transient network errors."""
    headers = {"User-Agent": USER_AGENT}
    if accept:
        headers["Accept"] = accept
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(url, headers=headers)
    last_error: Exception | None = None

    for attempt in range(1, RETRIES + 1):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                raise NotFound(url) from exc
            last_error = exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc

        if attempt < RETRIES:
            time.sleep(attempt)

    raise RuntimeError(f"{url}: {last_error}")


def list_app_folders(timeout: float) -> list[str]:
    payload = json.loads(request_bytes(
        API_URL, timeout, accept="application/vnd.github+json"))
    if not isinstance(payload, list):
        raise RuntimeError("GitHub returned an unexpected directory listing")

    folders = sorted({
        entry["name"]
        for entry in payload
        if entry.get("type") == "dir" and entry.get("name")
    })
    if not folders:
        raise RuntimeError(f"no folders found in {APPS_PATH}")
    return folders


def readme_url(folder: str) -> str:
    name = urllib.parse.quote(folder, safe="")
    return f"{RAW_URL}/{name}/README.md"


def write_file(target: Path, data: bytes) -> None:
    """Write *data* atomically, so an interrupted download cannot truncate a
    file that is already in the folder."""
    fd, temp_name = tempfile.mkstemp(
        dir=target.parent, prefix=f".{target.name}.", suffix=".part")
    temp_path = Path(temp_name)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(temp_path, 0o644)
        os.replace(temp_path, target)
    except BaseException:
        try:
            temp_path.unlink()
        except FileNotFoundError:
            pass
        raise


def save_readme(target: Path, data: bytes, source: str,
                mode: str) -> str:
    """Save *data*, returning saved/overwritten/unchanged/skipped."""
    if target.exists():
        try:
            if target.is_file() and target.read_bytes() == data:
                return "unchanged"
        except OSError:
            pass

        if mode == "overwrite":
            write_file(target, data)
            return "overwritten"
        if mode == "skip":
            return "skipped"

        try:
            answer = input(
                f"         {target.name} already exists. "
                f"Overwrite with {source}? [y/N] ")
        except EOFError:
            print()
            return "skipped"
        if answer.strip().lower() in ("y", "yes"):
            write_file(target, data)
            return "overwritten"
        return "skipped"

    write_file(target, data)
    return "saved"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Download each app's README.md from the UV-K1/K5 v3 "
                    "firmware's App/apps folders.")
    parser.add_argument(
        "--dest",
        default=str(Path(__file__).resolve().parent),
        help="destination directory (default: the directory holding this script)")
    parser.add_argument(
        "--timeout", type=float, default=TIMEOUT,
        help=f"per-request timeout in seconds (default: {TIMEOUT})")
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--overwrite", action="store_true",
        help="overwrite existing files without asking")
    group.add_argument(
        "--skip-existing", action="store_true",
        help="keep existing files without asking")
    args = parser.parse_args(argv)

    mode = "overwrite" if args.overwrite else (
        "skip" if args.skip_existing else "prompt")
    dest = Path(args.dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)

    print(f"Repository: {REPO} (branch {BRANCH})")
    print(f"Source:     {APPS_PATH}/*/README.md")
    print(f"Destination: {dest}")
    print()

    try:
        print(f"Looking for folders in {APPS_PATH} ...")
        folders = list_app_folders(args.timeout)
    except (RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        print("If GitHub rate-limited the request, set GITHUB_TOKEN and retry.",
              file=sys.stderr)
        return 1

    print(f"Found {len(folders)} folders. Probing each for README.md.")
    print()

    counts = {
        "saved": 0,
        "overwritten": 0,
        "unchanged": 0,
        "skipped": 0,
        "missing": 0,
        "failed": 0,
    }

    for index, folder in enumerate(folders, start=1):
        source = f"{APPS_PATH}/{folder}/README.md"
        target = dest / f"{folder}.md"

        print(f"[{index:2d}/{len(folders)}] {folder}")
        print(f"         probing {source}")

        try:
            data = request_bytes(readme_url(folder), args.timeout)
        except NotFound:
            print("         README.md not found - skipped")
            counts["missing"] += 1
            continue
        except RuntimeError as exc:
            print(f"         download failed: {exc}")
            counts["failed"] += 1
            continue

        status = save_readme(target, data, source, mode)
        counts[status] += 1

        if status == "saved":
            print(f"         saved {target.name} ({len(data)} B)")
        elif status == "overwritten":
            print(f"         overwrote {target.name} ({len(data)} B)")
        elif status == "unchanged":
            print(f"         {target.name} is already up to date")
        else:
            print(f"         kept existing {target.name}")

    print()
    print("Done: "
          f"{counts['saved']} new, "
          f"{counts['overwritten']} overwritten, "
          f"{counts['unchanged']} unchanged, "
          f"{counts['skipped']} skipped, "
          f"{counts['missing']} missing, "
          f"{counts['failed']} failed.")
    print(f"Files are in {dest}")
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
