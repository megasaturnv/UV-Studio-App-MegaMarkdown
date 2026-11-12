# MegaMarkdown — Markdown viewer for UV-K1 / UV-K5 v3 by MegaSaturnv

**MegaMarkdown** is a portable overlay app that reads Markdown documents on the
[armel/uv-k1-k5v3-firmware-custom](https://github.com/armel/uv-k1-k5v3-firmware-custom)
firmware, targeting **v6.3.0, ABI 1, API level 3**.

With the amount of apps people are developing, I wanted a way to view their readme files on the device. I would have liked to develop this by hand, but due to time constraints AI has been used for most of the code and then reviewed. This will allow me more time to study API level 4. Over time I would like to minimise this app as much as possible to allow room for more markdown files. Most of the text below this line is AI-written, with a few tweaks.

![Main Menu](/images/Menu.png)
![Markdown Normal Font](/images/MarkdownFileNormal.png)
![Markdown Small Font](/images/MarkdownFileSmall.png)

---
## Adding Markdown Files

The documents live in [`markdown files/`](markdown%20files/). At build time each
file is converted to plain text paragraphs, compressed with a small Huffman
codec, and packed into the app's read-only flash assets. The radio app then
decompresses only the text it draws and wraps it to the screen. Press `0` in the
reader to switch between the normal 6x8 font (18 columns) and the radio's 3x5
font (32 columns and nine rows).

This project includes the ARM toolchain installer and an SDK fetch script, so a
fresh checkout can build without cloning the firmware. It is also usable inside
the firmware tree as `App/apps/markdown_viewer/`.

## Quick Start

```sh
./tools/fetch-toolchain.sh   # once; installs ./.toolchain (gitignored)
python3 sdk/fetch_sdk.py     # once; downloads the v6.3.0 / API 3 SDK
./build.sh                   # or: make
```

The toolchain step is skipped automatically if `arm-none-eabi-gcc` is already on
`PATH` (or `TOOLCHAIN_ROOT` points at one). The SDK step leaves files with the
expected hashes in place; use `--force` to download fresh copies. Output:

```
markdown_viewer/MegaMarkdown.app
```

Install that file with **UV Studio → Apps → Community → Install a .app**, choose
a free slot, then open the Apps menu on the radio (`F` then `7`), select
**MegaMarkdown** and press `MENU`.

## Layout

```
.
├── build.sh                  convenience wrapper
├── Makefile                  make / make clean / make toolchain
├── markdown files/           source documents (*.md, *.markdown)
│   └── download_armel_app_readmes.py  downloads app READMEs from the firmware repo
├── markdown_viewer/          the app itself
│   ├── markdown_viewer_app.c entry point, list, reader, Huffman decoder
│   ├── app.ld                linker script: everything in the 4 KiB overlay
│   ├── gen_assets.py         Markdown -> logical paragraphs -> compressed assets
│   ├── huffman.py            compressor and code-length builder
│   ├── build.sh              compile, link, pack
│   ├── MegaMarkdown.app      build output (gitignored)
│   └── markdown_assets.*     generated at build time (gitignored)
├── sdk/                      fetch_sdk.py + downloaded v6.3.0 SDK files (gitignored)
├── test/                     host decoder/renderer comparison
└── tools/fetch-toolchain.sh  installs ./.toolchain locally
```

## Changing the documents

Put `.md` or `.markdown` files in `markdown files/` (the folder is flat; hidden
files are ignored) and rebuild:

```sh
make clean
./build.sh
```

To probe the firmware repository and download each app's `README.md` as
`<folder>.md`, run:

```sh
python3 "markdown files/download_armel_app_readmes.py"
```

The script checks every immediate folder under `App/apps/` (not the top-level
`App/apps/README.md`), reports folders without a README, and asks before
overwriting an existing file whose contents differ.

The build prints one row per document:

```
SOURCE                     TEXT  PACKED  ROWS  TINY  NAME
about-viewer.md            1895    1130   138    87  ABOUT-VIEWER
markdown-help.md            571     358    50    40  MARKDOWN-HELP
radio-notes.md             1373     860   112    77  RADIO-NOTES

Assets: 2348 / 3840 B (61%)
```

`ROWS` is the normal 18-column view; `TINY` is the 32-column 3x5 view.

The asset area holds **3840 bytes** of compressed documents. If the total is
larger, the build stops with the exact size; remove or trim documents and
rebuild. The display name is the filename without its extension, uppercased and
limited to 16 characters (duplicates get `~2`, `~3`, ...).

Every source file is rendered at build time, not on the radio:

| Markdown | Result on the radio |
| --- | --- |
| `# Heading` | Uppercase heading, separated by blank lines |
| `- item`, `1. item` | `- item`, `1. item` |
| `> quote` | `> quote` |
| `**bold**`, `*italic*`, `` `code` `` | Markers removed |
| `[text](url)`, `![alt](url)` | Visible text only |
| fenced code block | Plain lines, hard-wrapped |
| `---` | A line of dashes |
| table rows | Cells joined with `|` |

Each paragraph is word-wrapped at runtime to 18 columns (normal font) or 32
columns (tiny font); a word longer than the width is split. Long paragraphs are
stored in chunks that fit the 255-character line buffer. Non-ASCII characters
become `?`, because the radio font covers printable ASCII.

## Controls

| Key | File list | Reader |
| --- | --- | --- |
| `UP` / `DOWN` (or `LEFT` / `RIGHT` on K1) | previous / next file | scroll one line |
| `MENU` | open the selected file | back to the file list |
| `0` | — | toggle normal (18-column) / tiny (32-column) font |
| `STAR` | first file | jump to the top |
| `F` | last file | jump to the bottom |
| `EXIT` | quit to the Apps menu | quit to the Apps menu |

Held keys repeat, except `0`, which toggles once per press. The screen saver is
enabled; any key wakes the display and redraws it.

## Compression choice

gzip/deflate compresses text slightly better, but an inflate decoder would take
a large part of the 4 KiB overlay code budget. This app uses a static order-0
Huffman code instead: one code-length table is built from all documents, the
decoder walks a short canonical code per byte, and there is no ring window in
`.bss`. On the bundled documents, 3839 bytes of logical text become 2348 bytes
of assets (61%); raw deflate would be about 2157 bytes (56%). The 191-byte
difference is not worth an inflate decoder, and the Huffman build leaves 1768
bytes of overlay headroom. Use larger source documents only if the compressed
total stays under the asset limit.

## Limits worth remembering

- **4 KiB total** for code, constants, initialised data and `.bss` (the linker
  assert enforces it). The current build uses 2328 B, including the Huffman
  tables and the runtime word-wrap buffer.
- **Assets**: at most 3840 bytes, read with `asset_read()`; this app requires
  API level 2 for assets and API level 3 for the rest of its build.
- **No firmware symbols and no C library**: only `libgcc` is linked. The app
  forwards the resident division helpers, so constant division in the status
  line is free of libgcc's larger routines.
- **Never write flash**: this app does not call any persistence service. The
  documents are read-only assets; a build update replaces them with the app.
- **No transmit path**: the app never keys the radio and changes no radio
  settings. It only turns the LED off at entry and exit.
- `APP_API_MIN=3` means the app is refused by firmware older than API level 3.
  The v6.3.0 overlay apps run on the **Labs** and **Fusion_** presets; the plain
  Fusion preset does not enable `ENABLE_FEAT_F4HWN_OVERLAY_APPS`.

## Building inside the firmware tree

Copy `markdown_viewer/` to `App/apps/markdown_viewer/`, then point
`MARKDOWN_DIR` at this repository's `markdown files/` folder and run the app's
`build.sh`:

```sh
cd App/apps/markdown_viewer
MARKDOWN_DIR=/path/to/markdown\ files \
TOOLCHAIN_ROOT=/path/to/project/.toolchain ./build.sh
```

`build.sh` and `gen_assets.py` detect the parent directory's `pack_app.py` and
`app_overlay.h` instead of `../sdk`. The firmware's `compile-app.sh` also works
if the SDK files and documents are copied alongside the app.

## Portability

- `.toolchain/` stays inside the directory on disk (gitignored), so a copy on
  the same host architecture builds without network access.
- On a different architecture, `./tools/fetch-toolchain.sh` replaces it with the
  matching ARM GNU 13.3.rel1 toolchain.
- Build outputs, generated assets and `__pycache__` are gitignored; the SDK and
  document sources are tracked.

## Verification

- The build was verified with the pinned ARM GNU 13.3.rel1 toolchain against the
  vendored v6.3.0 headers.
- `make test` runs a host-side decoder test: it decodes every generated stream
  in both the normal and tiny layouts. The normal output matches the Python
  renderer exactly (300 lines), and the tiny layout decodes 204 rows without
  errors or overlong lines.
- `make test` uses a temporary patched `app_api.h` because the SDK's 32-bit
  layout asserts do not hold on a 64-bit host. The test copies the header to a
  temporary directory and removes only those `_Static_assert` offset checks
  before compiling; the radio build always uses the unmodified SDK header.
- The blob header reports ABI 1, API minimum 3, no required capabilities, a
  valid code CRC, a valid asset CRC and `app_main` at offset 0.
- It has not been run on radio hardware. Test on a radio before publishing.

Overlay apps run privileged native code with no sandbox. Only install apps you
trust. See `LICENSE` (Apache-2.0).
