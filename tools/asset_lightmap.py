#!/usr/bin/env python3
"""Inspect original per-model lightmap bindings and DDS headers; emit metadata only."""
import argparse
import json
import mmap
from pathlib import Path
import struct
import sys
import zlib
from asset_skeleton import archive_entries, chunks


def binding(identifier, payload):
    if len(payload) != 76 or struct.unpack_from("<I", payload)[0] != identifier:
        raise ValueError("invalid lmpd size or model identity")
    end = payload.find(b"\0", 4)
    if end < 4:
        raise ValueError("unterminated lmpd filename")
    filename = payload[4:end].decode("latin1")
    if not filename or "/" in filename or "\\" in filename or filename in (".", ".."):
        raise ValueError("unsafe lmpd filename")
    # Post-NUL bytes are not read by 0x004a1930; never interpret them as UV transforms.
    return {"model_id": identifier, "filename": filename, "filename_bytes": end - 4}


def texture_index(archive):
    result = {}
    with archive.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        for path, offset, size in archive_entries(data):
            item = {"member": path, "bytes": size}
            if path.lower().endswith(".dds"):
                if size < 128 or data[offset:offset + 4] != b"DDS ":
                    raise ValueError(f"invalid DDS header: {path}")
                height, width = struct.unpack_from("<II", data, offset + 12)
                item.update(width=width, height=height,
                            format=data[offset + 84:offset + 88].decode("latin1"))
            key = path.casefold()
            if key in result:
                raise ValueError("ambiguous case-insensitive lightmap member")
            result[key] = item
    return result


def inspect(archive, member=None, lightmap_archives=()):
    textures = {path.name: texture_index(path) for path in lightmap_archives}
    reports, failures = [], []
    with archive.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        entries = archive_entries(data)
        selected = [entry for entry in entries if entry[0] == member] if member else entries
        if not selected:
            raise ValueError("requested scene member not found")
        for scene, offset, size in selected:
            if not scene.lower().endswith(".adf"):
                continue
            try:
                model_ids, bindings = set(), []
                for tag, identifier, payload in chunks(data[offset:offset + size]):
                    if tag == b"item":
                        model_ids.add(identifier)
                    elif tag == b"lmpd":
                        bindings.append(binding(identifier, payload))
                if not bindings:
                    continue
                zone = Path(scene).stem
                for item in bindings:
                    if item["model_id"] not in model_ids:
                        raise ValueError("lmpd target model not present")
                    filename = item["filename"]
                    # Original zone==-1 uses root; otherwise uses current zone name.
                    # Report both explicitly rather than inventing a root fallback for missing zones.
                    item["zone_path"] = f"lightmaps/{zone}/{filename}"
                    item["root_path"] = f"lightmaps/{filename}"
                    item["textures"] = {}
                    for name, index in textures.items():
                        matches = {}
                        for mode in ("zone", "root"):
                            candidate = item[mode + "_path"]
                            actual = index.get(candidate.casefold())
                            if actual is None and len(filename) > 3:
                                actual = index.get((candidate[:-3] + "jpg").casefold())
                            if actual is not None:
                                matches[mode] = actual
                        item["textures"][name] = matches
                reports.append({"scene": scene, "bindings": bindings})
            except (ValueError, struct.error, zlib.error) as error:
                failures.append({"scene": scene, "error": str(error)})
    bindings = [item for report in reports for item in report["bindings"]]
    return {"schema": "ikv-original-lightmap-metadata-1", "archive": archive.name,
            "scene_count": len(reports), "binding_count": len(bindings),
            "unique_filenames": len({item["filename"] for item in bindings}),
            "max_filename_bytes": max((item["filename_bytes"] for item in bindings), default=0),
            "texture_archive_entries": {name: len(index) for name, index in textures.items()},
            "failures": failures, "reports": reports}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--member", help="exact scene member; default all scenes")
    parser.add_argument("--lightmaps", type=Path, action="append", default=[], help="original lightmaps_N.DAT archive, repeatable")
    parser.add_argument("--output", type=Path, help="metadata JSON outside original installation")
    args = parser.parse_args()
    if args.output and any(args.output.resolve().is_relative_to(path.resolve().parent)
                           for path in (args.archive, *args.lightmaps)):
        raise ValueError("metadata output must be outside original archive directories")
    result = inspect(args.archive, args.member, args.lightmaps)
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
