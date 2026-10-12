# Wave 17 — Windows build, LAN hosting (2026-10-12)

User ask: build a Windows version of the launcher, installer and client to test the game on Windows; make sure the
server serves the LAN, not only localhost. (Running alongside: MisirLights, interior lighting at night / all-zone
lighting sweep, analysis/porting-wave17/MisirLights/.)

## Facts
- The launcher already hosts a server (`launcher_server.cpp`) with a LAN option, but only when the server is first
  created: bind address and certificate SANs are fixed then. The user's hosted server (IKVLauncher/server/host.json) was
  created localhost-only (`bind 127.0.0.1`, SANs localhost/127.0.0.1). This machine's LAN address is 192.168.2.2 (enp19s0).
- Joining another server = launcher server profile: HTTPS auth origin + CA PEM file (blank = system trust).
- No Windows machine or VM here; Wine is installed (smoke only). No mingw toolchain on the host; docker is available
  (the AppImage is already built in a pinned container, tools/appimage/Dockerfile.ubuntu24).
- Linux-only code: client_session.cpp (BSD sockets, poll, pthread, SIGPIPE), audio.cpp (ffmpeg via vfork/execv for Ogg),
  client_ui.cpp (fontconfig), launcher_extraction.cpp (native ELF innoextract), launcher_main.cpp (execv hand-off,
  AppImage self-update), launcher_server.cpp (fork, PostgreSQL, ikv-auth/ikv-server hosting), FSR3 via
  third_party/ikv-ffx-posix. Top-level CMake requires PostgreSQL/Boost/OpenSSL for everything.

## Decisions (defaults; user can override)
- Windows scope: launcher (sign-in, server profiles, settings, original-asset import, play), client (GL, Vulkan, RT,
  FSR3 if the FidelityFX build works under MinGW, else documented), installer. **Hosting a server stays Linux-only**;
  the Windows launcher joins servers (e.g. this PC over LAN). Self-update on Windows: not in this wave (the update
  payload is the Linux AppImage); the Windows launcher does not offer updates.
- Toolchain: cross-compile on Linux in a pinned container (MinGW-w64 GCC, dependencies built from pinned sources or
  pinned prebuilt Qt for win64_mingw), like the AppImage. Output `dist/IKV-Windows-X.Y.Z-x64-Setup.exe` (NSIS) and a
  portable `.zip`. Unsigned (SmartScreen warning expected).
- LAN: an existing hosted server can be switched between local-only and LAN at any time (bind + certificate SANs
  reissued from the same CA, so existing clients keep trusting it). LAN mode listens on all IPv4 interfaces and the
  certificate covers every non-loopback IPv4 address plus the host name. The admin API stays 127.0.0.1. The host page
  exports the CA file for other PCs.

## Slices (parallel)
1. **Lan** — launcher_server.cpp (+ ikv-auth/ikv-server bind handling if needed): switchable LAN mode, CA export, apply
   to the user's hosted server, verify from another network namespace / LAN address.
2. **WinToolchain** — container + CMake: options to build client/launcher without server deps, Windows targets,
   dependency builds, packaging script (`tools/package_windows.py`), NSIS installer, Wine smoke.
3. **WinClient** — client portability: sockets/TLS transport (Winsock), audio Ogg decode without ffmpeg/fork, fonts
   without fontconfig, paths/settings dirs, any POSIX in client_main/scene/renderers; FSR3 under MinGW.
4. **WinLauncher** — launcher portability: no hosting/self-update on Windows, client hand-off (QProcess), original-asset
   import on Windows (installed game folder; installer extraction with innoextract.exe), settings paths.
