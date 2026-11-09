#!/usr/bin/env bash
# Build the MegaMarkdown overlay app.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$ROOT/markdown_viewer/build.sh" "$@"
