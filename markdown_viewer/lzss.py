#!/usr/bin/env python3
"""Small LZSS compressor for MegaMarkdown's document assets.

The radio app has two hard limits:

  * 4 KiB of overlay RAM for code, constants, data *and* .bss;
  * 3840 B of read-only assets in the .app header sector.

gzip/deflate compresses text better, but an inflate decoder costs well over a
kilobyte of the 4 KiB code budget.  This LZSS variant needs only a small ring
buffer and a short decoder, so more of the budget is left for documents.

Format (all values little-endian):

    flag byte, then eight tokens (LSB first, 1 = literal, 0 = match)
    literal:  one byte
    match:    two bytes: (distance - 1) | ((length - MIN_MATCH) << OFFSET_BITS)

The generated markdown_assets.h carries the same constants, so the C decoder
cannot drift from this compressor.
"""

WINDOW_BITS = 10
WINDOW_SIZE = 1 << WINDOW_BITS
OFFSET_BITS = 10
LEN_BITS = 6
MIN_MATCH = 3
MAX_MATCH = MIN_MATCH + (1 << LEN_BITS) - 1
CHAIN_LIMIT = 96
MAX_TOKEN = (1 << (OFFSET_BITS + LEN_BITS)) - 1


def _add(data, pos, head, prev):
    if pos + MIN_MATCH <= len(data):
        key = data[pos:pos + MIN_MATCH]
        prev[pos & (WINDOW_SIZE - 1)] = head.get(key, -1)
        head[key] = pos


def _find(data, pos, head, prev):
    """Longest match for data[pos:] already present in the window."""
    if pos + MIN_MATCH > len(data):
        return 0, 0

    best_len = 0
    best_off = 0
    start = pos - WINDOW_SIZE
    limit = min(MAX_MATCH, len(data) - pos)
    cand = head.get(data[pos:pos + MIN_MATCH], -1)

    steps = 0
    while cand >= start and cand >= 0 and steps < CHAIN_LIMIT:
        length = 0
        while length < limit and data[cand + length] == data[pos + length]:
            length += 1
        if length > best_len:
            best_len = length
            best_off = pos - cand
            if length == limit:
                break
        cand = prev[cand & (WINDOW_SIZE - 1)]
        steps += 1

    if best_len < MIN_MATCH:
        return 0, 0
    return best_len, best_off


def compress(data):
    """Compress bytes with the format described above."""
    data = bytes(data)
    n = len(data)
    head = {}
    prev = [-1] * WINDOW_SIZE
    out = bytearray()
    flags_pos = 0
    flags_left = 0

    def put_literal(value):
        nonlocal flags_pos, flags_left
        if flags_left == 0:
            flags_pos = len(out)
            out.append(0)
            flags_left = 8
        out[flags_pos] |= 1 << (8 - flags_left)
        flags_left -= 1
        out.append(value)

    def put_match(distance, length):
        nonlocal flags_pos, flags_left
        if flags_left == 0:
            flags_pos = len(out)
            out.append(0)
            flags_left = 8
        # The flag byte's zero bit means "match".
        flags_left -= 1
        token = (distance - 1) | ((length - MIN_MATCH) << OFFSET_BITS)
        assert token <= MAX_TOKEN
        out.append(token & 0xFF)
        out.append((token >> 8) & 0xFF)

    i = 0
    while i < n:
        best_len, best_off = _find(data, i, head, prev)

        if best_len:
            # One-byte lazy lookahead: take the longer match at i+1 when it is
            # clearly better, otherwise stay with the match at i.
            _add(data, i, head, prev)
            next_len = 0
            if i + 1 < n:
                next_len, _ = _find(data, i + 1, head, prev)
            if next_len > best_len + 1:
                put_literal(data[i])
                i += 1
                continue

            put_match(best_off, best_len)
            for j in range(1, best_len):
                _add(data, i + j, head, prev)
            i += best_len
        else:
            put_literal(data[i])
            _add(data, i, head, prev)
            i += 1

    return bytes(out)


def decompress(blob, out_len):
    """Reference decoder, used by the build's self-test."""
    out = bytearray()
    pos = 0
    flags = 0
    flags_left = 0
    while len(out) < out_len:
        if flags_left == 0:
            flags = blob[pos]
            pos += 1
            flags_left = 8
        if flags & 1:
            out.append(blob[pos])
            pos += 1
        else:
            token = blob[pos] | (blob[pos + 1] << 8)
            pos += 2
            distance = (token & ((1 << OFFSET_BITS) - 1)) + 1
            length = (token >> OFFSET_BITS) + MIN_MATCH
            for _ in range(length):
                out.append(out[-distance])
        flags >>= 1
        flags_left -= 1
    return bytes(out[:out_len])


if __name__ == "__main__":
    import sys

    raw = sys.stdin.buffer.read()
    packed = compress(raw)
    assert decompress(packed, len(raw)) == raw
    sys.stderr.write(f"{len(raw)} -> {len(packed)} B "
                     f"({len(packed) * 100 / max(1, len(raw)):.1f}%)\n")
    sys.stdout.buffer.write(packed)
