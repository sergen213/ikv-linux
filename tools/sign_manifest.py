#!/usr/bin/env python3
"""Validate a release tar against its payload and sign exact manifest bytes with an external Ed25519 PEM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
import tempfile


MAX_PACKAGE = 1024 * 1024 * 1024
MAX_FILE = 512 * 1024 * 1024
MAX_TOTAL = 4 * 1024 * 1024 * 1024
MAX_FILES = 10000
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def safe_path(name):
    if not name or len(name) > 512:
        raise ValueError("payload path must contain 1..512 safe ASCII characters")
    components = name.split("/")
    if components[0] == ".ikv-update" or any(
        component in ("", ".", "..") or len(component) > 128
        or re.fullmatch(r"[A-Za-z0-9._-]+", component) is None
        for component in components
    ):
        raise ValueError("unsafe payload path: " + repr(name))
    return name


def regular_file(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    source = os.fdopen(descriptor, "rb")
    if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
        source.close()
        raise ValueError("not a regular file: " + str(path))
    return source


def identity(metadata):
    return (metadata.st_dev, metadata.st_ino, metadata.st_size,
            metadata.st_mtime_ns, metadata.st_ctime_ns, metadata.st_mode)


def hash_stream(source, maximum):
    digest = hashlib.sha256()
    size = 0
    while chunk := source.read(1024 * 1024):
        size += len(chunk)
        if size > maximum:
            raise ValueError("file exceeds permitted size")
        digest.update(chunk)
    return size, digest.hexdigest()


def permissions(mode):
    result = stat.S_IMODE(mode)
    if result & 0o7000:
        raise ValueError("setuid, setgid and sticky permission bits are not allowed")
    return result


def payload_inventory(root):
    if not stat.S_ISDIR(root.lstat().st_mode):
        raise ValueError("payload root must be a real directory, not a symlink")
    files, modes, directories = {}, {}, {}
    total = 0

    def unreadable(error):
        raise error

    for directory, children, names in os.walk(root, followlinks=False, onerror=unreadable):
        children.sort()
        names.sort()
        for name in children + names:
            path = Path(directory) / name
            relative = safe_path(path.relative_to(root).as_posix())
            metadata = path.lstat()
            mode = permissions(metadata.st_mode)
            if stat.S_ISDIR(metadata.st_mode):
                directories[relative] = mode
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError("payload contains a symlink or special file: " + relative)
            if metadata.st_size > MAX_FILE or len(files) >= MAX_FILES:
                raise ValueError("payload file size or count limit exceeded")
            with regular_file(path) as source:
                opened = os.fstat(source.fileno())
                if identity(opened) != identity(metadata):
                    raise ValueError("payload changed while opening: " + relative)
                size, digest = hash_stream(source, MAX_FILE)
                if identity(os.fstat(source.fileno())) != identity(opened) or size != opened.st_size:
                    raise ValueError("payload changed while hashing: " + relative)
            total += size
            if total > MAX_TOTAL:
                raise ValueError("payload total exceeds 4 GiB")
            modes[relative] = mode
            files[relative] = {"path": relative, "size": size, "sha256": digest,
                               "executable": bool(mode & 0o111)}
    if not files:
        raise ValueError("payload must contain at least one regular file")
    parents = {str(parent) for name in files for parent in Path(name).parents if str(parent) != "."}
    if directories.keys() - parents:
        raise ValueError("payload contains empty directories not represented by signed files")
    return files, modes, directories


def package_inventory(package, files, modes, directories):
    with regular_file(package) as source:
        before = os.fstat(source.fileno())
        if not 0 < before.st_size <= MAX_PACKAGE:
            raise ValueError("package size must be 1 byte..1 GiB")
        magic = source.read(2)
        source.seek(0)
        seen, archived_files = set(), set()
        # Explicit formats avoid accidentally publishing bzip2/xz/zip packages.
        with tarfile.open(fileobj=source, mode="r:gz" if magic == b"\x1f\x8b" else "r:") as archive:
            for member in archive:
                name = member.name
                if member.isdir() and name.endswith("/"):
                    name = name[:-1]
                safe_path(name)
                if name in seen:
                    raise ValueError("duplicate archive member: " + name)
                seen.add(name)
                mode = permissions(member.mode)
                if member.isdir():
                    if name not in directories or mode != directories[name] or member.size != 0:
                        raise ValueError("archive directory does not match payload: " + name)
                elif member.isreg() and member.sparse is None:
                    if name not in files:
                        raise ValueError("archive file absent from payload: " + name)
                    expected = files[name]
                    if member.size != expected["size"] or mode != modes[name]:
                        raise ValueError("archive file size or mode differs from payload: " + name)
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise ValueError("archive file cannot be read: " + name)
                    with extracted:
                        size, digest = hash_stream(extracted, MAX_FILE)
                    if size != expected["size"] or digest != expected["sha256"]:
                        raise ValueError("archive file content differs from payload: " + name)
                    archived_files.add(name)
                else:
                    raise ValueError("archive contains a link, sparse or special file: " + name)
        if archived_files != files.keys():
            raise ValueError("archive is missing payload files")
        source.seek(0)
        size, digest = hash_stream(source, MAX_PACKAGE)
        if identity(os.fstat(source.fileno())) != identity(before) or size != before.st_size:
            raise ValueError("package changed during validation")
        return size, digest


def openssl(arguments):
    result = subprocess.run(["openssl", *map(str, arguments)], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    if result.returncode != 0:
        raise ValueError("OpenSSL failed: " + result.stderr.decode("utf-8", errors="replace").strip())
    return result.stdout


def write_output(path, data):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".ikv-sign-", dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sequence", required=True)
    parser.add_argument("--channel", choices=("stable", "experimental"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--signature", type=Path, required=True)
    parser.add_argument("--key-file", type=Path, required=True)
    args = parser.parse_args()
    try:
        if re.fullmatch(r"[0-9]+", args.sequence) is None:
            raise ValueError("sequence must be a positive ASCII decimal uint64")
        sequence = int(args.sequence)
        if not 1 <= sequence <= 18446744073709551615:
            raise ValueError("sequence must be in 1..18446744073709551615")
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", args.version) is None:
            raise ValueError("version must be a safe ASCII token of at most 96 characters")
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,159}", args.package.name) is None:
            raise ValueError("package basename must be a safe ASCII token of at most 160 characters")
        root = args.root.absolute()
        package = args.package.absolute()
        private_key = args.key_file.resolve(strict=True)
        if not private_key.is_file():
            raise ValueError("--key-file must name a supplied private PEM file")
        destinations = [args.output.absolute(), args.signature.absolute()]
        resolved = [path.resolve() for path in destinations]
        if resolved[0] == resolved[1]:
            raise ValueError("manifest and signature outputs must be distinct")
        for path, target in zip(destinations, resolved):
            if target in (package.resolve(), private_key) or target.is_relative_to(root.resolve()):
                raise ValueError("outputs must not overwrite inputs or lie inside the payload")
            if not path.parent.is_dir() or path.is_dir():
                raise ValueError("output parent must exist and output must not be a directory")
        public_der = openssl(["pkey", "-in", private_key, "-pubout", "-outform", "DER"])
        if len(public_der) != 44 or not public_der.startswith(ED25519_SPKI_PREFIX):
            raise ValueError("supplied private key must be Ed25519")
        files, modes, directories = payload_inventory(root)
        package_size, package_sha256 = package_inventory(package, files, modes, directories)
        manifest = {"schema": 2, "sequence": sequence, "version": args.version, "channel": args.channel,
                    "package": package.name, "package_size": package_size,
                    "package_sha256": package_sha256,
                    "files": [files[name] for name in sorted(files)]}
        encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        if len(encoded) > 1024 * 1024:
            raise ValueError("encoded manifest exceeds 1 MiB")
        # pkeyutl requires a seekable exact-byte input for Ed25519; never pass key content on argv.
        with tempfile.TemporaryDirectory(prefix="ikv-sign-") as temporary:
            manifest_input = Path(temporary) / "manifest.json"
            signature_output = Path(temporary) / "signature.bin"
            manifest_input.write_bytes(encoded)
            openssl(["pkeyutl", "-sign", "-rawin", "-inkey", private_key,
                     "-in", manifest_input, "-out", signature_output])
            signature = signature_output.read_bytes()
        if len(signature) != 64:
            raise ValueError("OpenSSL did not produce a raw 64-byte Ed25519 signature")
        write_output(destinations[0], encoded)
        write_output(destinations[1], signature)
    except (OSError, ValueError, tarfile.TarError, subprocess.TimeoutExpired) as error:
        print("sign_manifest: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
