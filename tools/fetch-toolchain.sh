#!/usr/bin/env bash
# Install the pinned ARM GNU toolchain inside this project at ./.toolchain.
# The toolchain is gitignored (too large for git) but stays with the directory
# when you copy it to another machine of the same architecture.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${ARM_GCC_VERSION:-13.3.rel1}"

case "$(uname -m)" in
    x86_64|amd64)  ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) echo "error: unsupported host architecture $(uname -m)" >&2; exit 1 ;;
esac

DEST="$ROOT/.toolchain"
if [ -x "$DEST/bin/arm-none-eabi-gcc" ] && \
   "$DEST/bin/arm-none-eabi-gcc" --version >/dev/null 2>&1; then
    echo "Toolchain already installed: $("$DEST/bin/arm-none-eabi-gcc" -dumpversion)"
    echo "  $DEST"
    exit 0
fi

# A toolchain copied from another host architecture fails to execute; replace
# it with the one for this machine.
if [ -e "$DEST" ]; then
    echo "Existing .toolchain is not usable on $(uname -m); reinstalling"
    rm -rf "$DEST"
fi

TARBALL="arm-gnu-toolchain-${VERSION}-${ARCH}-arm-none-eabi.tar.xz"
URL="https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu/${VERSION}/binrel/${TARBALL}"
CACHE="$ROOT/.toolchain-dl"
STAGE="$ROOT/.toolchain-tmp"

mkdir -p "$CACHE"
if [ ! -s "$CACHE/$TARBALL" ]; then
    echo "Downloading $URL"
    curl -fL --retry 3 -o "$CACHE/$TARBALL" "$URL"
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
trap 'rm -rf "$STAGE"' EXIT

echo "Extracting to $DEST"
tar -xf "$CACHE/$TARBALL" -C "$STAGE" --strip-components=1
rm -rf "$DEST"
mv "$STAGE" "$DEST"
trap - EXIT

# The extracted toolchain is what builds need; the archive is optional.
if [ "${KEEP_TOOLCHAIN_ARCHIVE:-0}" != "1" ]; then
    rm -f "$CACHE/$TARBALL"
    rmdir "$CACHE" 2>/dev/null || true
fi

"$DEST/bin/arm-none-eabi-gcc" --version | head -1
echo "Installed at $DEST"
echo "Now run ./build.sh"
