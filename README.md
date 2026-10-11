# İstanbul Kıyamet Vakti — independent native preservation project

Native C++20 Linux client, HTTPS accounts/private administration, authoritative TLS world, PostgreSQL persistence, and Qt6 launcher with signed updates. **This is an independent implementation, not the original server or proof of original-game interoperability.** The complete requirements remain in [Goal](Goal). Unknown gameplay rules are explicitly **NONCANONICAL** in `config/gameplay.json`; original framing research remains separate from the authenticated custom v2 wire.

Use only legitimately supplied local assets and owned/authorized services. No Wine/Proton, original credentials, official-server probes or proprietary server code are required. Installer, extracted originals, analysis projects, credentials and backups are excluded from source/container distribution. [Progress/evidence](docs/PROGRESS.md) distinguishes historical proof, implemented source and current unverified behavior.

## Build and install

Linux/POSIX, CMake 3.20+, C++20 compiler, Threads, OpenSSL **3.2+ with the ARGON2ID KDF provider**, Boost **1.74+** Beast/Asio headers, nlohmann JSON **3.9+**, PostgreSQL/libpq and Zlib are required. The native client additionally requires SDL3 **3.4+**, OpenGL, FreeType, Fontconfig development files and pkg-config. Its optional Vulkan backend is enabled by default (`IKV_BUILD_VULKAN=ON`) and additionally requires Vulkan development files/loader and `glslc`; an OpenGL-only build requires neither the Vulkan SDK/loader nor a shader compiler. The launcher independently requires Qt6 **6.2+** Core/Widgets/Network and libarchive. `IKV_BUILD_CLIENT=OFF` skips all client dependency discovery, including Vulkan and Fontconfig. Original Ogg decoding uses the installed native `ffmpeg`; importing an Inno installer uses configured native `innoextract`. No dependency or proprietary asset is downloaded by CMake.

`BUILD_TESTING=ON` requires Python3 **3.9+**; launcher-enabled testing also needs the real `openssl` executable (signed-update fixture keys and TLS). Deployment/release builds with `BUILD_TESTING=OFF` do not acquire these test-only requirements.

The launcher's "Kurulum dosyasını seç (istanbulkur6_0.exe)" path needs the native ELF `innoextract`; it finds `.tools/innoextract-1.9-linux/bin/amd64/innoextract`, one beside the launcher or one on `PATH` automatically (not the distribution's top-level shell wrapper) and asks only if none exists.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

To build the genuine OpenGL client without Vulkan:

```sh
cmake -S . -B build-gl-only -DCMAKE_BUILD_TYPE=Debug -DIKV_BUILD_VULKAN=OFF
cmake --build build-gl-only --parallel
ctest --test-dir build-gl-only --output-on-failure
cmake --install build-gl-only --prefix "$HOME/.local"
```

`IKV_BUILD_VULKAN=OFF` omits the Vulkan renderer, hardware ray tracing and all Vulkan shader compilation/installation. `--renderer vulkan` is unavailable in that build and fails explicitly; it does not substitute OpenGL while reporting Vulkan. The default build retains both genuine backends and all ten compiled Vulkan SPIR-V shaders.

For playing, build an optimized client; Debug builds are several times slower (HUD rasterization, CPU skinning and per-frame scene traversal run unoptimized):

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-release --parallel --target ikv-client ikv-server ikv-auth
```

Only the play targets are listed because some test tools (`audio-asset-test`) require assertions and refuse `NDEBUG`. Rebuild client and services from the same source revision; the custom wire changes with it.

Targets: `ikv-auth`, `ikv-server`, `ikv-client`, `ikv-launcher`, `ikv-updater`, `ikv-assets`, `ikv-original-inspect`. Installed binaries go to `bin/`; independently implemented config, migrations, dashboard and, when enabled, compiled Vulkan shaders go to `share/ikv/`. Licensed original assets are never installed by this command. Client and launcher can be disabled independently with `IKV_BUILD_CLIENT=OFF` and `IKV_BUILD_LAUNCHER=OFF`; server-only [deployment](docs/SELF_HOSTING.md) also disables offline tools.

### Portable AppImage

`python3 tools/package_appimage.py` builds `dist/IKV-Linux-0.4.0-x86_64.AppImage` plus `.sha256` and a `.json` inventory/licence report. It needs only `docker` or `podman` (plus network on first run): it builds the pinned Ubuntu 24.04 toolchain image from `tools/appimage/Dockerfile.ubuntu24` (OpenSSL 3.5.9 with ARGON2ID, SDL 3.4.18, libjpeg-turbo 3.2.0, Vulkan-Headers 1.4.363, all SHA-256 pinned), compiles every target inside it with the source tree mounted read-only (work tree `build-appimage/`), and assembles a Type-2 image on the hash-pinned uruntime v0.8.1 runtime, as the sibling Kabus 22 AppImage does. `python3 tools/package_appimage.py --self-check` checks the library policy.

The AppImage starts `ikv-launcher`; `IKV-Linux-….AppImage ikv-client|ikv-auth|ikv-server|ikv-updater|ikv-assets|ikv-original-inspect|innoextract …` runs the bundled tool directly. It bundles Qt 6 (xcb and Wayland platform plugins), libstdc++, libpq, the signed upstream innoextract 1.9 helper and the PostgreSQL 16 server tools used by **Sunucu Kur** (`usr/lib/postgresql/16/bin`, put first on `PATH` by AppRun). It requires glibc 2.38+ and the host's GPU drivers and loaders (libGL/libEGL via libglvnd, libvulkan and the Mesa or vendor ICDs), X11/xcb or Wayland client libraries, fontconfig, FreeType and HarfBuzz, and the time-zone database. Without FUSE it falls back to extracting itself. No original game files are included: the launcher imports them from your own installer. Licences and the source offer are in `usr/share/licenses/ikv/` (`packaging/NOTICE.txt`).

Releases: `python3 tools/release.py X.Y.Z` bumps the version (CMake `project()` is the single source; `package_appimage.py --version/--work/--dist` override it for test images), builds and smokes the AppImage, packs `dist/ikv-linux-x86_64-X.Y.Z.tar.gz` (only `bin/ikv-linux.AppImage`, deterministic) and signs `dist/manifest.json` + `manifest.sig` (channel `experimental`) with `~/.local/share/ikv-dev/secrets/release-signing.pem`, proving it with the image's own `ikv-updater install` against `packaging/ikv-release-public.pem`; the next sequence comes from `release-sequence.json` beside the key and is written back only after that check. `--publish --notes FILE` then commits, pushes `main`, creates the `vX.Y.Z` GitHub prerelease with all six assets, downloads them back and re-verifies them.

## Owned disposable service check

The runnable integration suite uses its own Unixsocket-only PostgreSQL cluster, synthetic accounts, local TLS CA/SAN certificates and private temporary DSN files. It starts the actual native services and clients and cleans them up, rather than using anonymous login or mocked state:

```sh
python3 tests/local_multiplayer.py --auth build/ikv-auth --server build/ikv-server \
  --client build/ikv-client --source .
python3 tests/client_eof.py --auth build/ikv-auth --server build/ikv-server \
  --client build/ikv-client --source .
```

Requires `initdb`, `pg_ctl`, `psql`, `pg_dump`, `pg_restore`, `openssl`, Linux `ss` (iproute2), owned-process `/proc` visibility and Python3, run as a non-root user. These are commands to reproduce acceptance, **not claims they have passed**. [Testing](docs/network/NETWORK_TESTING.md) describes coverage and the reusable fixture for actual graphical, launcher and load smokes.

For legitimate original-font CPU bitmap/UTF8/kerning acceptance, build `client_font_diagnostic` and run `build/client_font_diagnostic /owned/assets/fonts/verdana.ttf /owned/assets/fonts/sobee.ttf /owned/new-raster-output`; the first font is human text, the second explicitly icon glyphs, and the optional output directory receives labelled actual16/32pixel PPMs. Supplied human faces and configured native Fontconfig fonts provide fallback coverage. This prerequisite-dependent diagnostic is not unattended CTest or graphical/GPU frame proof.

## Native services and client

Create an independently owned PostgreSQL database and private mode0600 DSN file; use your own TLS certificate/private key and explicit CA, with the selected DNS/IP names in certificate SANs. See [authentication/database](docs/AUTH_DATABASE.md) and [self-hosting](docs/SELF_HOSTING.md) for bootstrap, backups and exact deployment precautions.

```sh
build/ikv-auth --bind 127.0.0.1 --port 7443 --admin-bind 127.0.0.1 --admin-port 7444 \
  --cert /private/tls-cert --key /private/tls-key --dsn-file /private/dsn \
  --migrations database/migrations --web-root web --backup-dir /private/backups
build/ikv-server --bind 127.0.0.1 --port 7000 --cert /private/tls-cert \
  --key /private/tls-key --dsn-file /private/dsn --gameplay config/gameplay.json \
  --asset-root /owned/import/assets
```

Run `build/ikv-launcher` (or the installed `ikv-launcher`, or the AppImage): on first run it shows **Oyunu Kur** (pick the original installer or an existing installation folder; files go to `~/.local/share/IKVCommunity/IKVLauncher`), then a login page (server dropdown with a **Localhost** preset and **Sunucu ekle**: name, `host[:port]` of the HTTPS account service, optional CA file), then the character list with **Yeni Karakter** and **OYNA**. OYNA runs the `ikv-client` bundled beside the launcher; the world port comes from `/v1/status`. Graphics and everything advanced (updates, verify/repair, mappings, diagnostics) are under **⚙ Ayarlar**; see [launcher and updates](docs/LAUNCHER_UPDATES.md). **The launcher updates itself:** on start it checks the `sergen213/ikv-linux` GitHub prereleases and offers **Güncelleme mevcut: vX.Y.Z → Güncelle**, which downloads the release's signed AppImage package, verifies it against the compiled-in release key (`packaging/ikv-release-public.pem`), installs it beside the imported files and restarts into it; later starts of an older AppImage hand off to the newer installed one, and **Önceki sürüme dön** rolls back. Repository, channel and trust key can be overridden in Ayarlar → Gelişmiş. The player listener never serves `/admin` or `/v1/admin`; the separate management listener remains private and additionally requires an administrator session.

To host your own server from the launcher, use the header **server chip** (**Sunucu Kur**): a short form (name, administrator account and password, bot count, local or LAN) creates and starts a private local server and logs the launcher in to it. The same chip then shows its live state and opens **Sunucu Yönetimi**, a native management panel (status, start/stop/restart, live bots, announcements, kick/ban, accounts, rates, backups/restore, log) that talks to the server's private management API over its own local CA, with no browser setup. The server can optionally keep running after the launcher closes and start with the launcher; see [self-hosting](docs/SELF_HOSTING.md#launcher-hosted-local-server-sunucu-kur--sunucu-yönetimi).

The client accepts a session only through stdin, never argv. After obtaining a session and owned character from your configured service, send the token using a private input stream:

```sh
build/ikv-client --host localhost --port 7000 --ca /private/tls-ca --session-stdin \
  --character 1 --asset-root /owned/import/assets --renderer opengl
```

Use `--renderer vulkan` for the real alternative backend. `--headless --ticks N --commands FILE` provides authenticated deterministic network automation; command arrays contain `{ "tick": N, "action": {...} }` or `{ "tick": N, "destination": {"x": 100, "y": 0} }`. World coordinates are protocol millimetres. Exact action/state schemas are in [custom protocol](docs/network/CUSTOM_PROTOCOL.md). Credentials/passwords must not be put in shell history, argv, URLs or public logs.

Vulkan RT defaults to `--rt-style faithful`: in any appearance it keeps the original lighting equation (vertex light, lightmaps, fog, day/night record) and only adds deterministic Sun-ray shadows cast by actors (`docs/GRAPHICS_ROADMAP.md`, `analysis/porting-wave12/RtLighting/report.md`). Physical RT (`--appearance enhanced --rt required --rt-style physical`) denoises only the stochastic indirect-diffuse and reflection terms. The deterministic Sun shadow ray and the textured artwork stay unfiltered. History reprojects through the camera for static scenery and through motion vectors for animated actors, with a per-pixel history length. Three edge-aware à-trous passes then run every frame, so disoccluded pixels behind moving actors and at the screen edges draw on their converged neighbours. Physical-GPU measurements: [frozen camera](analysis/porting-wave10/RtNoise/report.md), [noise in motion](analysis/porting-wave10/RtMotion/report.md).

Graphical scene/support/actor selection follows admitted authenticated `source_registry_id`, not local scene or avatar-fallback switches. The recovered 71-row SourceRegistry maps to 18 support datasets; server startup with the supplied source-associated gameplay config requires original `--asset-root` and caches grids by support ID. Default PCs use genuine selected BODY+DRESS/BIP assemblies. Native classes1..3 and sex0..1 map to source visual classes2..7 via an explicitly **NONCANONICAL** adapter; this is not historical class equivalence. Optional `--model-map FILE` accepts only explicit positive uint32 `id:N` member mappings (for example `{"id:1":"objects/npc/AGAH.ADF"}`), not role aliases or a global fallback. `--render-scale .25..1` is Vulkan-only; OpenGL requires 1. See [client porting](docs/CLIENT_PORTING.md) for the current CLI and source/runtime evidence boundaries.

**Graphics: FSR 3.1.4 upscaling and frame generation.** With `--renderer vulkan`, `--upscaler fsr3 --fsr-mode native|quality|balanced|performance|ultra_performance --fsr-sharpness 0..1` (or Ayarlar → Ölçekleme, or the launcher's Grafik tab) uses AMD FidelityFX Super Resolution 3.1.4 temporal upscaling with motion vectors and a reactive mask, in original/enhanced raster and with ray tracing; the HUD stays at display resolution. On an RX 7900 XTX at 2560×1366 with required RT, Quality/Performance cut GPU frame time from ~20 ms to ~12/~9.5 ms. `--frame-generation on` (off by default; Ayarlar → Kare üretimi, launcher Grafik → Kare üretimi; needs an FSR mode) adds FSR 3.1.4 frame generation (SDK optical flow + frame interpolation, IKV-owned present thread and pacing, HUD composited, never interpolated): one generated frame between rendered frames, roughly doubling displayed frames in GPU-bound RT at the cost of about half a frame of latency. Details, measurements and limits: [graphics](docs/GRAPHICS_ROADMAP.md#fsr-314-temporal-upscaling-vulkan); vendored SDK provenance and local patches: `third_party/FidelityFX-SDK/IKV-PROVENANCE.txt`.

Source0's genuinely absent registered particles produce a source-backed nonfatal attempt diagnostic, not fabricated particles. Independent ordinary registration **union** source virtual child reachability determines color invocation; material draw eligibility remains separate, including clock/RNG advancement for hidden participating Ocean/isik callbacks and range errors for invalid excluded IDs. Original ENV/fog10..210 and Ocean textures/UV/waves/normals are preserved, not brightened to mask dark source lighting. Source1's private spawn[-109500,0] (source[-109.5,0]) has retained nine-cell/+ .75 margin, ten-ray and conservative standing-box SAT evidence; ordinary Shop geometry remains unchanged and no original rendered-floor guarantee follows.

Original borrowed atlas 24-cell inventory art and 15 source skill/rank definitions are distinct from the native 32-slot bag and five independent abilities. Help Book/Page text (first Book `Chat`) and `Help_CharacterSelection.dds` image navigation are separate namespaces, neither a Source9 selector. Registry9 is Er Meydanı4/support6; native UI mode9 is character information. Default help-image inspection and wire-driven gear captures do not prove help navigation or UI equip/unequip.

## Offline original-data research

These tools do not contact any service and do not establish original authentication/gameplay compatibility:

```sh
build/ikv-assets list original/app/base/textures_4.DAT
build/ikv-assets inspect original/app/base/textures_4.DAT textures/045marbretile256.dds
build/ikv-original-inspect --client-commands /owned/offline-stream.bin
```

SKYF/archive/DDS/ADF facts belong in [asset formats](docs/ASSET_FORMATS.md); original LE32 stream/envelope facts belong in [protocol specification](docs/network/PROTOCOL_SPECIFICATION.md). The original-wire tool's 65535-byte ceiling is a local defensive subset, not the recovered original maximum. Preserve originals unmodified and do not redistribute them.

## Documentation and evidence

- [Self-hosting, Compose and systemd](docs/SELF_HOSTING.md)
- [Authentication/database administration](docs/AUTH_DATABASE.md)
- [Server architecture](docs/SERVER_ARCHITECTURE.md), [client porting](docs/CLIENT_PORTING.md)
- [Graphics evidence and remaining targets](docs/GRAPHICS_ROADMAP.md)
- [Engine architecture](docs/ENGINE_ARCHITECTURE.md), [function map](docs/FUNCTION_MAP.md)
- [Reverse engineering](docs/REVERSE_ENGINEERING.md), [known uncertainties](docs/KNOWN_ISSUES.md)
- [Test commands and evidence limits](docs/network/NETWORK_TESTING.md), [progress ledger](docs/PROGRESS.md)

Historical tests of the removed anonymous v1 harness do not verify this cutover. Source availability, build success, synthetic gameplay tests and screenshots have different evidentiary scope; none alone proves complete original reconstruction, Vulkan/RT hardware use or safe internet deployment.

### Current acceptance scope

These rows summarize integration-owner observations, not checks executed while editing this README. The current independent native implementation's integration acceptance is complete within the documented **NONCANONICAL** scope: three build configurations each passed14 tests (42 test runs), current signed packages and raw/guided hardwareRT long paths passed, with retained Source/UI/deployment evidence scoped below. This is not original Windows interoperability or complete original visual/mechanical reconstruction.

| Area | Observed acceptance / evidence limits |
| --- | --- |
| Native source CPU/terminal checks | `analysis/scene-assembly-proof9.json`: six source PCs' color-eligible indexed geometry, owned assembly replacement and real-clock initial-dead terminal28 restoration; normal28/62/.25 blending and62 → 11/frame490 retained. This is CPU consumer evidence, not all retained geometry or a GPU image. |
| Native GL/Vulkan/requiredRT Source observations | Selected six-PC captures and all39 transition images inspected before the later RT-retirement-only cutover. Source0 gear/combat/skill deliveries, natural player death and persisted-dead corpse views; all nine Source1 transfer/movement/reentry views show the own PC. These remain evidence for their frozen client snapshot, not fresh long-path RT acceptance. Exact transient clip cursors, per-gear UI interaction, native consumed snapshot history and GPU ground/resource counters remain unproven. Original rendered-floor/full visual/mechanical parity is not claimed. |
| Native Source HUD/input | Current proof3 **PASS32.36s**, seven assertions, native exit0/leases0; parent inspected all17 PNGs plus contact sheets/crops (`analysis/source-native-input-proof3-current-ui/parent-visual-inspection.json`). Source0 native class1/sex1 → visual class3 upright, original Verdana Turkish text and actual atlas icon75/sword slot11; ground authority[-351,-1966], camera-only WASD, wheel15 → 12, two fullscreen transitions, resize and final saved windowed1024×768. Owned Xvfb/Openbox software rendering, not physical GL GPU proof. The blade was autoequipped, not a UI equip gesture; first Chat/MesajYollama text and separate CharacterSelection image were inspected without help navigation. Help clipping in window/fullscreen/resized/final views remains observed. |
| Ordinary invocation consumer | Actual144 GL/Vulkan fixture frames (12 variants × 6 frames × 2): excluded placements byte-exact neutral, hidden participating callbacks contribute, invalid excluded references reject. Synthetic consumer evidence, not original surface parity. Registered Source0 Ocean was separately inspected dark BLUE, GL/VK approximately RGB7,10,12 and requiredRT3,5,6 under source ENV. |
| Tests / overall integration | Current matrix **PASS42/42 across three configurations**: regular full14/14 PASS145.77s CTest/145.91s wall (`artifact://1012`), GL-only14/14 PASS145.72s/145.92s (`artifact://1013`), ASan/UBSan14/14 PASS192.20s/192.35s (`artifact://1009`). All used `--output-on-failure --parallel1`; source/tests/real binaries unchanged after RT retirement. Sanitizer leak detection/halt-on-error retained, no `LSAN_OPTIONS` override. Earlier13/14 TLS `graceful_stop` world-poll timeout remains historical/intermittent/unattributed; current passes do not prove a production fix, concurrency cause or post-STOP destructor/LSan diagnosis. All three builds and separate asset/math checks also passed (two checks per build, aggregate2.37s). |
| Source1 deployment | Fresh Compose plus owned-user-adapted systemd **PASS99.87s**, guarded78 inputs/three native binaries. Compose used the least-privilege application role; the systemd fixture bootstrapped PostgreSQL with a superuser. No current container custom-dump restore or unmodified privileged systemd proof is claimed. |
| Final signed install/Qt Play/raster provider transition | Current9c834718… package **PASS415.82s**, `analysis/final-launcher-source-vulkan-half-retirement-proof1/evidence.json`: both full/GL-only launcher capability flows, guarded seven ELF binaries/ten SPIR-V, actual canonical original root distinct from displayed parent, two-second native Source0 liveness/normal SIGTERM exit, all four Qt images inspected. Current post-GUI hashes/original inventory/userdata and owned cleanup guards retained. Earlier326.23s9fb… proof remains historical; the old EACCES cause remains unproven. |
| Final signed requiredRT child | Current9c834718… package **PASS240.60s**, `analysis/final-launcher-source-rt-half-retirement-proof1/evidence.json`: actual1280×720 Vulkan/.5/enhanced/required, unavailable-provider negative before explicitGL/1, same full payload and guarded seven-ELF/ten-SPIR-V package; all four Qt images inspected, post-GUI verification/current hashes/original inventory/userdata/owned cleanup retained. Earlier500.85s9fb… proof remains historical. Local ephemeral Ed25519 signing only, not public GitHub publication or production keys; queued presentation acceptance is not GPU-fence completion or temporal quality. |
| Current hardwareRT raw/guided long-path execution | Both scales **PASS** with four native exits0/no TLS failure and final authority[0,1500]/camera(15,.65,0): `analysis/rt-temporal-source-revive-half-proof1`210.51s, frames1559/1533/1840/1841; `analysis/rt-temporal-source-revive-full-proof1`193.73s, frames819/848/797/1276. Legal type8 resurrection before final motion addresses genuine death without fake HP/NPC disabling or deadline changes. Guided half eligible/retained1799 each/reset1; full597/1197/reset3, motion vectors0/cap32/RTbits1. Parent inspected all four images per scale; immutable raw manifests and separate visual-inspection companions retain distinct provenance. Homogeneous-world ROI luma variance: half32.7462 → .363423 and25.3687 → .385300; full52.53177033061073 → 1.0602859933821913 and51.97293040399927 → .6040032249771422. These are8-bit spatial proxies only (`measured_no_quality_threshold`), excluding UI/actors/edges/sky/gray background—not original parity, noise ground truth or convergence-quality acceptance. |

The implemented independent target and its current integration gates are accepted: all18 Goal completion criteria are completed within this explicit independent **NONCANONICAL** implementation scope, not invented historical equivalence. Source compatibility means native use of original assets with owned authenticated services—not the original Windows wire/server or full original-game parity. Exact UI gear-click/transient clip-cursor proof, original rendered-floor guarantees, external GitHub publication/production keys, container custom-dump restoration/unmodified privileged units, and noise-ground-truth quality are not claimed by these results. Historical SIGKILLs and intermittent sanitizer shutdown failure remain unattributed rather than retrospectively explained by passing runs.
