#!/usr/bin/env bash
# Decode the generated assets with the C decoder and compare the result with
# the Python renderer.  Runs on the build host; needs only gcc and python3.
#
# The 32-bit app_api_t layout asserts in sdk/app_api.h do not hold with the
# host's 64-bit pointers, so this test compiles against a temporary copy with
# just those asserts removed.  The radio build uses the unmodified header.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCS="${MARKDOWN_DIR:-$ROOT/markdown files}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/megamarkdown-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

command -v gcc >/dev/null 2>&1 || { echo "error: gcc not found" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "error: python3 not found" >&2; exit 1; }

echo "Generating assets from: $DOCS"
(
    cd "$ROOT/markdown_viewer"
    MARKDOWN_DIR="$DOCS" PYTHONDONTWRITEBYTECODE=1 \
        ./gen_assets.py markdown_assets.bin markdown_assets.h
)

mkdir -p "$TMP/sdk"
cp "$ROOT/sdk/app_api.h" "$TMP/sdk/app_api.h"
python3 - "$TMP/sdk/app_api.h" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text()
start = text.index("_Static_assert(offsetof(app_api_t, sys_edition)")
marker = '"app_api_t is append-only: every edition shares this layout");'
end = text.index(marker, start) + len(marker)
path.write_text(text[:start] + "/* host test: 32-bit layout asserts omitted */" + text[end:])
PY

gcc -std=gnu11 -O1 -Wall -Wextra \
    -I"$TMP/sdk" -I"$ROOT/markdown_viewer" -I"$ROOT/sdk" \
    "$ROOT/test/host_decode.c" -o "$TMP/host_decode"

for WIDTH in 18 32; do
    python3 - "$ROOT" "$DOCS" "$TMP/expected-$WIDTH.txt" "$WIDTH" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
docs = Path(sys.argv[2])
width = int(sys.argv[4])
sys.path.insert(0, str(root / "markdown_viewer"))
from gen_assets import DOC_SUFFIXES, render_markdown

paths = sorted(
    (p for p in docs.iterdir()
     if p.is_file() and not p.name.startswith(".")
     and p.suffix.lower() in DOC_SUFFIXES),
    key=lambda p: p.name.lower())
with open(sys.argv[3], "w", encoding="ascii") as out:
    for path in paths:
        lines = render_markdown(
            path.read_text(encoding="utf-8", errors="replace"), width)
        out.write("\n".join(lines) + "\n")
PY
done

for MODE in normal tiny; do
    if [ "$MODE" = "tiny" ]; then WIDTH=32; else WIDTH=18; fi
    "$TMP/host_decode" "$ROOT/markdown_viewer/markdown_assets.bin" "$MODE" \
        > "$TMP/decoded-$MODE.txt"
    if ! cmp -s "$TMP/expected-$WIDTH.txt" "$TMP/decoded-$MODE.txt"; then
        echo "DECODER MISMATCH ($MODE)" >&2
        diff -u "$TMP/expected-$WIDTH.txt" "$TMP/decoded-$MODE.txt" \
            | head -40 >&2 || true
        exit 1
    fi
done

echo "Decoder matches the Python renderer at 18 and 32 columns."
