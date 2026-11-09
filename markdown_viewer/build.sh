#!/usr/bin/env bash
# Build the MegaMarkdown overlay-app blob (.app) for v6.3 / API level 3.
#
# Works in two layouts:
#   1. this portable project        SDK in ../sdk, documents in ../markdown files
#   2. copied into the firmware as  SDK in .. (App/apps/pack_app.py)
#      (set MARKDOWN_DIR to the folder holding the .md sources)
#
# The ARM toolchain is taken from, in order:
#   $TOOLCHAIN_ROOT/bin, <app>/.toolchain/bin, <repo>/.toolchain/bin,
#   <repo>/../.toolchain/bin, PATH.
set -euo pipefail

APP="markdown_viewer"
APP_NAME="MegaMarkdown"
APP_VER="1.0"
APP_API_MIN=3
APP_VMA=${APP_VMA:-0x20000280}
OUT="MegaMarkdown"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

if [ -f "$ROOT/sdk/pack_app.py" ]; then
    SDK="$ROOT/sdk"
elif [ -f "$ROOT/pack_app.py" ]; then
    SDK="$ROOT"
else
    echo "error: pack_app.py not found in $ROOT/sdk or $ROOT" >&2
    exit 1
fi

if [ -n "${TOOLCHAIN_ROOT:-}" ]; then
    export PATH="$TOOLCHAIN_ROOT/bin:$PATH"
elif [ -x "$HERE/.toolchain/bin/arm-none-eabi-gcc" ]; then
    export PATH="$HERE/.toolchain/bin:$PATH"
elif [ -x "$ROOT/.toolchain/bin/arm-none-eabi-gcc" ]; then
    export PATH="$ROOT/.toolchain/bin:$PATH"
elif [ -x "$ROOT/../.toolchain/bin/arm-none-eabi-gcc" ]; then
    export PATH="$ROOT/../.toolchain/bin:$PATH"
fi

if [ -z "${MARKDOWN_DIR:-}" ] && [ -d "$ROOT/markdown files" ]; then
    export MARKDOWN_DIR="$ROOT/markdown files"
fi

CC="${CC:-arm-none-eabi-gcc}"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"
PYTHON="${PYTHON:-python3}"

command -v "$CC" >/dev/null 2>&1 || {
    echo "error: $CC not found." >&2
    echo "       Run ./tools/fetch-toolchain.sh, set TOOLCHAIN_ROOT, or install arm-none-eabi-gcc." >&2
    exit 1
}
command -v "$OBJCOPY" >/dev/null 2>&1 || { echo "error: $OBJCOPY not found" >&2; exit 1; }
command -v "$PYTHON" >/dev/null 2>&1 || { echo "error: $PYTHON not found" >&2; exit 1; }

CFLAGS=(
    -mcpu=cortex-m0plus -mthumb -Oz -fno-jump-tables
    -std=gnu11 -ffreestanding -fno-builtin -fno-common
    -fomit-frame-pointer -ffunction-sections -fdata-sections
    -Wall -Wextra
    "-I$SDK" "-I$HERE"
)
LDFLAGS=(
    -nostdlib -nostartfiles
    -T "$HERE/app.ld"
    "-Wl,--defsym,APP_VMA=$APP_VMA"
    -Wl,--gc-sections
    "-Wl,-Map=$HERE/$APP.map"
    -Wl,--build-id=none
    -Wl,--no-warn-rwx-segments
)

cd "$HERE"
rm -f ./*.app ./*.elf ./*.bin ./*.map markdown_assets.h

echo "Building $APP_NAME $APP_VER (API level >= $APP_API_MIN)"
echo "  SDK       $SDK"
echo "  documents ${MARKDOWN_DIR:-auto}"
echo "  compiler  $("$CC" -dumpversion 2>/dev/null || echo "$CC") ($CC)"
echo "  VMA       $APP_VMA"

PYTHONDONTWRITEBYTECODE=1 "$PYTHON" ./gen_assets.py \
    markdown_assets.bin markdown_assets.h

"$CC" "${CFLAGS[@]}" "${LDFLAGS[@]}" "${APP}_app.c" -lgcc -o "${APP}.elf"
"$OBJCOPY" -O binary "${APP}.elf" "${APP}.bin"

PYTHONDONTWRITEBYTECODE=1 "$PYTHON" "$SDK/pack_app.py" \
    "${APP}.bin" "${OUT}.app" \
    --name "$APP_NAME" --ver "$APP_VER" --api-min "$APP_API_MIN" \
    --vma "$APP_VMA" --screensaver --assets markdown_assets.bin >/dev/null

BYTES=$(wc -c < "${APP}.bin")
ASSETS=$(wc -c < markdown_assets.bin)
if [ "$BYTES" -gt 4096 ]; then
    echo "OVERFLOW: $APP_NAME is $BYTES B, over the 4 KiB overlay" >&2
    exit 1
fi

printf 'OK: %s  %d B code (%d%% of 4 KiB) + %d B assets -> %s\n' \
       "$APP_NAME" "$BYTES" "$((BYTES * 100 / 4096))" "$ASSETS" \
       "$HERE/$OUT.app"
