#!/usr/bin/env python3
"""Cross-build the IKV Windows (x64) client, launcher and server (hosting) and package them.

Host:  python3 tools/package_windows.py [X.Y.Z] [--build-only] [--work DIR] [--dist DIR]   (docker or podman)
       Builds tools/windows/Dockerfile (pinned Debian 13 snapshot, MinGW-w64 GCC 14 posix/SEH, Qt 6.11.3 win64_mingw,
       OpenSSL 3.5.9, SDL 3.4.18, ...), then configures/compiles inside it with the source tree mounted read-only.
       --build-only stops after compiling (build log: <work>/build.log).
Output: dist/IKV-Windows-<version>-x64-Setup.exe (NSIS, per-user) and dist/IKV-Windows-<version>-x64.zip (portable),
        each with .sha256, plus a .json inventory/licence report. Version defaults to CMake project().
Work:  build-windows/ (build tree, staged install). Original game assets are never read.
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
import zipfile

ROOT = Path(__file__).resolve().parent.parent
VERSION = re.search(r"^project\(\w+ VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(), re.MULTILINE).group(1)
IMAGE = "ikv-windows-toolchain:trixie"
QT = Path("/opt/w64/qt")
# DLL search order for the closure: Qt, our dependency prefix, the GCC 14 posix runtime, the MinGW-w64 CRT side libs,
# then EDB PostgreSQL (libpq and its OpenSSL/gettext DLLs) and the app-local Visual C++ runtime they import.
PGSQL, VCREDIST = Path("/opt/w64/pgsql"), Path("/opt/w64/vcredist")
SEARCH = [QT / "bin", Path("/opt/w64/bin"), Path("/usr/lib/gcc/x86_64-w64-mingw32/14-posix"), Path("/usr/x86_64-w64-mingw32/lib"),
          PGSQL / "bin", VCREDIST]
# The hosted server's PostgreSQL subset (bin/pgsql/, found by launcher_server.cpp pgBinDir()); EDB's own DLLs come first.
PG_TOOLS = ["initdb.exe", "pg_ctl.exe", "postgres.exe", "psql.exe", "pg_dump.exe"]
PG_SEARCH = [PGSQL / "bin", VCREDIST]
PG_LIB_SKIP = re.compile(r"(perl|python|pltcl)", re.IGNORECASE)  # procedural languages needing an absent Perl/Python/Tcl
QT_PLUGINS = {"platforms": ["qwindows.dll"], "styles": ["qmodernwindowsstyle.dll"],
              "tls": ["qschannelbackend.dll", "qcertonlybackend.dll"], "imageformats": ["qico.dll", "qjpeg.dll", "qgif.dll"]}
# Provided by Windows 10/11 itself or by the GPU driver (opengl32 front end, vulkan-1 loader): never bundled.
SYSTEM = set("""advapi32 authz bcrypt cfgmgr32 comctl32 comdlg32 crypt32 d2d1 d3d11 d3d12 d3d9 dcomp dnsapi dwmapi dwrite
dxgi gdi32 hid imm32 iphlpapi kernel32 mpr msvcrt mswsock ncrypt netapi32 normaliz ntdll ole32 oleaut32 opengl32 pdh powrprof
rpcrt4 secur32 setupapi shcore shell32 shlwapi user32 userenv usp10 uxtheme version vulkan-1 winhttp winmm winspool.drv
wintrust wldap32 wtsapi32 ws2_32""".split())


def run(*args, **kwargs) -> str:
    return subprocess.run([str(a) for a in args], check=True, text=True, stdout=subprocess.PIPE, **kwargs).stdout


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def system_dll(name: str) -> bool:
    name = name.lower()
    return name.startswith(("api-ms-win-", "ext-ms-win-")) or name.removesuffix(".dll") in SYSTEM


def imports(path: Path) -> list[str]:
    return re.findall(r"DLL Name: (\S+)", run("x86_64-w64-mingw32-objdump", "-p", path))


def close_dependencies(bin_dir: Path, scan: list[Path] | None = None, search: list[Path] = SEARCH) -> tuple[dict, list]:
    """windeployqt-equivalent closure: copy every non-system imported DLL into bin_dir, fail on anything unresolved."""
    bundled, system = {}, set()
    pending = scan if scan is not None else [p for p in sorted(bin_dir.rglob("*")) if p.suffix.lower() in (".exe", ".dll")]
    present = {p.name.lower() for p in bin_dir.iterdir()}
    while pending:
        pe = pending.pop()
        for name in imports(pe):
            if system_dll(name):
                system.add(name.lower())
                continue
            if name.lower() in present:
                continue
            # Windows resolves imports case-insensitively (EDB tools import LIBPQ.dll, the file is libpq.dll).
            source = next((p for d in search if d.is_dir() for p in sorted(d.iterdir()) if p.name.lower() == name.lower()), None)
            if source is None:
                raise SystemExit(f"Unresolved DLL {pe.name} -> {name}")
            target = bin_dir / source.name
            shutil.copyfile(source, target)
            if str(source).startswith("/usr/"):  # Debian's GCC/winpthreads runtime DLLs carry debug info (26 MB libstdc++)
                run("x86_64-w64-mingw32-strip", "--strip-unneeded", target)
            present.add(name.lower())
            bundled[source.name] = {"source": str(source), "sha256": sha256(target)}
            pending.append(target)
    return bundled, sorted(system)


def configure_and_build(version: str, out: Path) -> Path:
    build = out / "build"
    run("cmake", "-S", "/src", "-B", build, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=/src/tools/windows/mingw-w64.cmake",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=/opt/w64;/opt/w64/qt", "-DOPENSSL_USE_STATIC_LIBS=ON",
        "-DQT_NO_QTPATHS_DEPLOYMENT_WARNING=ON",  # Qt's deploy API is unused: close_dependencies() deploys
        "-DBUILD_TESTING=ON", "-DIKV_BUILD_SERVER=ON", "-DIKV_BUILD_ASSET_TOOLS=OFF", "-DIKV_BUILD_ORIGINAL_TOOLS=OFF",
        f"-DPostgreSQL_INCLUDE_DIR={PGSQL / 'include'}", f"-DPostgreSQL_LIBRARY={PGSQL / 'lib/libpq.lib'}",
        "-DIKV_BUILD_CLIENT=ON", "-DIKV_BUILD_VULKAN=ON", "-DIKV_BUILD_LAUNCHER=ON", f"-DIKV_VERSION={version}",
        "-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic")
    log = subprocess.run(["cmake", "--build", build, "--parallel", str(os.cpu_count())],
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "build.log").write_text(log.stdout)
    if log.returncode:
        sys.exit(log.stdout[-8000:])
    warnings = sorted(set(re.findall(r"^(\S+:\d+:\d+: warning: .*)$", log.stdout, re.MULTILINE)))
    print(f"compiler warnings in this build: {len(warnings)}")
    (out / "warnings.txt").write_text("".join(w + "\n" for w in warnings))
    return build


def inside(args) -> None:
    src, out, dist = Path("/src"), Path("/out"), Path("/dist")
    build = configure_and_build(args.version, out)
    if args.build_only:
        return
    stage = out / "stage"
    shutil.rmtree(stage, ignore_errors=True)
    run("cmake", "--install", build, "--prefix", stage, "--strip")
    bin_dir = stage / "bin"
    shutil.copyfile("/opt/w64/innoextract/innoextract.exe", bin_dir / "innoextract.exe")
    shutil.copyfile("/opt/w64/bin/openssl.exe", bin_dir / "openssl.exe")  # launcher hosting: development CA and leaf
    for category, names in QT_PLUGINS.items():
        (bin_dir / category).mkdir()
        for name in names:
            shutil.copyfile(QT / "plugins" / category / name, bin_dir / category / name)
    bundled, system = close_dependencies(bin_dir)
    if any(p.name.lower() == "vulkan-1.dll" for p in bin_dir.iterdir()):
        raise SystemExit("vulkan-1.dll must come from the GPU driver, not the package")
    pg = bin_dir / "pgsql"
    (pg / "bin").mkdir(parents=True)
    for tool in PG_TOOLS:
        shutil.copyfile(PGSQL / "bin" / tool, pg / "bin" / tool)
    shutil.copytree(PGSQL / "lib", pg / "lib", ignore=lambda d, names: [n for n in names if not n.lower().endswith(".dll") or PG_LIB_SKIP.search(n)])
    shutil.copytree(PGSQL / "share", pg / "share", ignore=shutil.ignore_patterns("doc"))
    pg_bundled, pg_system = close_dependencies(pg / "bin", sorted((pg / "bin").iterdir()) + sorted((pg / "lib").glob("*.dll")), PG_SEARCH)
    system = sorted(set(system) | set(pg_system))

    licenses = stage / "share/licenses"
    shutil.copytree("/opt/w64/share/licenses", licenses, dirs_exist_ok=True)
    shutil.copytree("/opt/w64/innoextract/license", licenses / "innoextract")
    shutil.copyfile(src / "packaging/windows/NOTICE.txt", licenses / "NOTICE.txt")
    (licenses / "stb_vorbis").mkdir()  # public domain / MIT (text at the end of stb_vorbis.c, quoted in the provenance file)
    shutil.copyfile(src / "third_party/stb_vorbis/IKV-PROVENANCE.txt", licenses / "stb_vorbis/IKV-PROVENANCE.txt")
    for package in ("gcc-mingw-w64-x86-64-posix-runtime", "mingw-w64-common"):  # libstdc++/libgcc (GCC RLE), winpthreads
        shutil.copyfile(Path(f"/usr/share/doc/{package}/copyright").resolve(), licenses / f"{package}.copyright")
    shutil.copyfile(src / "packaging/windows/README.txt", stage / "README.txt")

    report = {"version": args.version, "asset_free": True,
              "compiler_warnings": (out / "warnings.txt").read_text().splitlines(),
              "bundled_dlls": bundled, "pgsql_bundled_dlls": pg_bundled, "system_dlls": system,
              "components": {"toolchain": run("x86_64-w64-mingw32-g++-posix", "--version").splitlines()[0],
                             "qt": "6.11.3 win64_mingw (official prebuilt qtbase)", "openssl": "3.5.9 (static; openssl.exe for hosting)",
                             "sdl3": "3.4.18 (static)", "freetype": "2.14.3 (static)", "libjpeg-turbo": "3.2.0 (static)",
                             "zlib": "1.3.2 (static)", "vulkan": "headers/loader import lib 1.4.363; vulkan-1.dll from the driver",
                             "innoextract": "1.9 signed upstream Windows build",
                             "postgresql": "EDB 16.15-2 windows-x64 binaries (bin/pgsql: " + ", ".join(PG_TOOLS) + ")",
                             "vcredist": "Microsoft Visual C++ 2015-2022 x64 runtime (app-local, from VC_redist.x64.exe sha256 cc0ff0eb…713b)"},
              "debian_packages": Path("/opt/ikv-baseline/installed-packages.tsv").read_text().splitlines(),
              "inventory": {str(p.relative_to(stage)): {"size": p.stat().st_size, "sha256": sha256(p)}
                            for p in sorted(stage.rglob("*")) if p.is_file()}}

    base = f"IKV-Windows-{args.version}-x64"
    archive = dist / f"{base}.zip"
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9, strict_timestamps=False) as z:  # ADDed licence files: epoch mtime
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                z.write(path, f"IKV-{args.version}/{path.relative_to(stage)}")
    # Uninstall exactly what was staged (the install folder may be user-chosen and hold unrelated bin/ or share/):
    # files by name, then directories deepest first, non-recursively so only directories left empty disappear.
    files = sorted(str(p.relative_to(stage)).replace("/", "\\") for p in stage.rglob("*") if p.is_file())
    dirs = sorted((str(p.relative_to(stage)).replace("/", "\\") for p in stage.rglob("*") if p.is_dir()),
                  key=lambda d: (-d.count("\\"), d))
    (out / "uninstall.nsh").write_text("".join(f'Delete "$INSTDIR\\{f}"\n' for f in files) +
                                       "".join(f'RMDir "$INSTDIR\\{d}"\n' for d in dirs), encoding="utf-8")
    setup = dist / f"{base}-Setup.exe"
    run("makensis", "-V2", "-INPUTCHARSET", "UTF8", f"-DVERSION={args.version}", f"-DSTAGE={stage}",
        f"-DUNINSTALL_LIST={out / 'uninstall.nsh'}", f"-DOUTFILE={setup}", src / "packaging/windows/ikv.nsi")
    for artifact in (setup, archive):
        digest = sha256(artifact)
        (dist / (artifact.name + ".sha256")).write_text(f"{digest}  {artifact.name}\n")
        report[artifact.suffix[1:]] = {"file": artifact.name, "size": artifact.stat().st_size, "sha256": digest}
        print(f"{artifact}: {artifact.stat().st_size} bytes sha256 {digest}")
    (dist / f"{base}.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


def host(args) -> None:
    engine = shutil.which("podman") or shutil.which("docker")
    if not engine:
        raise SystemExit("docker or podman is required")
    work, dist = args.work.absolute(), args.dist.absolute()
    work.mkdir(parents=True, exist_ok=True)
    dist.mkdir(parents=True, exist_ok=True)
    subprocess.run([engine, "build", "-t", IMAGE, ROOT / "tools/windows"], check=True)
    subprocess.run([engine, "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp",
                    "-v", f"{ROOT}:/src:ro", "-v", f"{work}:/out", "-v", f"{dist}:/dist", IMAGE,
                    "python3", "/src/tools/package_windows.py", "--inside", args.version]
                   + (["--build-only"] if args.build_only else []), check=True)


def self_check() -> None:
    assert system_dll("KERNEL32.dll") and system_dll("api-ms-win-crt-runtime-l1-1-0.dll") and system_dll("vulkan-1.dll")
    assert system_dll("OPENGL32.dll") and system_dll("WS2_32.dll")
    assert not system_dll("libstdc++-6.dll") and not system_dll("Qt6Core.dll") and not system_dll("libwinpthread-1.dll")
    assert system_dll("WLDAP32.dll") and not system_dll("VCRUNTIME140.dll") and not system_dll("LIBPQ.dll")
    assert PG_LIB_SKIP.search("plperl.dll") and PG_LIB_SKIP.search("jsonb_plpython3.dll") and not PG_LIB_SKIP.search("plpgsql.dll")
    print("package_windows self-check passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?", default=VERSION)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--work", type=Path, default=ROOT / "build-windows")
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        raise SystemExit("version must be X.Y.Z")
    self_check() if args.self_check else inside(args) if args.inside else host(args)
