#!/usr/bin/env python3
"""Decode original support/height grids; emit bounded metadata, never asset payloads."""
import argparse
from array import array
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys

WIDTH = HEIGHT = 2048
COUNT = WIDTH * HEIGHT
PITCH = 1.5
OFFSET = 1024
WORLD_TO_GRID = struct.unpack("<f", bytes.fromhex("abaa2a3f"))[0]
BINARY_SHA256 = "44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d"


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def round_original(value):
    """00444af0: ceil(value-.5) below zero, floor(value+.5) otherwise."""
    return math.ceil(value - 0.5) if value < 0 else math.floor(value + 0.5)


def world_index(x, z):
    """Bounded runtime conversion at 00477b85..00477be2, including SSE precision."""
    indices = []
    for value in (x, z):
        if not math.isfinite(value):
            return None
        try:
            scaled = f32(f32(value) * WORLD_TO_GRID)
            shifted = f32(scaled + OFFSET)
        except (OverflowError, struct.error):
            return None
        if not math.isfinite(shifted):
            return None
        indices.append(round_original(shifted))
    ix, iz = indices
    if not (0 <= ix < WIDTH and 0 <= iz < HEIGHT):
        return None
    return iz * WIDTH + ix


@dataclass
class ZoneGrid:
    cells: bytes
    heights: array

    def height_at_world(self, x, z):
        """None means outside or no accepted cached support, not an invented wall."""
        index = world_index(x, z)
        if index is None or self.cells[index] != 255:
            return None
        return self.heights[index]


def read_stable(path, expected):
    if path.is_symlink():
        raise ValueError(f"refusing symbolic link: {path}")
    before = path.stat()
    if before.st_size != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {before.st_size}")
    data = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (
        after.st_size, after.st_mtime_ns, after.st_ino
    ) or len(data) != expected:
        raise ValueError(f"file changed during read: {path}")
    return data


def decode_zone(cell_path, height_path):
    cells = read_stable(Path(cell_path), COUNT)
    raw_heights = read_stable(Path(height_path), COUNT * 4)
    if set(cells) - {0, 255}:
        raise ValueError(f"{cell_path}: unsupported cell value (original generator emits 0/255)")
    heights = array("f")
    if heights.itemsize != 4:
        raise ValueError("platform does not provide 32-bit array floats")
    heights.frombytes(raw_heights)
    if sys.byteorder != "little":
        heights.byteswap()
    if any(not math.isfinite(value) for value in heights):
        raise ValueError(f"{height_path}: non-finite height")
    return ZoneGrid(cells, heights), hashlib.sha256(cells).hexdigest(), hashlib.sha256(raw_heights).hexdigest()


def generator_corners(ix, iz):
    """Exact asymmetric rounding of four rays in 00402310, not a guessed quad."""
    x, z = ix - OFFSET, iz - OFFSET
    left = round_original(x * PITCH)
    right = round_original((x + 0.5) * PITCH)
    near = round_original(z) * PITCH
    far = round_original(z + 0.5) * PITCH
    return [[left, near], [right, near], [right, far], [left, far]]


def zone_metadata(grid, zone_id, root, cell_path, height_path, cell_hash, height_hash):
    histogram = Counter(grid.cells)
    first = last = minimum = maximum = None
    inactive_nonzero = active_zero = 0
    min_x = min_z = WIDTH
    max_x = max_z = -1
    for index, value in enumerate(grid.heights):
        if grid.cells[index] != 255:
            inactive_nonzero += value != 0
            continue
        active_zero += value == 0
        if first is None:
            first = minimum = maximum = index
        last = index
        if value < grid.heights[minimum]:
            minimum = index
        if value > grid.heights[maximum]:
            maximum = index
        iz, ix = divmod(index, WIDTH)
        min_x, max_x = min(min_x, ix), max(max_x, ix)
        min_z, max_z = min(min_z, iz), max(max_z, iz)
    samples = []
    for label, index in (("first_supported", first), ("last_supported", last),
                         ("minimum_height", minimum), ("maximum_height", maximum)):
        if index is None:
            continue
        iz, ix = divmod(index, WIDTH)
        samples.append({"kind": label, "index": index, "grid_x": ix, "grid_z": iz,
                        "nominal_world_xz": [(ix - OFFSET) * PITCH, (iz - OFFSET) * PITCH],
                        "generator_ray_xz": generator_corners(ix, iz),
                        "cell": grid.cells[index], "height_y": grid.heights[index]})
    return {"zone_id": zone_id,
            "cells": {"path": cell_path.relative_to(root).as_posix(), "bytes": COUNT,
                      "sha256": cell_hash, "histogram": {str(k): v for k, v in sorted(histogram.items())}},
            "heights": {"path": height_path.relative_to(root).as_posix(), "bytes": COUNT * 4,
                        "sha256": height_hash, "finite_count": COUNT,
                        "supported_min_y": None if minimum is None else grid.heights[minimum],
                        "supported_max_y": None if maximum is None else grid.heights[maximum],
                        "unsupported_nonzero_count": inactive_nonzero,
                        "supported_zero_count": active_zero},
            "supported_cells": histogram[255],
            "supported_grid_bounds_inclusive": None if first is None else [min_x, min_z, max_x, max_z],
            "samples": samples}


EVIDENCE = [
    {"claim": "Headerless byte and four-byte height buffers; selected filename uses zone-ID table.",
     "source": "analysis/zone-game/decompile/00402140.c", "lines": [25, 64],
     "addresses": ["00402140", "0040220e", "004022b4"]},
    {"claim": "Two signed loops [-1024,1023], x-fastest; consecutive byte and float stores.",
     "source": "analysis/zone-game/decompile/00402310.c", "lines": [66, 131],
     "addresses": ["004023a2", "004023e6", "0040266e", "00402673", "0040267b", "00402698"]},
    {"claim": "Four downward scene queries; 255/all accepted or 0/any failed; stored height is fourth hit Y.",
     "source": "analysis/zone-game/decompile/00402310.c", "lines": [71, 124],
     "addresses": ["00402488", "0040251e", "004025a5", "0040262a", "0040264a", "00402653", "00402656", "00402667", "0040266a"]},
    {"claim": "Float32 world-to-grid multiplier, center offset, pitch and ray constants read from executable memory.",
     "source": "analysis/zone-index/trace.tsv", "lines": [1, 18],
     "addresses": ["0058d35c", "00592ee8", "0058d374", "0058d3b8", "0058d360", "0058d368"]},
    {"claim": "Primary runtime rounds after float32 scale and +1024; bounds each axis; accepts byte255, reads Y float.",
     "source": "analysis/zone-index/trace.tsv", "lines": [453, 482],
     "addresses": ["00477b85", "00477b8d", "00477b9b", "00477bae", "00477bbd", "00477bcd", "00477bdd", "00477bf1", "00477bfc"]},
    {"claim": "Signed nearest integer rounding with ties away from zero.",
     "source": "analysis/zone-index/00444af0.c", "lines": [2, 12],
     "addresses": ["00444af0", "00444b0b", "00444b3a"]},
    {"claim": "Negative rounding branch calls ceil(value-.5).", "source": "analysis/zone-index/0056e040.c",
     "lines": [17, 36], "addresses": ["0056e040"]},
    {"claim": "Nonnegative rounding branch calls floor(value+.5).", "source": "analysis/zone-index/005702e0.c",
     "lines": [17, 37], "addresses": ["005702e0"]},
    {"claim": "Separate helper rounds before centered offset; linear-only bounds and final-cell fallback are original quirks.",
     "source": "analysis/zone-index/00401060.c", "lines": [9, 15],
     "addresses": ["00401060", "0040109a", "004010bb"]},
    {"claim": "Direct byte helper also rounds before offset; 0x200400 = 1024*2048+1024.",
     "source": "analysis/zone-index/004b37a0.c", "lines": [8, 10], "addresses": ["004b37a0"]},
    {"claim": "First scene-object list hit rejects support; second list hit returns XYZ and normal.",
     "source": "analysis/zone-index/00446cc0.c", "lines": [8, 36], "addresses": ["00446cc0", "00446ce3", "00446d1c"]},
    {"claim": "Ray setter negates input direction; generator passes (0,1,0), producing (0,-1,0).",
     "source": "analysis/zone-index/00446a50.c", "lines": [2, 6], "addresses": ["00446a50"]},
    {"claim": "Original loader callsites.", "source": "analysis/zone-game/functions.tsv", "lines": [2, 3],
     "addresses": ["0041cba8", "004b2e8f", "004b29bb"]}
]


def inventory(root):
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("installation root must be a directory")
    base = root / "base"
    cell_paths = sorted(base.glob("zone[0-9][0-9].bin"))
    if not cell_paths:
        raise ValueError(f"no original zoneNN.bin grids in {base}")
    expected_heights = {p.with_name(p.stem + "_height.bin") for p in cell_paths}
    actual_heights = set(base.glob("zone[0-9][0-9]_height.bin"))
    if expected_heights != actual_heights:
        raise ValueError("zone byte/height pairs are incomplete")
    zones = []
    for cell_path in cell_paths:
        zone_id = int(re.fullmatch(r"zone([0-9]{2})\.bin", cell_path.name)[1])
        height_path = cell_path.with_name(cell_path.stem + "_height.bin")
        grid, cell_hash, height_hash = decode_zone(cell_path, height_path)
        zones.append(zone_metadata(grid, zone_id, root, cell_path, height_path, cell_hash, height_hash))
    return {"schema": "ikv-original-zone-decoder-1", "root": root.name,
            "evidence_kind": "static original executable plus full raw-grid corpus scan",
            "original_dynamic_confirmation": False,
            "original_executable_sha256": BINARY_SHA256,
            "format": {"width": WIDTH, "height": HEIGHT, "index": "z*2048+x",
                       "cell_encoding": "uint8 support mask", "height_encoding": "IEEE754 float32 little-endian",
                       "header_bytes": 0, "height_scale": 1.0, "vertical_axis": "Y",
                       "pitch_world_units": PITCH, "nominal_origin_xz": [-1536.0, -1536.0],
                       "world_zero_grid_xz": [OFFSET, OFFSET],
                       "world_to_grid_multiplier_float32": WORLD_TO_GRID,
                       "world_to_grid_multiplier_bits_hex": "3f2aaaab",
                       "runtime_index": "roundAway(f32(f32(world_xz)*f32(2/3))+1024), each axis bounds [0,2048)",
                       "cell_255": "all four downward rays accepted; cached support height usable",
                       "cell_0": "at least one support query failed or hit rejection list; no cached support",
                       "height_source": "fourth ray hit Y (not mean, bilinear sample or vertex height)",
                       "ray_origin_y": 2500.0, "ray_direction_xyz": [0.0, -1.0, 0.0],
                       "generator_corners": "x0=roundAway(sx*1.5), x1=roundAway((sx+.5)*1.5), z0=roundAway(sz)*1.5, z1=roundAway(sz+.5)*1.5; order (x0,z0),(x1,z0),(x1,z1),(x0,z1)",
                       "alternate_helper_index": "(roundAway(z*f32(2/3))+1024)*2048+roundAway(x*f32(2/3))+1024",
                       "alternate_helper_difference": "negative half ties differ from primary runtime; original linear-only bound/final-cell fallback not used by safe decoder"},
            "limitations": ["Support cache is not complete collision geometry or a proven walkability map.",
                            "Physical meter scale and scene-object-list category names are not recovered.",
                            "No interpolation: original stored Y is fourth accepted ray hit; scene triangles remain separate.",
                            "Primary runtime absent support uses a separate scene query; decoder returns None instead of inventing its result."],
            "source_evidence": EVIDENCE, "zone_count": len(zones), "zones": zones}


def self_check():
    assert [round_original(v) for v in (-1.5, -0.5, 0.5, 1.5)] == [-2, -1, 1, 2]
    assert world_index(0, 0) == 1024 * WIDTH + 1024
    assert world_index(-1536, -1536) == 0
    assert world_index(1534.5, 1534.5) == COUNT - 1
    assert world_index(1536, 0) is None
    assert world_index(0, float("nan")) is None
    assert world_index(1e300, 0) is None
    assert world_index(-0.75, 0) == 1024 * WIDTH + 1024
    assert round_original(f32(f32(-0.75) * WORLD_TO_GRID)) + OFFSET == 1023
    assert generator_corners(1024, 1024) == [[0, 0.0], [1, 0.0], [1, 1.5], [0, 1.5]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", type=Path, help="original installation root containing base/")
    parser.add_argument("--output", type=Path, help="metadata JSON outside original installation; default stdout")
    parser.add_argument("--self-check", action="store_true", help="run bounded coordinate assertions without native binaries")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        if args.root is None:
            print("zone coordinate self-check passed")
            return
    if args.root is None:
        parser.error("root is required unless using --self-check")
    root = args.root.resolve(strict=True)
    if args.output and args.output.resolve().is_relative_to(root):
        parser.error("output must be outside the original installation")
    result = inventory(root)
    text = json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(f"{args.output}: metadata for {result['zone_count']} original zone pairs")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, OverflowError) as error:
        sys.exit(str(error))
