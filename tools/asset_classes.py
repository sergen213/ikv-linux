#!/usr/bin/env python3
"""Inspect original scene class dispatch; emit metadata only, never geometry."""
import argparse
import json
import mmap
from pathlib import Path
import struct
import sys
import zlib
from asset_skeleton import archive_entries, chunks


PREFIX_CLASSES = ((b"sky", "sky"), (b"ocean", "ocean"),
                  (b"isik_huzme", "isik_huzme"), (b"occ_", "occluder"))


def classify(item_type, raw_name):
    """Mirror dispatch 0x004a2e70 and case-sensitive prefix helper 0x004a0270.

    >>> [classify(1, name) for name in (b'skybox', b'oceanSky', b'isik_huzme1', b'occ_box')]
    [('sky', True), ('ocean', True), ('isik_huzme', True), ('occluder', False)]
    >>> [classify(4, name) for name in (b'Sky', b'xsky', b'sky_ocean')]
    [('mesh', True), ('mesh', True), ('sky', True)]
    >>> classify(11, b'sky')
    ('particle', None)
    >>> classify(99, b'occ_box')
    ('unsupported_99', None)
    """
    if item_type in (1, 4):
        for prefix, scene_class in PREFIX_CLASSES:
            if raw_name.startswith(prefix):
                return scene_class, scene_class != "occluder"
        return "mesh", True
    if item_type == 11:
        return "particle", None
    return f"unsupported_{item_type}", None


def item_metadata(identifier, payload):
    if len(payload) != 256:
        raise ValueError("item chunk must contain one 256-byte record")
    model_id = struct.unpack_from("<I", payload, 8)[0]
    item_type = struct.unpack_from("<I", payload, 20)[0]
    end = payload.find(b"\0", 192, 256)
    if model_id != identifier or end < 0:
        raise ValueError("invalid item identity or unterminated name")
    raw_name = payload[192:end]
    scene_class, visible_color = classify(item_type, raw_name)
    return scene_class, visible_color, {"model_id": model_id, "type": item_type,
                                        "name": raw_name.decode("latin1")}


def add_class(classes, scene_class, visible_color, example):
    entry = classes.setdefault(scene_class, {"count": 0, "visible_color": visible_color,
                                           "examples": []})
    entry["count"] += 1
    if len(entry["examples"]) < 5:
        entry["examples"].append(example)


def inspect(archive, member=None):
    reports, failures, totals = [], [], {}
    with archive.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entries = archive_entries(data)
        selected = [entry for entry in entries
                    if entry[0].lower().endswith(".adf") and (member is None or entry[0] == member)]
        if member is not None and not selected:
            raise ValueError("requested scene member not found")
        for scene, offset, size in selected:
            try:
                classes = {}
                for tag, identifier, payload in chunks(data[offset:offset + size]):
                    if tag == b"item":
                        scene_class, visible_color, example = item_metadata(identifier, payload)
                        add_class(classes, scene_class, visible_color, example)
                model_count = sum(entry["count"] for entry in classes.values())
                reports.append({"scene": scene, "model_count": model_count, "classes": classes})
                for scene_class, entry in classes.items():
                    total = totals.setdefault(scene_class, {"count": 0,
                                                           "visible_color": entry["visible_color"],
                                                           "examples": []})
                    total["count"] += entry["count"]
                    for example in entry["examples"]:
                        if len(total["examples"]) < 5:
                            total["examples"].append({"scene": scene, **example})
            except (ValueError, struct.error, zlib.error) as error:
                failures.append({"scene": scene, "error": str(error)})
    return {"schema": "ikv-original-scene-class-metadata-1", "archive": archive.name,
            "scene_count": len(reports),
            "model_count": sum(report["model_count"] for report in reports),
            "classification": {"dispatch": "0x004a2e70", "name_offset": 192,
                               "comparison": "case-sensitive raw-byte prefix (0x004a0270)",
                               "prefix_order": [prefix.decode("ascii") for prefix, _ in PREFIX_CLASSES],
                               "prefix_types": [1, 4]},
            "visible_color_semantics": "Class color-pass eligibility, not observed visibility; "
                                       "occluder render 0x00476860 is a no-op (diagnostic bounds excluded); "
                                       "null means particle or unsupported color semantics are unproven.",
            "classes": totals, "failures": failures, "reports": reports}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--member", help="exact scene member; default all scenes")
    parser.add_argument("--output", type=Path, help="metadata JSON outside original archive directory")
    args = parser.parse_args()
    if args.output and args.output.resolve().is_relative_to(args.archive.resolve().parent):
        raise ValueError("metadata output must be outside original archive directory")
    result = inspect(args.archive, args.member)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return bool(result["failures"])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
