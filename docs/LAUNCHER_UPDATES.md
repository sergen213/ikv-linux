# Native launcher and signed updates

The Qt 6 launcher manages the independently implemented native client and your own community-server accounts. It does not use official accounts, discover official servers, run Windows executables, or distribute original copyrighted game assets. Import only local content you are authorized to use; retain the source installation/installer.

## Using the launcher

Start `bin/ikv-launcher` (Qt Wayland or X11). The main window is one page; everything technical lives behind **⚙ Ayarlar**.

1. **Oyunu Kur** (first run, shown while no imported original files exist). Choose **Kurulum dosyasını seç (istanbulkur6_0.exe)** for the supported Inno Setup 6.0 installer, or **Mevcut kurulum klasörünü seç** for an existing authorized installation. The installer is never executed: only SHA-256 `34bb864018ad0c46a06a26d8bf0b4c48cd4bcf4c8db971c64b2ebda1b6fbdca7` is accepted and it is unpacked by the native ELF `innoextract`, found automatically (the configured path in Ayarlar → Gelişmiş, beside the launcher, the source tree's `.tools/innoextract-1.9-linux/bin/amd64/innoextract`, then `PATH`); the launcher asks for it only when none is found. Shell wrappers and Wine are rejected. Files are imported into the installation folder, by default Qt's `AppLocalDataLocation` (`~/.local/share/IKVCommunity/IKVLauncher`). Original sources are never edited; installer extraction is retained under `<install-root>/sources/<unique-id>` for later repair. On success the login page appears.
2. **Giriş**: pick a server, enter **Kullanıcı adı** / **Parola**, then **Giriş Yap** (or **Kayıt Ol** for a new private account; never official İKV credentials). A **Localhost** profile (`https://localhost:7443`) is preset. **Sunucu ekle** adds or edits a profile: a name, the account service address as `host` or `host:port` (default port 7443), and an optional CA certificate file (blank = the system trust store). The world server is the same host; its port is read from the account service's public `/v1/status` (`world.world_port`, default 7000) when you press OYNA. There is no certificate-verification bypass and no implicit official endpoint. **Parola sıfırla** takes an administrator-issued single-use token; the new password goes in the Parola field.
3. **OYNA**: after login the page names the account and offers **OYNA** and **Çıkış** (logout). There is no launcher character list: OYNA starts the client with `--auth <account origin>` and the session on stdin, and the client shows the original 3D character selection/creation screens (Yeni Karakter / Karakteri Sil / Başlat, plus native Çıkış to close the client without logout) itself, calling the same HTTPS `/v1/characters` API with the same CA ([CLIENT_PORTING](CLIENT_PORTING.md#in-client-character-selection-and-creation)). Passwords and session tokens are never persisted; the client receives its session on stdin, not in command-line arguments.
4. **Sunucu Kur / Sunucu Yönetimi** (the header **server chip**, which also shows the live state, e.g. `● Sunucu: Çalışıyor · 3 oyuncu`; also offered by the install page's opt-in **Bu bilgisayarda yerel sunucu da kur** checkbox after a successful import). **Guided creation**: İsim, administrator account and password (no default), bot count, Yerel/LAN and one button; progress steps are shown, errors are explained in Turkish, ports that are in use move to the next free port, and afterwards the launcher logs in to the **Localhost** profile automatically. It provisions an owned development CA/secrets (0600), a Unix-socket-only PostgreSQL cluster in AppLocalData and `ikv-auth` + `ikv-server` under a detached supervisor (`ikv-launcher --host-supervisor`). **Sunucu Yönetimi** is a native panel over the private management API pinned to that CA (no browser, no CA import): live status (uptime, players, bots, zones, tick, memory), start/stop/restart, live bot count, announcements, kick/ban, accounts with role/ban, rates and open zones, backups (create, list, restore with confirmation) and the server log. **Başlatıcı kapanınca da çalışsın** keeps the server running after the launcher closes (the next launcher reattaches); **Başlatıcı açılınca yerel sunucuyu otomatik başlat** starts it with the launcher. **Yönetim paneli (tarayıcı, gelişmiş)** remains as an advanced option, and **Sunucuyu sil** deletes the server after confirmation. Details and limits: [self-hosting](SELF_HOSTING.md#launcher-hosted-local-server-sunucu-kur--sunucu-yönetimi).

**Original look.** Once original files are imported, the launcher takes the original Windows launcher's appearance: it reads the skin bitmaps (RT_BITMAP resources: window frame, OYNA, OYUN REHBERİ, AYARLAR, minimize/close) at runtime from the user's own `launcher.exe`, located beside the selected source (`assets/source` itself, its `app/`, or the parent of a selected `base/`; installer imports retain it under `<install-root>/sources/`), with Verdana from the imported `fonts/`. The bitmaps are parsed natively with strict bounds checks and are never copied, bundled or redistributed. The same widgets are laid into the artwork's panels (pages top right, status/progress in the middle, server choice left, OYNA = the page's Giriş/OYNA action, activity log and **Sunucu Kur** at the bottom); **OYUN REHBERİ** opens the original `ikv_oyun_rehberi.pdf`. Before the first import, or if `launcher.exe` is missing or malformed, the plain window is shown.

**OYNA** always starts the `ikv-client` bundled beside the running launcher (inside the AppImage, or the build/install directory). After a startup handoff (below) that is the newest installed release's client. A blank server CA passes the system bundle (OpenSSL default, `SSL_CERT_FILE` honoured) to the client.

**Ayarlar → Grafik** persists renderer, Vulkan render scale, appearance (`original`/`enhanced`), ray tracing (`off`/`auto`/`required`, hardware-dependent; selecting it is not evidence of support), RT lighting style (`faithful` original lighting + actor shadows, default / `physical`), resolution, fullscreen and volume. **Ayarlar → Gelişmiş** holds the installation folder, asset verify/repair, signed package install, update repository/channel/trust/CA/API, update and rollback, optional original-content mappings and the diagnostic log (**Tanılama kaydet** exports it; review before sharing).

Vulkan is an optional native build capability: `IKV_BUILD_VULKAN=ON` (the default) retains both renderer choices and the hardware-dependent Vulkan ray-tracing provider. With `IKV_BUILD_VULKAN=OFF`, the launcher consumes `IKV_HAS_VULKAN=0` and offers only the genuine `opengl` renderer; this build needs neither the Vulkan SDK/loader nor `glslc`. A previously saved Vulkan selection remains unavailable rather than silently changing to OpenGL: **OYNA** reports the unavailable renderer until you explicitly select `opengl` in **Ayarlar → Grafik**. Saving unrelated settings does not replace that stored renderer. OpenGL cannot satisfy `required` ray tracing, and Play reports that incompatibility instead of claiming ray-tracing support. These compile-time choices do not establish GPU capability.

The optional mappings in Gelişmiş — **Model eşlemesi** (operator `id:N` → original member JSON), **Ses klasörü** (relative to the imported assets root) and **Ses olayı eşlemesi** — are passed to the client as `--model-map`, `--audio` and `--sound-map`. They remain explicit **NONCANONICAL** selections unless independently supported by recovered evidence.

## Asset verification and source repair

The original installation/installer is an import source, not an update artifact. Import resolves a selected installation's `base`, `app/base`, `app`, or directly selected content root, and validates actual nonempty SKYF texture/lightmap/object archives. Imported content lives under `<install-root>/assets` with its hash snapshot at `<install-root>/asset-manifest.json`; installation is atomic and does not replace an existing imported asset directory. These paths are separate from `releases/<version>` and native updates do not replace them.

The Inno path is deliberately restricted to the known Inno Setup 6.0 installer above, of exactly **1,684,201,720 bytes**. Its private staged copy is SHA-256 verified before launching the explicitly selected native ELF `innoextract` executable; unknown or changed installers fail closed without extraction, and shell scripts or Wine are not accepted as extractors. The accepted extracted source must contain exactly **759 regular files totaling 4,075,408,963 bytes**, with symlinks, hardlinks, and special files rejected. During extraction, a monitored staging budget permits at most **4,482,949,859 bytes** (10% payload headroom) and **859 files**. The child is additionally restricted by Linux `RLIMIT_FSIZE` of **2 GiB per file**, `RLIMIT_AS` of **2 GiB**, and `RLIMIT_CPU` of **300 CPU seconds**, with an absolute **300-second elapsed extraction deadline**, further limited by the whole operation's remaining time. One absolute **900-second deadline**, started before the installer snapshot/hash, covers snapshot copying, hashing, extraction, payload validation, asset import and retained-source promotion. Copy/hash chunks, tree entries and phase boundaries cooperatively check it; regular-file OS I/O cannot be forcibly interrupted if the kernel blocks. The finite extraction cap was raised because the integration owner had already observed the legitimate exact 6.0 payload taking **122.92 seconds** under native extraction; this is the reason for the correction, not a new verification result. On timeout or rejection the child is terminated and reaped and private staging is removed. Staging stays on the installation filesystem under `sources`; only successful extraction and import validation promote it to a retained repair source. No user-provided installer can enlarge these limits; authorized directory imports remain the alternative. The optional real-installer `launcher-extraction-test <trusted-installer> <native-innoextract>` check should run separately from CTest's 60-second unit-test timeout, with enough time for the 900-second operation budget.

**Dosyaları doğrula** checks recorded asset hashes. **Onar** prompts for the preserved source and checks every expected source file against the recorded hashes before changing the installed copy. Only missing or changed installed files are replaced. Unexpected extra files are preserved and reported as a verification failure, never deleted as part of repair. No official download endpoint or dummy substitute is used. Keep the source available: a hash establishes integrity, not permission to obtain missing copyrighted files. If it becomes unavailable, recover the exact legitimate source used for the snapshot or explicitly choose a fresh installation root for a new import.

The launcher uses QSettings under organization `IKVCommunity`, application `IKVLauncher`; profiles and launcher settings therefore remain outside versioned binaries. Client graphics settings live under the selected installation root's `settings/graphics.json`. Back up both settings and imported content independently of native release packages.

## Self-update

The launcher updates itself and the game in place; nobody has to download a new AppImage. A release's update payload **is the AppImage**: the signed package `ikv-linux-x86_64-X.Y.Z.tar.gz` contains exactly `bin/ikv-linux.AppImage` (mode `0755`). Defaults are compiled in, so nothing needs configuring: the trust root `packaging/ikv-release-public.pem` (the IKV Ed25519 release public key), repository `sergen213/ikv-linux` and channel `experimental` (IKV releases are GitHub prereleases). They come from CMake (`IKV_UPDATE_REPOSITORY`, `IKV_UPDATE_CHANNEL`, the PEM file); `IKV_VERSION` defaults to the `project()` version and is what `ikv-launcher --version` reports.

**On start** the launcher:

1. **Hands off to a newer installed release.** If `<install-root>/current` names a release whose version is SemVer-newer than the running build, and the full installed-release verification passes (trusted signed manifest, every installed file's hash, mode and owner, `current` confined to `releases/`, and the release contains `bin/ikv-linux.AppImage`), it replaces itself with that AppImage via `exec` (same arguments; `IKV_UPDATE_HANDOFF=1` prevents a loop). A manually downloaded *newer* AppImage therefore runs itself, while an *older* AppImage, or a source build, hands off to the newer installed one. An installed release that fails verification is reported (dialog and stderr: **Kurulu güncelleme kullanılamadı …**) and never executed; the running build continues.
2. **Checks for updates in the background.** It reads the channel's releases from the GitHub Releases API, ignores drafts and tags that are not `vX.Y.Z[-PRERELEASE]`, and when the highest one is SemVer-newer than the running build shows **Güncelleme mevcut: vX.Y.Z** with **Güncelle**. A failed check (offline, rate limit) is shown in the status line and diagnostic log only; the launcher stays usable.

**Güncelle** downloads that release's `manifest.json`, `manifest.sig` and the manifest's exact package basename, requires the signed manifest's channel and version to equal the release (tag = `v` + version), and installs it with `ikv-updater install` into `<install-root>/releases/<version>`. Imported assets, `settings/` and the launcher's settings are never touched. The launcher then restarts into the new `current/bin/ikv-linux.AppImage`; if that is refused (for example while the game is running) the banner offers **Yeniden başlat**. Every failure is shown in Turkish (**Güncelleme başarısız: …**, **İmzalı güncelleyici başarısız oldu; önceki sürüm korundu**) and leaves the previous release active.

Versions are ordered by strict [SemVer 2.0](https://semver.org/) precedence (`MAJOR.MINOR.PATCH[-PRERELEASE]`, a prerelease sorts before its release; no leading zeroes, build metadata or other garbage). Version order decides only what is offered and what a handoff selects. Installation freshness is still the signed per-channel sequence below, so a replayed older signed release is refused even if offered.

In **Ayarlar → Gelişmiş → Güncellemeler** every field may stay blank:

| Field | Meaning |
| --- | --- |
| **GitHub OWNER/REPOSITORY** | Blank = the compiled-in `sergen213/ikv-linux`. |
| **Kanal** | `experimental` (default) selects non-draft prereleases; `stable` selects non-draft, non-prerelease releases. The signed manifest must name the same channel. Stored as `updates/release_channel`; 0.3's `updates/channel` is discarded because it saved an unchosen `stable`. |
| **Güvenilen Ed25519 public PEM** | Blank = the compiled-in release key. Set it only to trust a different operator's key (or an ephemeral test key), acquired over an independently authenticated channel; never the private key. It also governs handoff and rollback verification. |
| **HTTPS CA PEM** | CA roots for release metadata and downloads; blank uses the system trust store (right for GitHub). For an owned test origin, choose its CA. |
| **Kendi HTTPS release API (isteğe bağlı)** | An HTTPS origin returning a GitHub Releases-style array; blank uses the repository's GitHub release API. |

**Güncellemeleri denetle** repeats the check on demand. **Önceki sürüme dön** verifies the previous retained signed release, swaps `current`/`previous` and restarts into it; repeating it toggles back. **İmzalı paket kur** installs a locally downloaded signed trio, used from the next start (handoff). HTTPS certificate validation and the Ed25519 signature are both required: HTTPS authenticates the transport, the trusted key authorizes installed bytes. A key shipped only with a download does not establish trust.

`ikv-launcher --update-once --install-root DIR --repo OWNER/REPO --trust PEM --update-ca PEM [--release-api URL] [--channel C] [--network-timeout-ms N]` runs the same check without dialogs: a newer release is installed through the real **Güncelle** button and the process restarts into it, which then prints `ikv-launcher X.Y.Z: güncel …` and exits 0. Failures print to stderr and exit 1.

The updater verifies the detached **raw 64-byte Ed25519** signature over the exact manifest bytes. Do not reformat JSON after signing. Strict schema `2` requires `schema`, `version`, `channel`, `sequence`, `package`, `package_size`, `package_sha256`, and `files`; `schema` must be `2`, and the signed `sequence` must be a JSON unsigned integer in `1`–`18446744073709551615`, not a string, float, or boolean. Schema `1` is rejected; there is no compatibility alias or unsigned sequence fallback. Each file records `path`, `size`, `sha256`, and `executable`. Package and file hashes are SHA-256. A safe unique version token is at most 96 ASCII characters (the launcher additionally requires SemVer); manifest file paths are bounded relative paths. Packages contain regular files/directories only, without a wrapping version directory or `./` prefix (IKV releases: exactly `bin/ikv-linux.AppImage`). `.ikv-update` is reserved updater metadata. Traversal, symlinks, hardlinks, device files, unexpected payload files, and extraction-limit violations are rejected.

Defensive limits are 1 MiB for manifest bytes, 16 KiB for the trusted public PEM, 10,000 signed files, 512 MiB per file, 4 GiB total payload, and 1 GiB compressed package. Paths are at most 512 ASCII characters, with components at most 128 characters using letters, digits, dot, underscore, and hyphen; absolute paths and `.`/`..` components are forbidden. Extraction permits at most 50,000 tar entries and bounds decompressed tar bytes to signed payload sizes plus 64 MiB of metadata overhead. Sparse, extended-attribute, encrypted, and special-file entries are rejected. Executable flags normalize file modes to `0755` or `0644`.

HTTPS downloads enforce byte caps on **every hop and every HTTP status**, not just successful responses: release API bodies are capped at **4 MiB**, manifest transport bodies at **2 MiB** (the updater still accepts only **1 MiB** of manifest bytes), signatures at **64 bytes**, and package bodies at the verified signed `package_size`. Redirect/error bodies have a separate **64 KiB per-hop** cap. All received bytes also count against the whole update's metadata/redirect/error budget of **7 MiB**; after verifying the manifest, the total budget becomes the signed `package_size` plus **7 MiB**. Redirect headers trigger early body refusal rather than downloading an unlimited body before deciding whether to follow. Only HTTPS redirects to allowed origins are followed: a configured owned API is confined to that same HTTPS origin; the default GitHub path allows `github.com`, `api.github.com`, and `*.githubusercontent.com`. Bearer-authenticated or POST requests never follow redirects.

Each request hop has an absolute **60-second wall-clock deadline**, independent of activity; staged downloads add one second per 128 KiB of their signed bound (about 8 minutes for a 62 MB package). The release check has one absolute **180-second** deadline, and the Güncelle download chain (manifest, signature, package, and all redirects) has 180 seconds plus the same per-128-KiB allowance for the signed `package_size`. The **60-second inactivity timeout** is additional, not a replacement for those deadlines; a slow stream cannot keep a request alive indefinitely. These deadlines cover network retrieval, not the separate local updater installation helper.


Installation stages and validates the package before activating `releases/<version>`. The `current` symlink is switched atomically only after verification. A running AppImage is never overwritten: the launcher `exec`s the newly installed one. Settings and imported assets remain outside the release. Rollback verifies the previous signed release before switching the pointer back; repeated rollback toggles the two retained releases. Retain the correct public key for rollback. Trust-key rotation requires an explicitly authenticated configuration change (or a release signed by the old key that compiles in the new one), not automatic trust of a replacement key from the download.

Release versions are immutable: an existing `releases/<version>` is never overwritten. An identical retained release may resume installation only after complete validation and an exact signed-metadata match; conflicting same-version payloads fail. Choose a new version identifier for every rebuilt payload. Rollback verifies the retained signed manifest, file hashes, normalized modes, and ownership before activation; it does not trust the previous pointer alone. The installation root must reside on a Linux filesystem supporting symlinks and `renameat2(RENAME_EXCHANGE)` for atomic rollback pointer exchange; unsupported filesystems fail closed. Root ancestors must not be symlinks and must be owned by root or the current user, without group/world write permissions unless sticky (such as root-owned `/tmp`).

The updater durably retains a separate sequence high-water mark for each signed channel at `<install-root>/.update-highwater/<channel>.json`, recording the sequence and SHA-256 of the exact verified manifest bytes. This state is fsynced before pointer cutover. A lower sequence is rejected on installation, even after rollback. Reusing the high-water sequence is allowed only for the byte-identical verified signed manifest; changing whitespace, version, package, or any other signed byte requires a higher sequence. Operators must allocate monotonically increasing sequences per channel and preserve those counters across releases; version-name ordering is never used. Explicit rollback may activate an older retained, verified release, and reinstalling the identical high-water manifest may reactivate it afterward. Neither rollback nor transaction recovery lowers the high-water mark or permits a replayed download. Do not remove updater-owned sequence metadata to work around a rejected update.

A durable `.update-transaction` journal records the version and prior `current`/`previous` targets before publication. Installation, verification, and rollback recover an interrupted cutover under the installation lock, restoring both prior pointers before proceeding. Leave updater-owned transaction metadata intact during recovery.


The same helper can be used without the UI:

```sh
bin/ikv-updater install --manifest manifest.json --signature manifest.sig \
  --package ikv-linux-x86_64-0.4.0.tar.gz --trust /path/to/operator-public.pem \
  --root /path/to/user-install
bin/ikv-updater rollback --trust /path/to/operator-public.pem \
  --root /path/to/user-install
```

## Publishing a release

`python3 tools/release.py X.Y.Z` (see [README](../README.md#portable-appimage)) builds the AppImage, packs `ikv-linux-x86_64-X.Y.Z.tar.gz` (only `bin/ikv-linux.AppImage`), signs `manifest.json` + `manifest.sig` (schema 2, channel `experimental`, the next per-channel sequence from the operator's local ledger) with `tools/sign_manifest.py` and the private key matching `packaging/ikv-release-public.pem`, and publishes the `vX.Y.Z` GitHub prerelease with the AppImage, its `.sha256`/`.json`, the package and the signed manifest. Signing happens only on the operator machine; there is no CI signing or publication workflow. The signer uses the OpenSSL executable and never generates a key. By hand:

```sh
python3 tools/sign_manifest.py --root payload \
  --package dist/ikv-linux-x86_64-X.Y.Z.tar.gz --version X.Y.Z --channel experimental \
  --sequence N --output dist/manifest.json --signature dist/manifest.sig \
  --key-file /secure/path/to/release-signing.pem
```

`payload/bin/ikv-linux.AppImage` is the AppImage being released; `N` must exceed every sequence previously signed for the channel. Never reuse a sequence for different bytes.

## Owned Qt GUI smoke

`launcher-gui-smoke` is a build-only test executable, not installed in the native release. It needs the `ikv-client` built beside it (OYNA runs the client beside the launcher). It takes one JSON object on stdin, followed by EOF. Required nonempty strings are `install_root`, `source`, `auth_url`, `world_host`, `ca`, `username`, and `password`; `world_port` is an integer in 1–65535; optional `trust` sets the update-key override field. `world_host`/`world_port` are the *expected* Play arguments: the launcher derives them from the server profile and `/v1/status`. Optional `timeout_seconds` is an integer in 1–3600 (default 300). Optional strings `model_map`, `audio_path`, and `sound_map` set the actual launcher fields.

Optional `install_screenshot_path` (first-run page), `login_screenshot_path` (login page with empty fields), `graphics_screenshot_path` (Ayarlar → Grafik) and `screenshot_path` (play page after the native child has stayed alive for two seconds on its character screen) each name a **new absolute PNG path** in an existing directory owned by the invoking user, without symlink components. Invalid paths, existing outputs, or capture/write failures fail the smoke. The images are Qt widget grabs, not game framebuffers or physical-GPU proof; they are mode 0600 because account names are visible.

The smoke asserts the first-run install page, drives the real directory picker, original import/hash verification (then the login page), graphics-settings save/reload, the **Sunucu ekle** dialog and profile persistence, HTTPS registration/login (then the play page naming the account), and OYNA with the `/v1/status` world port. Play's actual `ikv-client` (the one beside the smoke binary) must receive `--auth` (never `--character`), authenticate its HTTPS character listing through `--session-stdin`, print `CHARACTER_SCREEN characters=0` for the fresh account, remain running for two seconds and terminate with a normal zero exit after SIGTERM. Character creation and world entry are in-client steps outside this Qt smoke (see `analysis/porting-wave2/Frontend/capture_charsel.py`). Forced termination is a hard failure. Temporary QSettings data is removed; imported assets, graphics settings, screenshots, and the owned server account remain for operator inspection/cleanup. Use a fresh installation root without `assets` or `asset-manifest.json`, and a unique account name on an owned disposable database.

Use a trusted, nonsymlink home-directory ancestry for the proof directory, with enough disk space (about 4.064 GB) for the full original import; do not use a memory-backed runtime directory. Set `IKV_GUI_AUTH_URL`, `IKV_GUI_WORLD_HOST`, `IKV_GUI_WORLD_PORT`, and `IKV_GUI_CA` to the actual owned HTTPS/world services and certificate trust used by them. Hostnames must match the certificate SAN; there is no insecure TLS mode. Then run the smoke on an X display you own (for example one private `Xvfb -displayfd 1`), exported as `DISPLAY`:

```sh
export IKV_GUI_PROOF="$(mktemp -d "$HOME/.ikv-gui-proof.XXXXXX")"
QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=x11 python3 - <<'PY'
import json
import os
from pathlib import Path
import secrets
import subprocess

proof = Path(os.environ["IKV_GUI_PROOF"])
suffix = secrets.token_hex(6)
fixture = {
    "install_root": str(proof / "install"),
    "source": str(Path("original/app").resolve()),
    "auth_url": os.environ["IKV_GUI_AUTH_URL"],
    "world_host": os.environ["IKV_GUI_WORLD_HOST"],
    "world_port": int(os.environ["IKV_GUI_WORLD_PORT"]),
    "ca": str(Path(os.environ["IKV_GUI_CA"]).resolve()),
    "username": "gui_" + suffix,
    "password": secrets.token_urlsafe(24),
    "timeout_seconds": 600,
    "screenshot_path": str(proof / "launcher-live.png"),
}
result = subprocess.run(
    ["build-full/launcher-gui-smoke"],
    input=json.dumps(fixture).encode("utf-8"),
    check=False,
)
raise SystemExit(result.returncode)
PY
```

The password exists only in the Python/Qt processes and stdin JSON, never argv or printed fixture data. No fake child, official endpoint, Windows executable, service installation, or GitHub publication is involved. These commands describe preparation and invocation, not evidence that a smoke passed.

For the supplied `original/app`, directory import resolves `original/app/base`: **752 regular files totaling 4,064,003,749 bytes**, excluding the seven top-level Windows executables/DLLs/manual/icon. Existing import performs a full source hash scan, a second source read while copying/hashing, and fsyncs each output; explicit Verify reads/hashes the installed corpus once more. Thus this smoke needs about **4.064 GB additional asset storage** plus settings, and at least **12.192 GB of corpus reads plus 4.064 GB writes** (before archive/resource loading), not a zero-copy or subset import. It does not re-extract the 1.684 GB installer or modify the preserved source. Runtime duration depends on storage/cache; allow the documented timeout rather than claiming an unmeasured minimum time.

## Verification boundary

Unit and integration checks: `update-version-order` (strict SemVer ordering and garbage rejection) and `signed-update` (`tests/update_integration.py`: ephemeral Ed25519 key, the real signer/helper, and the real launcher `--update-once` against an owned TLS GitHub-Releases-shaped origin; it covers install, restart into the installed AppImage stand-in, startup handoff, a tampered installed release that is never executed, TLS/size/deadline/redirect/signature/hash rejections, replay and rollback). The two-AppImage end-to-end proof (the old AppImage detects and installs the new one and restarts into it, the old one hands off afterwards, rollback, tampered package/manifest/installed release) is `analysis/porting-wave12/SelfUpdate/selfupdate_e2e.py OLD.AppImage OLD_VERSION NEW.AppImage NEW_VERSION OUT_DIR`, with images from `tools/package_appimage.py --version … --work … --dist …`. All of these use owned temporary origins and ephemeral test keys, never the production key, real publication, or official game infrastructure.
