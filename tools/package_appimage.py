#!/usr/bin/env python3
"""Build the portable, asset-free IKV Type-2 AppImage.

Host:  python3 tools/package_appimage.py   (needs docker or podman and network on first run)
       Builds tools/appimage/Dockerfile.ubuntu24 (pinned Ubuntu 24.04, OpenSSL 3.5.9, SDL 3.4.18),
       then compiles and assembles inside it with the source tree mounted read-only.
Output: dist/IKV-Linux-<version>-x86_64.AppImage, .sha256 and a .json inventory/licence report.
Work:  build-appimage/ (build tree, AppDir, runtime cache). Original game assets are never read.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
VERSION = "0.2.0"
NAME = f"IKV-Linux-{VERSION}-x86_64.AppImage"
IMAGE = f"ikv-appimage-toolchain:{VERSION}"
# Same pinned static Type-2 runtime as the sibling Kabus 22 AppImage; sources/licences in packaging/runtime-uruntime.
RUNTIME_NAME = "uruntime-appimage-squashfs-lite-x86_64"
RUNTIME_URL = "https://github.com/VHSgunzo/uruntime/releases/download/v0.8.1/" + RUNTIME_NAME
RUNTIME_SHA = "32c6024a14ff375749ee3bed3330c1586a6dd2bfeddbbd7ceacc053aec2b27a3"
# Signed upstream innoextract 1.9 amd64 helper (packaging/helper-innoextract/PROVENANCE.txt).
HELPER_SHA = "7a8ad941deeb0de8d1f804830f339fd17e27cfbce473615d79d1d65c106884e9"
PG = "16"
QT_PLUGINS = Path("/usr/lib/x86_64-linux-gnu/qt6/plugins")
PRIORITY_LIBS = [Path("/opt/ikv-openssl/lib")]  # OpenSSL 3.5 (ARGON2ID) wins over Ubuntu's 3.0 libssl.so.3
# Host-provided ABI: the AppImage excludelist (github.com/AppImageCommunity/pkg2appimage/blob/master/excludelist,
# sha256 50db0f89…) plus the Kabus 22 GPU/Wayland policy. libstdc++/libgcc_s are bundled as in Kabus 22.
EXCLUDELIST = set("""
ld-linux.so.2 ld-linux-x86-64.so.2 libanl.so.1 libBrokenLocale.so.1 libcidn.so.1 libc.so.6 libdl.so.2 libm.so.6
libmvec.so.1 libnss_compat.so.2 libnss_dns.so.2 libnss_files.so.2 libnss_hesiod.so.2 libnss_nisplus.so.2 libnss_nis.so.2
libpthread.so.0 libresolv.so.2 librt.so.1 libthread_db.so.1 libutil.so.1 libGL.so.1 libEGL.so.1 libGLdispatch.so.0
libGLX.so.0 libOpenGL.so.0 libdrm.so.2 libglapi.so.0 libgbm.so.1 libxcb.so.1 libX11.so.6 libX11-xcb.so.1
libwayland-client.so.0 libasound.so.2 libfontconfig.so.1 libfreetype.so.6 libharfbuzz.so.0 libcom_err.so.2
libexpat.so.1 libgpg-error.so.0 libICE.so.6 libSM.so.6 libusb-1.0.so.0 libuuid.so.1 libz.so.1 libjack.so.0
libpipewire-0.3.so.0 libxcb-dri3.so.0 libxcb-dri2.so.0 libfribidi.so.0 libgmp.so.10""".split())
HOST = re.compile(r"(?:linux-vdso.*|lib(?:GL|GLX(?:_[^.]+)?|GLdispatch|OpenGL|EGL(?:_[^.]+)?|GLESv[12]|vulkan(?:_[^.]+)?|"
                  r"drm(?:_[^.]+)?|gbm|wayland-(?:client|egl|cursor|server)|cuda|nvidia[^.]*|vdpau(?:_[^.]+)?|"
                  r"va(?:-x11|-drm|-wayland)?)\.so(?:\.[0-9]+)*)\Z")
APPRUN = f'''#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# Host Qt plugin paths would load plugins built for another Qt ABI into the bundled Qt.
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH
# Sunucu Kur finds initdb/pg_ctl/psql/pg_dump on PATH: prefer the bundled PostgreSQL {PG}.
PATH="$HERE/usr/lib/postgresql/{PG}/bin:$PATH"
export PATH
case "${{1:-}}" in
ikv-launcher|ikv-client|ikv-auth|ikv-server|ikv-updater|ikv-assets|ikv-original-inspect|innoextract)
    tool=$1; shift; exec "$HERE/usr/bin/$tool" "$@" ;;
esac
exec "$HERE/usr/bin/ikv-launcher" "$@"
'''
DESKTOP = f'''[Desktop Entry]
Type=Application
Name=İstanbul Kıyamet Vakti
GenericName=IKV native launcher
Comment=Install from your own original installer, log in and play
Exec=ikv-launcher
Icon=ikv-launcher
Terminal=false
Categories=Game;RolePlaying;
X-AppImage-Version={VERSION}
'''
# Generic packaging artwork (crescent over a dome), not original game art.
ICON = '''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256"><rect x="16" y="16" width="224" height="224" rx="40" fill="#1b2230"/><path d="M56 196h144v-12H56zM72 184c0-42 25-72 56-72s56 30 56 72z" fill="#c9b27c"/><rect x="122" y="88" width="12" height="26" fill="#c9b27c"/><circle cx="150" cy="66" r="26" fill="#e8e1cf"/><circle cx="162" cy="58" r="22" fill="#1b2230"/></svg>'''
QTCONF = "[Paths]\nPrefix = ..\nPlugins = lib/qt6/plugins\n"


def run(*args, **kwargs) -> str:
    result = subprocess.run([str(a) for a in args], check=True, text=True, stdout=subprocess.PIPE, **kwargs)
    return result.stdout


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def is_elf(path: Path) -> bool:
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def needed(path: Path) -> list[str]:
    return re.findall(r"\(NEEDED\).*\[([^\]]+)\]", run("readelf", "-d", "-W", path))


def host_lib(name: str) -> bool:
    return name in EXCLUDELIST or bool(HOST.fullmatch(name))


def loader_cache() -> dict[str, Path]:
    cache: dict[str, Path] = {}
    for line in run("ldconfig", "-p").splitlines():
        match = re.match(r"\s*(\S+)\s+\([^)]*x86-64[^)]*\)\s+=>\s+(/\S+)", line)
        if match:
            cache.setdefault(match[1], Path(match[2]))
    return cache


def owner(path: Path) -> str | None:
    """dpkg package owning path (usrmerge: try /usr/lib and /lib spellings)."""
    for candidate in {str(path), str(path).removeprefix("/usr"), str(path.resolve())}:
        result = subprocess.run(["dpkg-query", "-S", candidate], text=True, capture_output=True)
        owners = [line.split(":", 1)[0].split(",")[0].strip() for line in result.stdout.splitlines()
                  if not line.startswith("diversion ")]
        if result.returncode == 0 and owners:
            return owners[0]
    return None


def close_dependencies(appdir: Path) -> tuple[dict, dict]:
    """Copy every non-host DT_NEEDED into usr/lib, then point each ELF's DT_RPATH at usr/lib."""
    lib = appdir / "usr/lib"
    cache = loader_cache()
    bundled: dict[str, dict] = {}
    host: dict[str, str] = {}
    elves = [p for p in sorted(appdir.rglob("*")) if p.is_file() and not p.is_symlink() and is_elf(p)]
    pending = [(p, p.parent) for p in elves]  # (ELF, directory its own $ORIGIN-relative RUNPATH refers to)
    while pending:
        elf, origin = pending.pop()
        rpaths = [Path(d.replace("$ORIGIN", str(origin)).replace("${ORIGIN}", str(origin)))
                  for entry in re.findall(r"\((?:RPATH|RUNPATH)\).*\[([^\]]*)\]", run("readelf", "-d", "-W", elf))
                  for d in entry.split(":") if d]
        for name in needed(elf):
            if host_lib(name):
                host.setdefault(name, str(cache.get(name, "?")))
                continue
            if name in bundled:
                continue
            source = next((d / name for d in PRIORITY_LIBS + rpaths if (d / name).exists()), None) or cache.get(name)
            if source is None:
                raise SystemExit(f"Unresolved dependency {elf} -> {name}")
            target = lib / name
            shutil.copyfile(source.resolve(), target)
            target.chmod(0o755)
            bundled[name] = {"source": str(source.resolve()), "sha256": sha256(target), "package": owner(source)}
            elves.append(target)
            pending.append((target, source.resolve().parent))
    for elf in elves:
        if any(not host_lib(name) for name in needed(elf)):
            run("patchelf", "--force-rpath", "--set-rpath",
                "$ORIGIN" if elf.parent == lib else "$ORIGIN/" + os.path.relpath(lib, elf.parent), elf)
    return bundled, host


def copy_tree(source: Path, target: Path, skip=lambda p: False) -> None:
    for path in sorted(source.rglob("*")):
        if path.is_file() and not skip(path):
            destination = target / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path.resolve(), destination)
            shutil.copymode(path.resolve(), destination)


def inside(args) -> None:
    src, out, dist = Path("/src"), Path("/out"), Path("/dist")
    build, appdir = out / "build", out / "AppDir"
    flags = "-march=x86-64 -mtune=generic"
    run("cmake", "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
        "-DCMAKE_CXX_COMPILER=g++-14", f"-DCMAKE_CXX_FLAGS={flags}",
        "-DCMAKE_PREFIX_PATH=/opt/ikv-sdl3", "-DOPENSSL_ROOT_DIR=/opt/ikv-openssl", "-DOPENSSL_USE_STATIC_LIBS=OFF",
        "-DIKV_BUILD_CLIENT=ON", "-DIKV_BUILD_VULKAN=ON", "-DIKV_BUILD_LAUNCHER=ON",
        "-DIKV_BUILD_ASSET_TOOLS=ON", "-DIKV_BUILD_ORIGINAL_TOOLS=ON")
    log = subprocess.run(["cmake", "--build", build, "--parallel", str(os.cpu_count())],
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "build.log").write_text(log.stdout)
    if log.returncode:
        sys.exit(log.stdout[-6000:])
    warnings = sorted(set(re.findall(r"^(\S+:\d+:\d+: warning: .*)$", log.stdout, re.MULTILINE)))
    print(f"compiler warnings in this build: {len(warnings)}")
    shutil.rmtree(appdir, ignore_errors=True)
    run("cmake", "--install", build, "--prefix", appdir / "usr", "--strip")
    usr = appdir / "usr"
    helper = src / "packaging/helper-innoextract"
    shutil.copyfile(helper / "bin/amd64/innoextract", usr / "bin/innoextract")
    (usr / "bin/innoextract").chmod(0o755)
    if sha256(usr / "bin/innoextract") != HELPER_SHA:
        raise SystemExit("innoextract differs from the signed upstream 1.9 helper")
    (usr / "bin/qt.conf").write_text(QTCONF)
    plugins = usr / "lib/qt6/plugins"
    wanted = {"platforms": {"libqxcb.so", "libqwayland-generic.so", "libqwayland-egl.so", "libqoffscreen.so"},
              "platformthemes": {"libqxdgdesktopportal.so"}}
    for category in ("platforms", "platformthemes", "platforminputcontexts", "xcbglintegrations", "tls", "imageformats",
                     "wayland-shell-integration", "wayland-decoration-client", "wayland-graphics-integration-client"):
        copy_tree(QT_PLUGINS / category, plugins / category,
                  lambda p, c=category: c in wanted and p.name not in wanted[c])
    pg_lib = Path(f"/usr/lib/postgresql/{PG}")
    copy_tree(pg_lib / "bin", usr / f"lib/postgresql/{PG}/bin")
    # JIT is optional (llvmjit is skipped when absent) and would pull in a 100 MB libLLVM.
    copy_tree(pg_lib / "lib", usr / f"lib/postgresql/{PG}/lib", lambda p: p.name.startswith("llvmjit") or "bitcode" in p.parts)
    copy_tree(Path(f"/usr/share/postgresql/{PG}"), usr / f"share/postgresql/{PG}", lambda p: "man" in p.parts)
    bundled, host = close_dependencies(appdir)

    licenses = usr / "share/licenses/ikv"
    copy_tree(helper, licenses / "innoextract", lambda p: p.parts[-3:-1] == ("bin", "amd64"))
    copy_tree(src / "packaging/runtime-uruntime", licenses / "uruntime")
    shutil.copyfile(src / "packaging/NOTICE.txt", licenses / "NOTICE.txt")
    shutil.copyfile("/opt/ikv-openssl/share/licenses/LICENSE.txt", licenses / "openssl-3.5.9-LICENSE.txt")
    shutil.copyfile("/opt/ikv-sdl3/share/licenses/LICENSE.txt", licenses / "SDL3-3.4.18-LICENSE.txt")
    copy_tree(Path("/usr/local/share/licenses"), licenses)  # libjpeg-turbo 3.2.0 (static), Vulkan-Headers 1.4.363
    copy_tree(Path("/usr/share/common-licenses"), licenses / "common-licenses")
    packages = {bundled[n]["package"] for n in bundled if bundled[n]["package"]}
    packages |= {owner(Path("/usr/lib/x86_64-linux-gnu/libQt6Core.so.6")), "qt6-qpa-plugins", "qt6-wayland",
                 f"postgresql-{PG}", f"postgresql-client-{PG}", "libboost-dev", "nlohmann-json3-dev"}
    versions = {}
    (licenses / "ubuntu").mkdir()
    for package in sorted(p for p in packages if p):
        version, source, source_version = run("dpkg-query", "-W", "-f=${Version}\t${source:Package}\t${source:Version}",
                                              package).split("\t")
        versions[package] = {"version": version, "source_package": source, "source_version": source_version}
        copyright_file = Path(f"/usr/share/doc/{package}/copyright").resolve()
        if copyright_file.is_file():
            shutil.copyfile(copyright_file, licenses / "ubuntu" / f"{package}.copyright")

    (appdir / "AppRun").write_text(APPRUN)
    (appdir / "AppRun").chmod(0o755)
    (appdir / "ikv-launcher.desktop").write_text(DESKTOP)
    (appdir / "ikv-launcher.svg").write_text(ICON)
    (appdir / ".DirIcon").write_text(ICON)
    (usr / "share/applications").mkdir(parents=True, exist_ok=True)
    (usr / "share/applications/ikv-launcher.desktop").write_text(DESKTOP)
    (usr / "share/icons/hicolor/scalable/apps").mkdir(parents=True, exist_ok=True)
    (usr / "share/icons/hicolor/scalable/apps/ikv-launcher.svg").write_text(ICON)

    glibc = sorted({v for p in appdir.rglob("*") if p.is_file() and is_elf(p)
                    for v in re.findall(r"GLIBC_([0-9.]+)", run("readelf", "-V", "-W", p))},
                   key=lambda v: tuple(map(int, v.split("."))))
    report = {"version": VERSION, "asset_free": True, "compiler_warnings": warnings,
              "bundled_libraries": bundled, "host_libraries": host, "ubuntu_packages": versions,
              "minimum_glibc": glibc[-1] if glibc else None,
              "components": {"openssl": "3.5.9 (source-built, /opt/ikv-openssl)", "sdl3": "3.4.18 (source-built, static)",
                             "postgresql": versions.get(f"postgresql-{PG}", {}).get("version"),
                             "innoextract": "1.9 signed upstream amd64 helper", "runtime": "uruntime v0.8.1"}}
    (licenses / "packaging.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    report["inventory"] = {str(p.relative_to(appdir)): {"size": p.stat().st_size, "sha256": sha256(p)}
                           for p in sorted(appdir.rglob("*")) if p.is_file()}

    runtime = Path(args.runtime).read_bytes()
    if hashlib.sha256(runtime).hexdigest() != RUNTIME_SHA or runtime.count(b"URUNTIME_EXTRACT=3") != 1:
        raise SystemExit("Runtime differs from pinned uruntime v0.8.1")
    # Same fixed-width policy change as Kabus 22: FUSE first, extraction fallback when FUSE is unavailable.
    runtime = runtime.replace(b"URUNTIME_EXTRACT=3", b"URUNTIME_EXTRACT=2")
    squash = out / "payload.squashfs"
    squash.unlink(missing_ok=True)
    run("mksquashfs", appdir, squash, "-noappend", "-all-root", "-no-xattrs", "-no-exports", "-comp", "zstd",
        "-Xcompression-level", "19", "-mkfs-time", "0", "-all-time", "0", "-no-progress", "-quiet")
    image = dist / NAME
    with image.open("wb") as output, squash.open("rb") as payload:
        output.write(runtime)
        shutil.copyfileobj(payload, output, 1 << 20)
    image.chmod(0o755)
    squash.unlink()
    digest = sha256(image)
    (dist / (NAME + ".sha256")).write_text(f"{digest}  {NAME}\n")
    report["appimage"] = {"file": NAME, "size": image.stat().st_size, "sha256": digest, "squashfs_offset": len(runtime)}
    (dist / (NAME + ".json")).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"{image}: {image.stat().st_size} bytes sha256 {digest}; glibc >= {report['minimum_glibc']}")


def host(args) -> None:
    engine = shutil.which("podman") or shutil.which("docker")
    if not engine:
        raise SystemExit("docker or podman is required")
    work, dist = ROOT / "build-appimage", ROOT / "dist"
    (work / "cache").mkdir(parents=True, exist_ok=True)
    dist.mkdir(exist_ok=True)
    runtime = work / "cache" / RUNTIME_NAME
    if not runtime.exists():
        data = urllib.request.urlopen(RUNTIME_URL, timeout=90).read()
        if hashlib.sha256(data).hexdigest() != RUNTIME_SHA:
            raise SystemExit("uruntime download checksum mismatch")
        runtime.write_bytes(data)
    with (ROOT / "tools/appimage/Dockerfile.ubuntu24").open("rb") as dockerfile:
        subprocess.run([engine, "build", "-t", IMAGE, "-"], stdin=dockerfile, check=True)
    subprocess.run([engine, "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp",
                    "-v", f"{ROOT}:/src:ro", "-v", f"{work}:/out", "-v", f"{dist}:/dist", IMAGE,
                    "python3", "/src/tools/package_appimage.py", "--inside",
                    "--runtime", f"/out/cache/{RUNTIME_NAME}"], check=True)


def self_check() -> None:
    assert host_lib("libc.so.6") and host_lib("libvulkan.so.1") and host_lib("libwayland-cursor.so.0")
    assert host_lib("libfontconfig.so.1") and not host_lib("libstdc++.so.6") and not host_lib("libssl.so.3")
    assert not host_lib("libQt6Core.so.6") and not host_lib("libxkbcommon.so.0")
    print("package_appimage self-check passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--runtime", help=argparse.SUPPRESS)
    parser.add_argument("--self-check", action="store_true", help="run the library-policy self-check")
    arguments = parser.parse_args()
    self_check() if arguments.self_check else inside(arguments) if arguments.inside else host(arguments)
