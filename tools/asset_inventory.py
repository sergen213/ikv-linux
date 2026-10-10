#!/usr/bin/env python3
"""Read-only metadata/hash inventory of a completed, user-supplied installation."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys


def inventory(root):
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("installation root must be a directory")
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"refusing symbolic link: {path}")
        if not path.is_file():
            continue
        before = path.stat()
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            prefix = stream.read(16)
            digest.update(prefix)
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns, before.st_ino) != (
            after.st_size, after.st_mtime_ns, after.st_ino
        ):
            raise ValueError(f"file changed during inventory: {path}")
        files.append({"path": path.relative_to(root).as_posix(),
                      "bytes": after.st_size, "sha256": digest.hexdigest(),
                      "prefix16_hex": prefix.hex()})
    extensions = Counter(Path(item["path"]).suffix.lower() or "<none>" for item in files)
    return {"schema": "ikv-local-asset-inventory-1", "root": root.name,
            "file_count": len(files), "total_bytes": sum(item["bytes"] for item in files),
            "extension_counts": dict(sorted(extensions.items())), "files": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path,
                        help="metadata report outside installation; default stdout")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    if args.output and args.output.resolve().is_relative_to(root):
        parser.error("output must be outside the original installation")
    result = json.dumps(inventory(root), ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding="utf-8")
        print(f"{args.output}: metadata for {json.loads(result)['file_count']} files")
    else:
        sys.stdout.write(result)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(str(error))
