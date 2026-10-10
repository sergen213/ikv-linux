#!/usr/bin/env python3
"""Read-only ADF skeleton ABI inspection; emit metadata, never asset payloads."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
import mmap
from pathlib import Path
import struct
import sys
import zlib


def archive_entries(data):
    if data[:6] != b"SKYF\x01\x00":
        raise ValueError("expected SKYF v1 archive")
    position = 6
    entries = []

    def directory(parent, depth):
        nonlocal position
        if depth > 64 or position + 68 > min(len(data), 64 << 20):
            raise ValueError("invalid SKYF directory bounds")
        raw = data[position:position + 64]
        children, files = struct.unpack_from("<HH", data, position + 64)
        position += 68
        component = name(raw)
        path = parent + "/" + component if parent else component
        for _ in range(files):
            if position + 136 > min(len(data), 64 << 20):
                raise ValueError("truncated SKYF file table")
            filename = name(data[position:position + 128])
            offset, size = struct.unpack_from("<II", data, position + 128)
            position += 136
            if offset + size > len(data):
                raise ValueError("SKYF member outside archive")
            entries.append((path + "/" + filename, offset, size))
        for _ in range(children):
            directory(path, depth + 1)

    directory("", 1)
    expected = position
    seen = set()
    for path, offset, size in entries:
        if path in seen or offset != expected:
            raise ValueError("duplicate path or noncontiguous SKYF payload")
        seen.add(path)
        expected += size
    if expected != len(data):
        raise ValueError("SKYF payload does not end at EOF")
    return entries


def name(raw):
    end = raw.find(b"\0")
    if end <= 0 or any(raw[end:]):
        raise ValueError("invalid padded SKYF name")
    result = raw[:end].decode("latin1")
    if result in (".", "..") or any(ord(c) < 32 or c in "/\\" for c in result):
        raise ValueError("unsafe SKYF component")
    return result


def arithmetic_decode(data, expected):
    """Original 0x0040fcc0 adaptive 16-bit arithmetic decoder."""
    frequencies = [0] + [1] * 257
    cumulative = list(range(257, -1, -1))
    symbols = [0] + list(range(256)) + [0]
    bit_position = 0
    # Original readers can consume zero padding after the final stored byte.
    def bit():
        nonlocal bit_position
        if bit_position >= len(data) * 8 + 16:
            raise ValueError("truncated arithmetic stream")
        value = (data[bit_position // 8] >> (bit_position % 8)) & 1 if bit_position < len(data) * 8 else 0
        bit_position += 1
        return value

    low, high, code = 0, 65535, 0
    for _ in range(16):
        code = code * 2 + bit()
    output = bytearray()
    while True:
        width = high - low + 1
        scaled = ((code - low + 1) * cumulative[0] - 1) // width
        symbol = 1
        while symbol <= 257 and scaled < cumulative[symbol]:
            symbol += 1
        if symbol > 257 or scaled >= cumulative[0]:
            raise ValueError("invalid arithmetic symbol interval")
        if symbol == 257:
            if len(output) != expected:
                raise ValueError("arithmetic stream decoded size mismatch")
            return bytes(output)
        output.append(symbols[symbol])
        if len(output) == expected:
            return bytes(output)
        high = low + cumulative[symbol - 1] * width // cumulative[0] - 1
        low += cumulative[symbol] * width // cumulative[0]
        while True:
            if high < 32768:
                pass
            elif low >= 32768:
                code -= 32768
                low -= 32768
                high -= 32768
            elif low >= 16384 and high < 49152:
                code -= 16384
                low -= 16384
                high -= 16384
            else:
                break
            low *= 2
            high = high * 2 + 1
            code = code * 2 + bit()
        if cumulative[0] == 16383:
            total = 0
            for index in range(257, -1, -1):
                cumulative[index] = total
                frequencies[index] = (frequencies[index] + 1) // 2
                total += frequencies[index]
        index = symbol
        while frequencies[index] == frequencies[index - 1]:
            index -= 1
        symbols[index], symbols[symbol] = symbols[symbol], symbols[index]
        frequencies[index] += 1
        for prior in range(index):
            cumulative[prior] += 1


def chunks(data):
    if len(data) < 24 or data[:4] != b"root":
        raise ValueError("expected ADF root")
    count, stride, codec = struct.unpack_from("<III", data, 4)
    if stride != 32 or count > (len(data) - 24) // stride:
        raise ValueError("invalid ADF directory")
    for index in range(count):
        tag, identifier, size, decoded, compressed, offset, _, _ = struct.unpack_from(
            "<4s7I", data, 24 + index * 32)
        if offset < 24 + count * 32 or offset + size > len(data) or decoded > 128 << 20:
            raise ValueError("invalid ADF chunk bounds")
        payload = data[offset:offset + size]
        if compressed:
            if codec == 255:
                inflater = zlib.decompressobj()
                payload = inflater.decompress(payload, decoded + 1)
                if not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
                    raise ValueError("invalid bounded zlib stream")
            else:
                payload = arithmetic_decode(payload, decoded)
        if len(payload) != decoded:
            raise ValueError("ADF decoded size mismatch")
        yield tag, identifier, payload


def finite(values):
    if not all(math.isfinite(value) for value in values):
        raise ValueError("nonfinite skeletal value")


def inspect_member(member, data):
    counts = Counter()
    bones = {}
    tracks = {}
    weights = defaultdict(float)
    influence_count = 0
    weight_min, weight_max = math.inf, -math.inf
    time_min, time_max = math.inf, -math.inf
    owner_vertices = {}
    influences = {}
    for tag, identifier, payload in chunks(data):
        counts[tag.decode("ascii")] += 1
        stride = {b"bone": 256, b"wmap": 32, b"qkey": 64, b"mver": 16, b"ptri": 116}.get(tag)
        if stride and len(payload) % stride:
            raise ValueError(f"{tag!r} record stride mismatch")
        if tag == b"bone":
            if len(payload) != 256:
                raise ValueError("bone chunk must contain one record")
            own, owner, parent = struct.unpack_from("<III", payload, 8)
            if own != identifier or identifier in bones or b"\0" not in payload[192:256]:
                raise ValueError("invalid bone identity or name")
            bones[identifier] = (owner, parent)
        elif tag in (b"mver", b"vert"):
            owner_vertices[identifier] = len(payload) // (16 if tag == b"mver" else 48)
        elif tag == b"wmap":
            records = []
            for offset in range(0, len(payload), 32):
                vertex, weight, *vectors = struct.unpack_from("<If6f", payload, offset)
                finite((weight, *vectors))
                if not 0 <= weight <= 1:
                    raise ValueError("skin weight outside [0,1]")
                records.append((vertex, weight))
            influences[identifier] = records  # Original loader replaces, rather than appends, bone maps.
        elif tag == b"qkey":
            previous = -math.inf
            for offset in range(0, len(payload), 64):
                position = struct.unpack_from("<3f", payload, offset)
                rotation = struct.unpack_from("<4f", payload, offset + 32)
                time = struct.unpack_from("<f", payload, offset + 48)[0]
                finite((*position, *rotation, time))
                if time < previous or sum(value * value for value in rotation) < 1e-12:
                    raise ValueError("invalid animation key order or quaternion")
                previous = time
                time_min, time_max = min(time_min, time), max(time_max, time)
            tracks[identifier] = tracks.get(identifier, 0) + len(payload) // 64
    for identifier, records in influences.items():
        if identifier not in bones:
            raise ValueError("wmap has no matching bone")
        owner = bones[identifier][0]
        for vertex, weight in records:
            influence_count += 1
            weight_min, weight_max = min(weight_min, weight), max(weight_max, weight)
            if owner not in owner_vertices or vertex >= owner_vertices[owner]:
                raise ValueError("wmap vertex outside owner mesh")
            weights[(owner, vertex)] += weight
    return {"member": member, "chunk_counts": dict(sorted(counts.items())),
            "bones": len(bones), "self_parent_roots": sum(parent == identifier for identifier, (_, parent) in bones.items()),
            "skin_influences": influence_count, "weighted_vertices": len(weights),
            "weight_range": [weight_min, weight_max] if influence_count else None,
            "weight_sum_range": [min(weights.values()), max(weights.values())] if weights else None,
            "nonunit_weight_sums": sum(abs(value - 1) > 1e-4 for value in weights.values()),
            "tracks": len(tracks), "animated_tracks": sum(count > 1 for count in tracks.values()),
            "key_count": sum(tracks.values()),
            "key_time_seconds_range": [time_min, time_max] if tracks else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--member", help="inspect exact member, default all ADF members")
    parser.add_argument("--output", type=Path, help="metadata JSON path, default stdout")
    args = parser.parse_args()
    reports, failures = [], []
    with args.archive.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entries = archive_entries(data)
        archive_hash = hashlib.sha256(data).hexdigest()
        selected = [entry for entry in entries if entry[0] == args.member] if args.member else entries
        if not selected:
            raise ValueError("requested member not found")
        for member, offset, size in selected:
            if not member.lower().endswith(".adf"):
                continue
            try:
                reports.append(inspect_member(member, data[offset:offset + size]))
            except (ValueError, struct.error, zlib.error) as error:
                failures.append({"member": member, "error": str(error)})
    result = {"schema": "ikv-skeleton-corpus-metadata-1", "archive": args.archive.name,
              "sha256": archive_hash, "members": len(entries), "inspected": len(reports),
              "failures": failures, "reports": reports}
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        if args.output.resolve().is_relative_to(args.archive.resolve().parent):
            raise ValueError("metadata output must be outside the original archive directory")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
