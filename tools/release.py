#!/usr/bin/env python3
"""Build, smoke, package and sign an IKV Linux release; optionally publish it as a GitHub prerelease.

    python3 tools/release.py X.Y.Z [--publish --notes FILE]

1. Bumps the version (CMakeLists.txt project(), README AppImage path; package_appimage.py reads CMake).
2. Builds dist/IKV-Linux-X.Y.Z-x86_64.AppImage (+.sha256, .json) with tools/package_appimage.py.
3. Smokes the extracted AppImage (server/client --help, offscreen launcher, bundled config and migrations).
4. Packs dist/ikv-linux-x86_64-X.Y.Z.tar.gz (only bin/ikv-linux.AppImage, deterministic), signs it with
   tools/sign_manifest.py (channel experimental, next sequence from the local ledger) into dist/manifest.json
   + manifest.sig, proves it with the image's own ikv-updater install against packaging/ikv-release-public.pem,
   and only then advances the ledger.
5. --publish: commit, push main, gh release create vX.Y.Z with the six assets, download them back and verify.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REPO = "sergen213/ikv-linux"
CHANNEL = "experimental"
SECRETS = Path.home() / ".local/share/ikv-dev/secrets"
KEY = SECRETS / "release-signing.pem"
LEDGER = SECRETS / "release-sequence.json"
INITIAL_SEQUENCE = 2026101100
PUBLIC = ROOT / "packaging/ikv-release-public.pem"
PAYLOAD = "bin/ikv-linux.AppImage"


def die(message: str) -> None:
    raise SystemExit("release: " + message)


def run(*args, capture: bool = False, **kwargs) -> str:
    print("+", " ".join(map(str, args)), flush=True)
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE if capture else None, **kwargs)
    if result.returncode:
        die(f"{Path(str(args[0])).name} failed with exit code {result.returncode}")
    return result.stdout or ""


def git(*args) -> str:
    return run("git", *args, capture=True, cwd=ROOT).strip()


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def assets(version: str) -> list[Path]:
    image = f"IKV-Linux-{version}-x86_64.AppImage"
    return [ROOT / "dist" / n for n in (image, image + ".sha256", image + ".json", "manifest.json", "manifest.sig",
                                         f"ikv-linux-x86_64-{version}.tar.gz")]


def preflight(args) -> None:
    if not KEY.is_file():
        die(f"signing key missing: {KEY}")
    if not PUBLIC.is_file():
        die(f"trust root missing: {PUBLIC}")
    derived = run("openssl", "pkey", "-in", KEY, "-pubout", capture=True)
    if derived.strip() != PUBLIC.read_text().strip():
        die(f"{PUBLIC.relative_to(ROOT)} is not the public half of {KEY}")
    tag = "v" + args.version
    if subprocess.run(["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"], cwd=ROOT,
                      stdout=subprocess.DEVNULL).returncode == 0:
        die(f"tag {tag} already exists locally")
    if git("ls-remote", "--tags", "origin", f"refs/tags/{tag}"):
        die(f"tag {tag} already exists on origin")
    # Everything `git add -A` would commit: tracked files plus untracked, non-ignored ones.
    pems = [p for p in git("ls-files", "-co", "--exclude-standard", "-z").split("\0")
            if p.endswith(".pem") and p != str(PUBLIC.relative_to(ROOT))]
    if pems:
        die("refusing: PEM files would be committed: " + ", ".join(pems))
    if args.publish:
        if not args.notes or not args.notes.is_file():
            die("--publish needs --notes FILE")
        if git("rev-parse", "--abbrev-ref", "HEAD") != "main":
            die("--publish must run on branch main")
        if not git("config", "user.email"):
            die("git user.email is not configured")
        run("gh", "auth", "status", capture=True)


def bump(version: str) -> None:
    cmake = ROOT / "CMakeLists.txt"
    text = cmake.read_text()
    old = re.search(r"^project\(\w+ VERSION ([0-9.]+)", text, re.MULTILINE).group(1)
    if old == version:
        return
    cmake.write_text(text.replace(f"VERSION {old} LANGUAGES", f"VERSION {version} LANGUAGES", 1))
    readme = ROOT / "README.md"
    readme.write_text(readme.read_text().replace(f"IKV-Linux-{old}-x86_64", f"IKV-Linux-{version}-x86_64"))
    print(f"version {old} -> {version}")


def smoke(appdir: Path, temp: Path) -> None:
    apprun = appdir / "AppRun"
    run(apprun, "ikv-server", "--help", capture=True)
    run(apprun, "ikv-client", "--help", capture=True)
    home = temp / "home"
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / "data"), XDG_CONFIG_HOME=str(home / "config"),
               XDG_CACHE_HOME=str(home / "cache"), QT_QPA_PLATFORM="offscreen")
    launcher = subprocess.Popen([apprun, "ikv-launcher"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        _, stderr = launcher.communicate(timeout=5)
        die(f"ikv-launcher exited early with {launcher.returncode}: {stderr.decode(errors='replace')[-2000:]}")
    except subprocess.TimeoutExpired:
        launcher.terminate()
        launcher.communicate()
    share = appdir / "usr/share/ikv"
    if (share / "config/gameplay.json").read_bytes() != (ROOT / "config/gameplay.json").read_bytes():
        die("bundled config/gameplay.json differs from the repository")
    latest = max(p.name for p in (ROOT / "database/migrations").glob("*.sql"))
    if not (share / "database/migrations" / latest).is_file():
        die(f"latest migration {latest} is not bundled")
    print(f"smoke passed (latest migration {latest})")


def pack(image: Path, package: Path, payload: Path) -> None:
    """Deterministic tar.gz with the single member bin/ikv-linux.AppImage (gzip -n equivalent)."""
    (payload / "bin").mkdir(parents=True)
    shutil.copyfile(image, payload / PAYLOAD)
    (payload / PAYLOAD).chmod(0o755)
    info = tarfile.TarInfo(PAYLOAD)
    info.size, info.mode, info.mtime = image.stat().st_size, 0o755, int(git("log", "-1", "--format=%ct"))
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    with package.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed, \
            tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive, \
            (payload / PAYLOAD).open("rb") as source:
        archive.addfile(info, source)


def updater_install(appdir: Path, manifest: Path, signature: Path, package: Path, root: Path, image: Path) -> None:
    run(appdir / "AppRun", "ikv-updater", "install", "--manifest", manifest, "--signature", signature,
        "--package", package, "--trust", PUBLIC, "--root", root)
    if sha256(root / "current" / PAYLOAD) != sha256(image):
        die("ikv-updater installed a different AppImage")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version")
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("--notes", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        die("version must be X.Y.Z")
    preflight(args)
    version, dist = args.version, ROOT / "dist"
    bump(version)
    run(sys.executable, ROOT / "tools/package_appimage.py", "--version", version, cwd=ROOT)
    image, *_, manifest, signature, package = assets(version)

    with tempfile.TemporaryDirectory(prefix="ikv-release-") as name:
        temp = Path(name)
        run(image, "--appimage-extract", cwd=temp, capture=True)
        appdir = temp / "squashfs-root"
        smoke(appdir, temp)
        pack(image, package, temp / "payload")
        ledger = json.loads(LEDGER.read_text()) if LEDGER.exists() else {}
        sequence = int(ledger.get(CHANNEL, INITIAL_SEQUENCE)) + 1
        run(sys.executable, ROOT / "tools/sign_manifest.py", "--root", temp / "payload", "--package", package,
            "--version", version, "--sequence", sequence, "--channel", CHANNEL, "--output", manifest,
            "--signature", signature, "--key-file", KEY)
        updater_install(appdir, manifest, signature, package, temp / "install", image)
        ledger[CHANNEL] = sequence
        written = LEDGER.with_suffix(".tmp")
        written.write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n")
        written.chmod(0o600)
        written.replace(LEDGER)
        print(f"signed {CHANNEL} sequence {sequence}; ledger updated")

        files = assets(version)
        for path in files:
            print(f"{path.relative_to(ROOT)}  {path.stat().st_size} bytes  sha256 {sha256(path)}")
        if not args.publish:
            return

        title = next(l for l in args.notes.read_text().splitlines() if l.strip()).lstrip("# ").strip()
        git("add", "-A")
        staged = [p for p in git("diff", "--cached", "--name-only", "-z").split("\0")
                  if p.endswith(".pem") and p != str(PUBLIC.relative_to(ROOT))]
        if staged:
            die("refusing: staged PEM files: " + ", ".join(staged))
        run("git", "commit", "-m", f"IKV Linux {version}: {title}", cwd=ROOT)
        run("git", "push", "origin", "main", cwd=ROOT)
        run("gh", "release", "create", "v" + version, "--repo", REPO, "--prerelease", "--target", "main",
            "--title", f"İKV Linux v{version} (experimental prerelease)", "--notes-file", args.notes, *files)
        back = temp / "download"
        run("gh", "release", "download", "v" + version, "--repo", REPO, "--dir", back)
        for path in files:
            if sha256(back / path.name) != sha256(path):
                die(f"downloaded {path.name} differs from {path}")
        run("sha256sum", "-c", image.name + ".sha256", cwd=back)
        updater_install(appdir, back / manifest.name, back / signature.name, back / package.name,
                        temp / "install-download", back / image.name)
        print(f"published and verified https://github.com/{REPO}/releases/tag/v{version}")


if __name__ == "__main__":
    main()
