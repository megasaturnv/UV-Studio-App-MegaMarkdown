# About This Viewer

**MegaMarkdown** is an overlay app for firmware v6.3, API level 3. It stores documents in the app's read-only flash assets, so they survive a firmware update of the main radio unless the app slot is erased.

## Build pipeline

1. Read every `.md` file in the source folder.
2. Split the text into paragraphs that fit the runtime line buffer.
3. Compress each document with static Huffman.
4. Pack the streams into the `.app` asset area.

The radio only decompresses the paragraph it needs and wraps it at runtime. There is no runtime Markdown parser, which keeps the code inside the 4 KiB overlay budget.

## Controls

- `UP` / `DOWN` move through the file list.
- `MENU` opens the selected file.
- `UP` / `DOWN` scroll one line in the reader.
- `MENU` returns to the file list.
- `STAR` jumps to the top, `F` to the bottom.
- `0` toggles the small 32-column font.
- `EXIT` quits to the Apps menu.

On a UV-K1, use `LEFT` / `RIGHT` where this text says `UP` / `DOWN`.

## Limits

The radio screen shows seven lines of eighteen characters below the status line. Each paragraph is word-wrapped at runtime; a paragraph is stored in chunks that fit the 255-character line buffer.

Press `0` in the reader to switch to the small 3x5 font. It shows nine lines of 32 characters. The status line ends with `N` for normal or `S` for small.

The total compressed size of every document must fit in 3840 bytes. The build prints the size of each file and refuses to continue if the assets overflow.

## Why Huffman?

gzip compresses text a little better, but an inflate decoder would take a large part of the 4 KiB code budget. A static Huffman decoder is small and needs no ring buffer, leaving room for the viewer and more documents.

## Source files

The documents in this folder are the source of truth. Edit them, then rebuild the app to refresh the assets. A document that is too large makes the build fail with the exact compressed size.
