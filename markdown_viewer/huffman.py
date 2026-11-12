#!/usr/bin/env python3
"""Static order-0 Huffman codec for MegaMarkdown's document assets.

The code-length table is global to all documents.  gen_assets.py writes the
decoder tables into markdown_assets.h; each document is stored as one raw
MSB-first bitstream.  A document's uncompressed length in md_file_t tells the
decoder where to stop.
"""

import heapq

MAX_BITS = 12


def _lengths(data):
    freq = {}
    for value in data:
        freq[value] = freq.get(value, 0) + 1
    if len(freq) == 1:
        return {next(iter(freq)): 1}

    heap = []
    order = 0
    for symbol, weight in sorted(freq.items()):
        heap.append((weight, order, {symbol: 0}))
        order += 1
    heapq.heapify(heap)

    while len(heap) > 1:
        weight_a, _, codes_a = heapq.heappop(heap)
        weight_b, _, codes_b = heapq.heappop(heap)
        merged = {symbol: length + 1 for symbol, length in codes_a.items()}
        merged.update({symbol: length + 1
                       for symbol, length in codes_b.items()})
        heapq.heappush(heap, (weight_a + weight_b, order, merged))
        order += 1

    return heap[0][2]


def _limit_lengths(lengths, freq):
    """Cap code lengths while keeping the prefix code valid (Kraft <= 1)."""
    lengths = {symbol: min(length, MAX_BITS)
               for symbol, length in lengths.items()}
    total = 1 << MAX_BITS
    used = sum(1 << (MAX_BITS - length) for length in lengths.values())

    candidates = [(freq[symbol], symbol) for symbol in lengths
                  if lengths[symbol] < MAX_BITS]
    heapq.heapify(candidates)
    while used > total:
        if not candidates:
            raise ValueError("cannot limit Huffman code lengths")
        _, symbol = heapq.heappop(candidates)
        length = lengths[symbol]
        used -= 1 << (MAX_BITS - length - 1)
        lengths[symbol] = length + 1
        if length + 1 < MAX_BITS:
            heapq.heappush(candidates, (freq[symbol], symbol))
    return lengths


def build_table(streams):
    data = b''.join(streams)
    freq = {}
    for value in data:
        freq[value] = freq.get(value, 0) + 1
    lengths = _limit_lengths(_lengths(data), freq)

    symbols = sorted(lengths, key=lambda symbol: (lengths[symbol], symbol))
    first_code = [0] * (MAX_BITS + 1)
    first_index = [0] * (MAX_BITS + 1)
    count = [0] * (MAX_BITS + 1)
    for symbol in symbols:
        count[lengths[symbol]] += 1

    code = 0
    index = 0
    for length in range(1, MAX_BITS + 1):
        first_code[length] = code
        first_index[length] = index
        code = (code + count[length]) << 1
        index += count[length]

    codes = {}
    next_code = first_code[:]
    for symbol in symbols:
        length = lengths[symbol]
        codes[symbol] = next_code[length]
        next_code[length] += 1

    max_bits = max(lengths.values())
    return {
        "max_bits": max_bits,
        "symbols": symbols,
        "first_code": first_code[:max_bits + 1],
        "first_index": first_index[:max_bits + 1],
        "count": count[:max_bits + 1],
        "codes": codes,
        "lengths": lengths,
    }


def compress(data, table):
    codes = table["codes"]
    out = bytearray()
    acc = 0
    bits = 0
    for value in data:
        code = codes[value]
        length = table["lengths"][value]
        for shift in range(length - 1, -1, -1):
            acc = (acc << 1) | ((code >> shift) & 1)
            bits += 1
            if bits == 8:
                out.append(acc)
                acc = 0
                bits = 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def decompress(data, size, table):
    symbols = table["symbols"]
    first_code = table["first_code"]
    first_index = table["first_index"]
    count = table["count"]
    out = bytearray()
    acc = 0
    bits = 0
    pos = 0
    while len(out) < size:
        code = 0
        for length in range(1, table["max_bits"] + 1):
            if not bits:
                if pos >= len(data):
                    raise ValueError("truncated Huffman stream")
                acc = data[pos]
                pos += 1
                bits = 8
            code = (code << 1) | ((acc >> 7) & 1)
            acc = (acc << 1) & 0xFF
            bits -= 1
            index = code - first_code[length]
            if index < count[length]:
                out.append(symbols[first_index[length] + index])
                break
        else:
            raise ValueError("invalid Huffman code")
    return bytes(out)
