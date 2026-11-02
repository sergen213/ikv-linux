# İKV Linux

An independent native client, launcher and self-hostable server for the Turkish online game **İstanbul Kıyamet Vakti (İKV)**, using your own copy of the original game files.

## What it is

İKV Linux is a C++20 reimplementation, not the original game or server. It has four parts:

- **Client** (`ikv-client`): SDL3 client with an OpenGL renderer and an optional Vulkan renderer with hardware ray tracing, AMD FSR 3.1.4 upscaling and optional frame generation.
- **Launcher** (`ikv-launcher`): a Qt 6 launcher with a Turkish interface. It imports the original game files, manages accounts on community servers, starts the client and updates itself on Linux.
- **Server** (`ikv-auth`, `ikv-server`): an HTTPS account service with a private management interface, and an authoritative TLS world server, backed by PostgreSQL. The launcher can set up a private server for you (**Sunucu Kur**).
- **Tools** (`ikv-assets`, `ikv-original-inspect`): read-only research tools for the original data.

The client and server talk a new, independently designed protocol. They do **not** connect to the original servers and are not compatible with the original Windows client. Where the original gameplay rules are unknown, the server uses invented rules, marked **NONCANONICAL** in `config/gameplay.json`.

**Status:** experimental prerelease, before 1.0. See [Known issues](docs/KNOWN_ISSUES.md) and [Porting gaps](docs/PORTING_GAPS.md).

## You need the original game

No game data is included: no installer, textures, models, sounds or other original files. You need your own original installer (`istanbulkur6_0.exe`) or an existing installation; the launcher imports the files from it without running any Windows program. İstanbul Kıyamet Vakti and its media belong to their respective owners. This project is not affiliated with or endorsed by them. Do not use original or official account credentials with it.

## Download

Prereleases are on [GitHub Releases](https://github.com/sergen213/ikv-linux/releases).

- **Linux:** `IKV-Linux-<version>-x86_64.AppImage` (checksum in the matching `.sha256` file).

  ```sh
  sha256sum --check IKV-Linux-<version>-x86_64.AppImage.sha256
  chmod +x IKV-Linux-<version>-x86_64.AppImage
  ./IKV-Linux-<version>-x86_64.AppImage
  ```

  The launcher checks for new releases at start and offers **Güncelle**; **Önceki sürüme dön** rolls back.

- **Windows (x64):** `IKV-Windows-<version>-x64-Setup.exe` (installer) or `IKV-Windows-<version>-x64.zip` (portable). The executables are not code-signed, and the Windows build has only been tested under Wine. There is no self-update on Windows; install a newer Setup.exe over the old one.

## Requirements

Linux:

- x86-64 with **glibc 2.38 or newer** (for example Ubuntu 24.04, Debian 13, Fedora 39 or later).
- X11 or Wayland, and your system's GPU drivers (OpenGL, and the Vulkan loader for the Vulkan renderer).
- Ray tracing needs a Vulkan GPU and driver with ray-tracing support.

Windows:

- 64-bit Windows 10 or 11.
- A current GPU driver with Vulkan 1.2 and OpenGL 3.3. `vulkan-1.dll` comes from the driver and is needed even for OpenGL.

## Usage

1. Start the AppImage (or `ikv-launcher`). On first run, **Oyunu Kur** asks for your original installer (**Kurulum dosyasını seç (istanbulkur6_0.exe)**) or an existing installation folder (**Mevcut kurulum klasörünü seç**). On Linux the files go to `~/.local/share/IKVCommunity/IKVLauncher`.
2. Pick a server and log in, or register a new account on it (**Kayıt Ol**). A **Localhost** profile is preset; **Sunucu ekle** adds another server by `host[:port]` of its account service (default port 7443) and an optional CA file.
3. Press **OYNA**. The client opens the character selection and creation screens.

Graphics, updates, verify/repair and diagnostics are under **⚙ Ayarlar**. See [Launcher and updates](docs/LAUNCHER_UPDATES.md).

**Host your own server:** the launcher's header server chip (**Sunucu Kur**) creates and starts a private local or LAN server with your administrator account, then opens **Sunucu Yönetimi** for status, accounts, announcements, backups and logs. To run the services yourself (Docker Compose or systemd), see [Self-hosting](docs/SELF_HOSTING.md).

Useful client options (the launcher passes these for you): `--renderer opengl|vulkan`, `--upscaler fsr3 --fsr-mode native|quality|balanced|performance|ultra_performance`, `--frame-generation on` (needs an FSR mode). Run `ikv-client --help` for the full list.

## Build from source

You need CMake 3.20+, a C++20 compiler, OpenSSL 3.2+ with the ARGON2ID KDF provider, Boost 1.74+ headers, nlohmann JSON 3.9+, PostgreSQL/libpq and Zlib. The client also needs SDL3 3.4+, OpenGL, FreeType, Fontconfig and libjpeg-turbo; the Vulkan renderer needs Vulkan development files and `glslc`; the launcher needs Qt 6.2+ and libarchive.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-release --parallel --target ikv-client ikv-server ikv-auth ikv-launcher
```

Add `-DIKV_BUILD_VULKAN=OFF` for an OpenGL-only client without the Vulkan dependencies. The full dependency list, tests, AppImage and Windows packaging are in [Development](docs/DEVELOPMENT.md).

## Documentation

For players and server operators:

- [Launcher and updates](docs/LAUNCHER_UPDATES.md) — launcher pages, settings, signed updates and rollback.
- [Self-hosting](docs/SELF_HOSTING.md) — server build, Docker Compose, systemd, backups.
- [Accounts and administration](docs/AUTH_DATABASE.md) — accounts, HTTPS and the management interface.
- [Graphics](docs/GRAPHICS_ROADMAP.md) — renderers, ray tracing, FSR and their limits.
- [Known issues](docs/KNOWN_ISSUES.md) — open problems and uncertainties.

For contributors:

- [Development](docs/DEVELOPMENT.md) — dependencies, build options, tests, packaging and releases.
- [Progress](docs/PROGRESS.md) and [porting gaps](docs/PORTING_GAPS.md).
- [Client](docs/CLIENT_PORTING.md), [server architecture](docs/SERVER_ARCHITECTURE.md), [engine architecture](docs/ENGINE_ARCHITECTURE.md), [database schema](docs/DATABASE_SCHEMA.md).
- [Custom protocol](docs/network/CUSTOM_PROTOCOL.md), [message state machine](docs/network/MESSAGE_STATE_MACHINE.md), [network testing](docs/network/NETWORK_TESTING.md).
- Original game research: [reverse engineering](docs/REVERSE_ENGINEERING.md), [asset formats](docs/ASSET_FORMATS.md), [function map](docs/FUNCTION_MAP.md), [protocol specification](docs/network/PROTOCOL_SPECIFICATION.md), [packet catalog](docs/network/PACKET_CATALOG.md), [endpoint discovery](docs/network/ENDPOINT_DISCOVERY.md).

## Contributing

Changes are accepted only as pull requests to this repository, reviewed and merged by the maintainer. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

The project's own code is © sergen213, all rights reserved, and source-available under [LICENSE](LICENSE): you may read it, build it for personal use and fork it only to send pull requests. Redistribution, published modified versions and commercial use need written permission. Versions published before this license keep their earlier license. Third-party components (see [packaging/NOTICE.txt](packaging/NOTICE.txt) and `third_party/`) keep their own licenses. Nothing here grants rights to İstanbul Kıyamet Vakti or its original files.
