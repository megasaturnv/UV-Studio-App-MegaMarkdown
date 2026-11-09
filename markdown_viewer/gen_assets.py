#!/usr/bin/env python3
"""Build MegaMarkdown's read-only assets.

Every *.md / *.markdown file in the source folder is converted to plain display
lines (18 columns, matching the radio's 6x8 font), compressed with the small
LZSS codec in lzss.py, and concatenated into one asset blob.  Continuation
lines carry a marker so the app can reflow the same text to the 3x5 font's 32
columns at runtime.  A generated header describes the files:

    markdown_assets.bin   compressed document streams, served by asset_read()
    markdown_assets.h     per-file offsets/sizes/line counts and LZSS constants

Markdown is rendered here, at build time, because a runtime parser and a 4 KiB
code limit do not mix.  The source files remain the editable source of truth;
rebuild after changing them.

Usage:  ./gen_assets.py <out.bin> <out.h>
"""

import os
import re
import sys
import textwrap
from pathlib import Path

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from lzss import (MIN_MATCH, LEN_BITS, OFFSET_BITS, WINDOW_BITS,
                  compress, decompress)

LINE_COLS = 18          # 128 px / (6 px glyph + 1 px gap)
NAME_MAX = 16
# The LCD has eight rows, but row 0 is the separate status line
# (gStatusLine).  gFrameBuffer holds only the seven content rows, so drawing
# at line 7 would write past the buffer and corrupt the status line.
ROWS = 7

# The 3x5 font used by print_tiny advances four pixels per glyph, so 128 px
# hold 32 columns and the 56-pixel frame buffer holds nine six-pixel rows.
TINY_COLS = 32
TINY_ROWS = 9

# The tiny view rejoins wrapped lines before reflowing them.  Keep each joined
# line below this size so the decoder's line buffer stays small.
WRAP_MAX = 255

# Continuation markers stored at the start of an 18-column line.  SPACE means
# the original text had whitespace at the break; HARD means a long word was
# split.  The normal view strips the marker, the tiny view reflows the text.
MARK_SPACE = "\x01"
MARK_HARD = "\x02"
MARK_RULE = "\x03"

DOC_SUFFIXES = {".md", ".markdown"}


def cdefine(path, name):
    pattern = re.compile(r"^\s*#define\s+" + re.escape(name) +
                         r"\s+(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b")
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = pattern.match(line)
            if match:
                return int(match.group(1), 0)
    sys.exit(f"{path}: #define {name} not found")


def docs_dir():
    candidates = []
    if os.environ.get("MARKDOWN_DIR"):
        candidates.append(os.environ["MARKDOWN_DIR"])
    candidates.append(os.path.join(ROOT, "markdown files"))
    candidates.append(os.path.join(HERE, "markdown files"))
    for path in candidates:
        if os.path.isdir(path):
            return os.path.abspath(path)
    sys.exit("markdown folder not found; looked in:\n  " +
             "\n  ".join(candidates))


def clean_inline(text):
    # Images and links: keep the visible text, drop the target.
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<((?:https?|ftp)://[^>]+)>", r"\1", text)
    # Inline code and simple emphasis markers.
    text = text.replace("`", "")
    text = re.sub(r"\*\*(.*?)\*\*", r"\1", text)
    text = re.sub(r"__(.*?)__", r"\1", text)
    text = re.sub(r"~~(.*?)~~", r"\1", text)
    text = re.sub(r"\*(.*?)\*", r"\1", text)
    text = re.sub(r"_(.*?)_", r"\1", text)
    # Any remaining HTML tag.
    text = re.sub(r"<[^>]*>", "", text)
    return re.sub(r"\s+", " ", text).strip()


def _wrap(text, cols):
    return textwrap.wrap(
        text, width=cols, break_long_words=True,
        break_on_hyphens=False, drop_whitespace=True)


def render_blocks(source):
    """Split Markdown into blank, rule and text blocks.

    Text blocks are not wrapped here; render_markdown() and render_assets()
    apply the width they need.
    """
    blocks = []
    in_code = False
    paragraph = []

    def blank():
        if blocks and blocks[-1][0] != "blank":
            blocks.append(("blank",))

    def flush():
        if paragraph:
            blocks.append(("text", " ".join(paragraph)))
            paragraph.clear()

    def text(value):
        flush()
        if value:
            blocks.append(("text", value))

    for raw in source.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.rstrip()

        fence = re.match(r"^\s*(```+|~~~+)", line)
        if fence:
            flush()
            in_code = not in_code
            blank()
            continue

        if in_code:
            content = line.strip()
            if content:
                text(content)
            continue

        if not line.strip():
            flush()
            blank()
            continue

        heading = re.match(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if heading:
            value = clean_inline(heading.group(2)).upper()
            if value:
                flush()
                blank()
                blocks.append(("text", value))
                blank()
            continue

        if re.match(r"^\s*([-*_])\s*(\1\s*){2,}$", line):
            flush()
            blank()
            blocks.append(("rule",))
            blank()
            continue

        quote = re.match(r"^\s*>\s?(.*)$", line)
        if quote:
            text("> " + clean_inline(quote.group(1)))
            continue

        bullet = re.match(r"^\s*[-*+]\s+(.*)$", line)
        if bullet:
            text("- " + clean_inline(bullet.group(1)))
            continue

        ordered = re.match(r"^\s*(\d+)[.)]\s+(.*)$", line)
        if ordered:
            text(f"{ordered.group(1)}. " + clean_inline(ordered.group(2)))
            continue

        if line.lstrip().startswith("|"):
            cells = [clean_inline(cell) for cell in line.strip().strip("|").split("|")]
            if cells and all(re.fullmatch(r":?-{2,}:?", cell or "-") for cell in cells):
                continue
            value = " | ".join(cell for cell in cells if cell)
            if value:
                text(value)
            continue

        value = clean_inline(line)
        if value:
            paragraph.append(value)

    flush()
    while blocks and blocks[-1][0] == "blank":
        blocks.pop()
    return blocks


def render_markdown(source, cols=LINE_COLS):
    """Return plain display lines wrapped to *cols* (used by the tests)."""
    lines = []
    for block in render_blocks(source):
        if block[0] == "blank":
            if lines and lines[-1] != "":
                lines.append("")
        elif block[0] == "rule":
            lines.append("-" * cols)
        else:
            lines.extend(_wrap(block[1], cols))

    while lines and lines[-1] == "":
        lines.pop()
    return lines


def _markers(text, lines):
    """Classify each break in *lines* as a whitespace or a hard-word break."""
    markers = []
    pos = 0
    for index in range(len(lines) - 1):
        found = text.find(lines[index], pos)
        if found < 0:
            # Defensive fallback; wrapped lines are substrings of text.
            markers.append(MARK_SPACE)
            continue
        pos = found + len(lines[index])
        after = pos
        while after < len(text) and text[after].isspace():
            after += 1
        markers.append(MARK_SPACE if after > pos else MARK_HARD)
        pos = after
    return markers


def render_assets(source):
    """Return 18-column lines with continuation markers for the tiny view.

    The normal reader draws these lines directly.  The tiny reader joins
    continuation lines and reflows the text to TINY_COLS.  A marker is also
    dropped when joining would exceed WRAP_MAX, bounding the decoder buffer.
    """
    stored = []
    for block in render_blocks(source):
        if block[0] == "blank":
            stored.append("")
            continue
        if block[0] == "rule":
            stored.append(MARK_RULE)
            continue

        text = block[1]
        lines = _wrap(text, LINE_COLS)
        if not lines:
            continue
        markers = _markers(text, lines)

        stored.append(lines[0])
        group_len = len(lines[0])
        for index in range(1, len(lines)):
            marker = markers[index - 1]
            extra = len(lines[index]) + (1 if marker == MARK_SPACE else 0)
            if group_len + extra > WRAP_MAX:
                # Start a new logical line: the tiny view cannot join further
                # without overflowing its wrap buffer.
                stored.append(lines[index])
                group_len = len(lines[index])
            else:
                stored.append(marker + lines[index])
                group_len += extra
    return stored


def display_name(path, used):
    base = path.stem or path.name
    # Keep the generated C string safe and the radio font happy: printable
    # ASCII only, no quote or backslash.
    base = "".join(
        ch if 32 <= ord(ch) < 127 and ch not in '"\\' else "_"
        for ch in base).upper()
    base = base[:NAME_MAX] or "DOC"
    if base not in used:
        used.add(base)
        return base
    for suffix in range(2, 100):
        tail = f"~{suffix}"
        candidate = (base[:NAME_MAX - len(tail)] + tail)[:NAME_MAX]
        if candidate not in used:
            used.add(candidate)
            return candidate
    sys.exit(f"too many documents with the name {base!r}")


def find_documents(folder):
    paths = []
    for entry in os.scandir(folder):
        if entry.is_file() and not entry.name.startswith("."):
            if os.path.splitext(entry.name)[1].lower() in DOC_SUFFIXES:
                paths.append(Path(entry.path))
    paths.sort(key=lambda path: path.name.lower())
    return paths


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_assets.py <out.bin> <out.h>")

    folder = docs_dir()
    docs = find_documents(folder)
    if not docs:
        sys.exit(f"no .md or .markdown files in {folder}")

    sdk = Path(HERE) / ".." / "sdk"
    if not (sdk / "app_overlay.h").is_file():
        sdk = Path(HERE) / ".."
    asset_max = cdefine(sdk / "app_overlay.h", "APP_ASSET_MAX")
    used_names = set()
    entries = []
    blob = bytearray()

    print(f"Markdown sources: {folder}")
    print(f"{'SOURCE':24} {'TEXT':>6} {'PACKED':>7} {'LINES':>5}  NAME")

    for doc in docs:
        with open(doc, encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        lines = render_assets(source)
        if not lines:
            print(f"  skipping empty document: {doc.name}")
            continue
        if len(lines) > 0xFFFF:
            sys.exit(f"{doc.name}: {len(lines)} display lines exceed the "
                     "16-bit line counter")
        for line in lines:
            content = line[1:] if line[:1] in (MARK_SPACE, MARK_HARD) else line
            if len(content) > LINE_COLS:
                sys.exit(f"{doc.name}: internal error, line is longer than "
                         f"{LINE_COLS} columns")

        text = ("\n".join(lines) + "\n").encode("ascii", "replace")
        if len(text) > 0xFFFF:
            sys.exit(f"{doc.name}: rendered text is {len(text)} B, over the "
                     "16-bit document length")
        packed = compress(text)
        if decompress(packed, len(text)) != text:
            sys.exit(f"{doc.name}: LZSS self-test failed")

        name = display_name(doc, used_names)
        entries.append({
            "source": doc.name,
            "name": name,
            "off": len(blob),
            "clen": len(packed),
            "ulen": len(text),
            "lines": len(lines),
        })
        blob += packed
        print(f"{doc.name:24} {len(text):6} {len(packed):7} {len(lines):5}  {name}")

    if not entries:
        sys.exit("all documents were empty")
    if len(entries) > 255:
        sys.exit(f"{len(entries)} documents, but the app's file index is 8-bit "
                 "(max 255)")

    if len(blob) > asset_max:
        print(f"\nERROR: compressed documents are {len(blob)} B, "
              f"but the asset area holds {asset_max} B.", file=sys.stderr)
        print("Remove or trim documents and rebuild.", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], "wb") as handle:
        handle.write(blob)

    guard = "MARKDOWN_ASSETS_H"
    out = [
        "/* Generated by gen_assets.py - do not edit. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        f"#define MD_ASSET_SIZE {len(blob)}u",
        f"#define MD_FILE_COUNT {len(entries)}u",
        f"#define MD_LINE_MAX {LINE_COLS}u",
        f"#define MD_ROWS {ROWS}u",
        f"#define MD_TINY_COLS {TINY_COLS}u",
        f"#define MD_TINY_ROWS {TINY_ROWS}u",
        f"#define MD_WRAP_MAX {WRAP_MAX}u",
        f"#define MD_MARK_SPACE {ord(MARK_SPACE)}u",
        f"#define MD_MARK_HARD {ord(MARK_HARD)}u",
        f"#define MD_MARK_RULE {ord(MARK_RULE)}u",
        f"#define MD_NAME_MAX {NAME_MAX}u",
        f"#define MD_LZ_WINDOW_BITS {WINDOW_BITS}u",
        f"#define MD_LZ_OFFSET_BITS {OFFSET_BITS}u",
        f"#define MD_LZ_LEN_BITS {LEN_BITS}u",
        f"#define MD_LZ_MIN_MATCH {MIN_MATCH}u",
        "",
        "typedef struct {",
        "    uint16_t off;",
        "    uint16_t clen;",
        "    uint16_t ulen;",
        "    uint16_t lines;",
        "    char name[MD_NAME_MAX + 1u];",
        "} md_file_t;",
        "",
        "static const md_file_t md_files[MD_FILE_COUNT] = {",
    ]
    for entry in entries:
        out.append(
            f"    /* {entry['source']} */ "
            f"{{ {entry['off']}u, {entry['clen']}u, {entry['ulen']}u, "
            f"{entry['lines']}u, \"{entry['name']}\" }},")
    out += [
        "};",
        "",
        f"#endif /* {guard} */",
        "",
    ]
    with open(sys.argv[2], "w", encoding="ascii") as handle:
        handle.write("\n".join(out))

    print(f"\nAssets: {len(blob)} / {asset_max} B "
          f"({len(blob) * 100 // asset_max}%)")
    print(f"Header: {sys.argv[2]}")


if __name__ == "__main__":
    main()
