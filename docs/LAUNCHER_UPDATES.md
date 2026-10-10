# Native launcher and signed updates

The Qt 6 launcher manages the independently implemented native client and your own community-server accounts. It does not use official accounts, discover official servers, run Windows executables, or distribute original copyrighted game assets. Import only local content you are authorized to use; retain the source installation/installer.

## Using the launcher

Start `bin/ikv-launcher` (Qt Wayland or X11). The main window is one page; everything technical lives behind **⚙ Ayarlar**.

1. **Oyunu Kur** (first run, shown while no imported original files exist). Choose **Kurulum dosyasını seç (istanbulkur6_0.exe)** for the supported Inno Setup 6.0 installer, or **Mevcut kurulum klasörünü seç** for an existing authorized installation. The installer is never executed: only SHA-256 `34bb864018ad0c46a06a26d8bf0b4c48cd4bcf4c8db971c64b2ebda1b6fbdca7` is accepted and it is unpacked by the native ELF `innoextract`, found automatically (the configured path in Ayarlar → Gelişmiş, beside the launcher, the source tree's `.tools/innoextract-1.9-linux/bin/amd64/innoextract`, then `PATH`); the launcher asks for it only when none is found. Shell wrappers and Wine are rejected. Files are imported into the installation folder, by default Qt's `AppLocalDataLocation` (`~/.local/share/IKVCommunity/IKVLauncher`). Original sources are never edited; installer extraction is retained under `<install-root>/sources/<unique-id>` for later repair. On success the login page appears.
2. **Giriş**: pick a server, enter **Kullanıcı adı** / **Parola**, then **Giriş Yap** (or **Kayıt Ol** for a new private account; never official İKV credentials). A **Localhost** profile (`https://localhost:7443`) is preset. **Sunucu ekle** adds or edits a profile: a name, the account service address as `host` or `host:port` (default port 7443), and an optional CA certificate file (blank = the system trust store). The world server is the same host; its port is read from the account service's public `/v1/status` (`world.world_port`, default 7000) when you press OYNA. There is no certificate-verification bypass and no implicit official endpoint. **Parola sıfırla** takes an administrator-issued single-use token; the new password goes in the Parola field.
3. **OYNA**: after login the page names the account and offers **OYNA** and **Çıkış** (logout). There is no launcher character list: OYNA starts the client with `--auth <account origin>` and the session on stdin, and the client shows the original 3D character selection/creation screens (Yeni Karakter / Karakteri Sil / Başlat) itself, calling the same HTTPS `/v1/characters` API with the same CA ([CLIENT_PORTING](CLIENT_PORTING.md#in-client-character-selection-and-creation)). Passwords and session tokens are never persisted; the client receives its session on stdin, not in command-line arguments.
4. **Sunucu Kur / Sunucu Yönetimi** (the header **server chip**, which also shows the live state, e.g. `● Sunucu: Çalışıyor · 3 oyuncu`; also offered by the install page's opt-in **Bu bilgisayarda yerel sunucu da kur** checkbox after a successful import). **Guided creation**: İsim, administrator account and password (no default), bot count, Yerel/LAN and one button; progress steps are shown, errors are explained in Turkish, ports that are in use move to the next free port, and afterwards the launcher logs in to the **Localhost** profile automatically. It provisions an owned development CA/secrets (0600), a Unix-socket-only PostgreSQL cluster in AppLocalData and `ikv-auth` + `ikv-server` under a detached supervisor (`ikv-launcher --host-supervisor`). **Sunucu Yönetimi** is a native panel over the private management API pinned to that CA (no browser, no CA import): live status (uptime, players, bots, zones, tick, memory), start/stop/restart, live bot count, announcements, kick/ban, accounts with role/ban, rates and open zones, backups (create, list, restore with confirmation) and the server log. **Başlatıcı kapanınca da çalışsın** keeps the server running after the launcher closes (the next launcher reattaches); **Başlatıcı açılınca yerel sunucuyu otomatik başlat** starts it with the launcher. **Yönetim paneli (tarayıcı, gelişmiş)** remains as an advanced option, and **Sunucuyu sil** deletes the server after confirmation. Details and limits: [self-hosting](SELF_HOSTING.md#launcher-hosted-local-server-sunucu-kur--sunucu-yönetimi).

**Original look.** Once original files are imported, the launcher takes the original Windows launcher's appearance: it reads the skin bitmaps (RT_BITMAP resources: window frame, OYNA, OYUN REHBERİ, AYARLAR, minimize/close) at runtime from the user's own `launcher.exe`, located beside the selected source (`assets/source` itself, its `app/`, or the parent of a selected `base/`; installer imports retain it under `<install-root>/sources/`), with Verdana from the imported `fonts/`. The bitmaps are parsed natively with strict bounds checks and are never copied, bundled or redistributed. The same widgets are laid into the artwork's panels (pages top right, status/progress in the middle, server choice left, OYNA = the page's Giriş/OYNA action, activity log and **Sunucu Kur** at the bottom); **OYUN REHBERİ** opens the original `ikv_oyun_rehberi.pdf`. Before the first import, or if `launcher.exe` is missing or malformed, the plain window is shown.

OYNA runs the signed release when one was installed via the updater (`<install-root>/current`, verified as below); otherwise it runs the `ikv-client` beside the launcher executable, so a source build or installed prefix works without packaging. A blank server CA passes the system bundle (OpenSSL default, `SSL_CERT_FILE` honoured) to the client.

**Ayarlar → Grafik** persists renderer, Vulkan render scale, appearance (`original`/`enhanced`), ray tracing (`off`/`auto`/`required`, hardware-dependent; selecting it is not evidence of support), resolution, fullscreen and volume. **Ayarlar → Gelişmiş** holds the installation folder, asset verify/repair, signed package install, update repository/channel/trust/CA/API, update and rollback, optional original-content mappings and the diagnostic log (**Tanılama kaydet** exports it; review before sharing).

Vulkan is an optional native build capability: `IKV_BUILD_VULKAN=ON` (the default) retains both renderer choices and the hardware-dependent Vulkan ray-tracing provider. With `IKV_BUILD_VULKAN=OFF`, the launcher consumes `IKV_HAS_VULKAN=0` and offers only the genuine `opengl` renderer; this build needs neither the Vulkan SDK/loader nor `glslc`. A previously saved Vulkan selection remains unavailable rather than silently changing to OpenGL: **OYNA** reports the unavailable renderer until you explicitly select `opengl` in **Ayarlar → Grafik**. Saving unrelated settings does not replace that stored renderer. OpenGL cannot satisfy `required` ray tracing, and Play reports that incompatibility instead of claiming ray-tracing support. These compile-time choices do not establish GPU capability.

The optional mappings in Gelişmiş — **Model eşlemesi** (operator `id:N` → original member JSON), **Ses klasörü** (relative to the imported assets root) and **Ses olayı eşlemesi** — are passed to the client as `--model-map`, `--audio` and `--sound-map`. They remain explicit **NONCANONICAL** selections unless independently supported by recovered evidence.

## Asset verification and source repair

The original installation/installer is an import source, not an update artifact. Import resolves a selected installation's `base`, `app/base`, `app`, or directly selected content root, and validates actual nonempty SKYF texture/lightmap/object archives. Imported content lives under `<install-root>/assets` with its hash snapshot at `<install-root>/asset-manifest.json`; installation is atomic and does not replace an existing imported asset directory. These paths are separate from `releases/<version>` and native updates do not replace them.

The Inno path is deliberately restricted to the known Inno Setup 6.0 installer above, of exactly **1,684,201,720 bytes**. Its private staged copy is SHA-256 verified before launching the explicitly selected native ELF `innoextract` executable; unknown or changed installers fail closed without extraction, and shell scripts or Wine are not accepted as extractors. The accepted extracted source must contain exactly **759 regular files totaling 4,075,408,963 bytes**, with symlinks, hardlinks, and special files rejected. During extraction, a monitored staging budget permits at most **4,482,949,859 bytes** (10% payload headroom) and **859 files**. The child is additionally restricted by Linux `RLIMIT_FSIZE` of **2 GiB per file**, `RLIMIT_AS` of **2 GiB**, and `RLIMIT_CPU` of **300 CPU seconds**, with an absolute **300-second elapsed extraction deadline**, further limited by the whole operation's remaining time. One absolute **900-second deadline**, started before the installer snapshot/hash, covers snapshot copying, hashing, extraction, payload validation, asset import and retained-source promotion. Copy/hash chunks, tree entries and phase boundaries cooperatively check it; regular-file OS I/O cannot be forcibly interrupted if the kernel blocks. The finite extraction cap was raised because the integration owner had already observed the legitimate exact 6.0 payload taking **122.92 seconds** under native extraction; this is the reason for the correction, not a new verification result. On timeout or rejection the child is terminated and reaped and private staging is removed. Staging stays on the installation filesystem under `sources`; only successful extraction and import validation promote it to a retained repair source. No user-provided installer can enlarge these limits; authorized directory imports remain the alternative. The optional real-installer `launcher-extraction-test <trusted-installer> <native-innoextract>` check should run separately from CTest's 60-second unit-test timeout, with enough time for the 900-second operation budget.

**Dosyaları doğrula** checks recorded asset hashes. **Onar** prompts for the preserved source and checks every expected source file against the recorded hashes before changing the installed copy. Only missing or changed installed files are replaced. Unexpected extra files are preserved and reported as a verification failure, never deleted as part of repair. No official download endpoint or dummy substitute is used. Keep the source available: a hash establishes integrity, not permission to obtain missing copyrighted files. If it becomes unavailable, recover the exact legitimate source used for the snapshot or explicitly choose a fresh installation root for a new import.

The launcher uses QSettings under organization `IKVCommunity`, application `IKVLauncher`; profiles and launcher settings therefore remain outside versioned binaries. Client graphics settings live under the selected installation root's `settings/graphics.json`. Back up both settings and imported content independently of native release packages.

Before **OYNA** runs a signed release, the launcher confines `current` to the selected release root and verifies that release's trusted signed manifest and native file hashes (this needs **Güvenilen Ed25519 public PEM**). Asset verification and native release verification are separate checks; a valid native package does not supply or authenticate missing original content.

## Update trust and channels

In **Ayarlar → Gelişmiş → Güncellemeler**, explicitly configure:

| Field | Meaning |
| --- | --- |
| **GitHub OWNER/REPOSITORY** | Your chosen release operator's repository; no repository is implicitly trusted. |
| **Güvenilen Ed25519 public PEM** | Operator public key acquired over an independently authenticated channel; never the private signing key. |
| **HTTPS CA PEM** | CA roots for HTTPS metadata and artifact downloads; blank uses the system trust store (fine for GitHub). For an owned test origin, choose its CA. |
| **Kendi HTTPS release API (isteğe bağlı)** | An explicitly configured HTTPS origin returning a GitHub Releases-style array; leave empty to use the configured repository's GitHub release API. |
| **Kanal** | `stable` selects a non-draft, non-prerelease release; `experimental` selects a non-draft prerelease. The signed manifest must identify the same channel. |

Choose **İmzalı güncelleme** to retrieve the selected release (**İmzalı paket kur** installs a locally downloaded signed trio instead). Its assets must include `manifest.json`, `manifest.sig`, and the manifest's exact package basename. The launcher selects the first matching release in the returned array; an owned release API must order releases newest first. Version strings are identifiers, not an ordering mechanism: installation freshness is determined by the signed per-channel sequence. HTTPS certificate validation and an Ed25519 signature are both required: HTTPS authenticates the transport, while the independently configured key authorizes installed bytes. Missing repository or trust configuration is an error, not successful installation. A key bundled only with an untrusted download does not establish trust.

The updater verifies the detached **raw 64-byte Ed25519** signature over the exact manifest bytes. Do not reformat JSON after signing. Strict schema `2` requires `schema`, `version`, `channel`, `sequence`, `package`, `package_size`, `package_sha256`, and `files`; `schema` must be `2`, and the signed `sequence` must be a JSON unsigned integer in `1`–`18446744073709551615`, not a string, float, or boolean. Schema `1` is rejected; there is no compatibility alias or unsigned sequence fallback. Each file records `path`, `size`, `sha256`, and `executable`. Package and file hashes are SHA-256. A safe unique version token is at most 96 ASCII characters; manifest file paths are bounded relative paths. Packages contain regular files/directories only, rooted at `bin/` and `share/ikv/`, without a wrapping version directory or `./` prefix. `.ikv-update` is reserved updater metadata. Traversal, symlinks, hardlinks, device files, unexpected payload files, and extraction-limit violations are rejected.

Defensive limits are 1 MiB for manifest bytes, 16 KiB for the trusted public PEM, 10,000 signed files, 512 MiB per file, 4 GiB total payload, and 1 GiB compressed package. Paths are at most 512 ASCII characters, with components at most 128 characters using letters, digits, dot, underscore, and hyphen; absolute paths and `.`/`..` components are forbidden. Extraction permits at most 50,000 tar entries and bounds decompressed tar bytes to signed payload sizes plus 64 MiB of metadata overhead. Sparse, extended-attribute, encrypted, and special-file entries are rejected. Executable flags normalize file modes to `0755` or `0644`.

HTTPS downloads enforce byte caps on **every hop and every HTTP status**, not just successful responses: release API bodies are capped at **4 MiB**, manifest transport bodies at **2 MiB** (the updater still accepts only **1 MiB** of manifest bytes), signatures at **64 bytes**, and package bodies at the verified signed `package_size`. Redirect/error bodies have a separate **64 KiB per-hop** cap. All received bytes also count against the whole update's metadata/redirect/error budget of **7 MiB**; after verifying the manifest, the total budget becomes the signed `package_size` plus **7 MiB**. Redirect headers trigger early body refusal rather than downloading an unlimited body before deciding whether to follow. Only HTTPS redirects to allowed origins are followed: a configured owned API is confined to that same HTTPS origin; the default GitHub path allows `github.com`, `api.github.com`, and `*.githubusercontent.com`. Bearer-authenticated or POST requests never follow redirects.

Each request hop has an absolute **60-second wall-clock deadline**, independent of activity, and the entire update download chain (API, manifest, signature, package, and all redirects) has one absolute **180-second deadline**. The **60-second inactivity timeout** is additional, not a replacement for those deadlines; a slow stream cannot keep a request alive indefinitely. These deadlines cover network retrieval, not the separate local updater installation helper.


Installation stages and validates the package before activating `releases/<version>`. The `current` symlink is switched atomically only after verification. The existing process continues running its original executable; a running launcher is never overwritten. Close it and start `<install-root>/current/bin/ikv-launcher` to use the newly installed launcher. Settings and imported assets remain outside the release. **Önceki sürüme dön** verifies the previous signed release and switches the pointer back; repeated rollback toggles the two retained releases. Retain the correct public key for rollback. Trust-key rotation requires an explicitly authenticated configuration change, not automatic trust of a replacement key from the download.

Release versions are immutable: an existing `releases/<version>` is never overwritten. An identical retained release may resume installation only after complete validation and an exact signed-metadata match; conflicting same-version payloads fail. Choose a new version identifier for every rebuilt payload. Rollback verifies the retained signed manifest, file hashes, normalized modes, and ownership before activation; it does not trust the previous pointer alone. The installation root must reside on a Linux filesystem supporting symlinks and `renameat2(RENAME_EXCHANGE)` for atomic rollback pointer exchange; unsupported filesystems fail closed. Root ancestors must not be symlinks and must be owned by root or the current user, without group/world write permissions unless sticky (such as root-owned `/tmp`).

The updater durably retains a separate sequence high-water mark for each signed channel at `<install-root>/.update-highwater/<channel>.json`, recording the sequence and SHA-256 of the exact verified manifest bytes. This state is fsynced before pointer cutover. A lower sequence is rejected on installation, even after rollback. Reusing the high-water sequence is allowed only for the byte-identical verified signed manifest; changing whitespace, version, package, or any other signed byte requires a higher sequence. Operators must allocate monotonically increasing sequences per channel and preserve those counters across releases; version-name ordering is never used. Explicit rollback may activate an older retained, verified release, and reinstalling the identical high-water manifest may reactivate it afterward. Neither rollback nor transaction recovery lowers the high-water mark or permits a replayed download. Do not remove updater-owned sequence metadata to work around a rejected update.

A durable `.update-transaction` journal records the version and prior `current`/`previous` targets before publication. Installation, verification, and rollback recover an interrupted cutover under the installation lock, restoring both prior pointers before proceeding. Leave updater-owned transaction metadata intact during recovery.


The same helper can be used without the UI:

```sh
bin/ikv-updater install --manifest manifest.json --signature manifest.sig \
  --package ikv-linux-x86_64-0.2.0.tar.gz --trust /path/to/operator-public.pem \
  --root /path/to/user-install
bin/ikv-updater rollback --trust /path/to/operator-public.pem \
  --root /path/to/user-install
```

## Asset-free release workflow

`.github/workflows/native-release.yml` runs **only** through `workflow_dispatch`. Supply a unique `version`, a `stable` or `experimental` channel, an explicit positive decimal uint64 `sequence` (`1`–`18446744073709551615`, without signs or leading zeroes), and the explicit `sign` and `publish` booleans. Both booleans default to `false`; there is no push/tag-triggered signing or publication. `publish=false` keeps the default artifact-only behavior: the build job has only `contents: read` and uploads a 14-day workflow artifact, which is not a GitHub Releases download source for the launcher. Access to artifacts follows repository/GitHub permissions. `publish=true` additionally requires `sign=true`, production approval, a configured independent public trust root, and an existing operator-initialized ledger branch; missing prerequisites fail closed. No production key is generated and no package registry is used.

For `publish=true`, dispatch only from the repository's **reviewed default branch**. A dispatch from any other branch or tag fails input validation rather than silently skipping publication. The publisher also requires the explicit source target to equal the repository's **current default-branch HEAD** before its first write. If that tip advances while the package builds or waits for approval, publication fails closed: review the new commit and explicitly dispatch/approve a fresh workflow with a new version and newly reserved sequence. There is no automatic retry, alternate credential, or extra `workflows: write` permission to tag arbitrary workflow-bearing commits.

Before authorizing production signing, configure the **native-release-signing** GitHub environment with required reviewers, trusted branch restrictions, and the externally provisioned environment secret **IKV_RELEASE_SIGNING_KEY_PEM** containing an Ed25519 private PEM. Do not place a production key in the repository. Environment approval applies to the job even when `sign=false`; signing additionally requires `sign=true`, and a missing secret fails the signing step. Review the source commit and workflow before approving access to the production key. The workflow writes the secret only to a restrictive temporary file and removes that file on step exit.

### Explicit production publication setup

Before dispatching with `publish=true`, an authorized repository administrator must:

1. Create the **native-release-production** GitHub environment with nonempty **required reviewers**, deployment branch restrictions allowing only the reviewed repository default branch, and self-review disabled. Keep the signing environment separately protected. Reviewers must review the source commit, workflow, channel, and sequence before approving each job. The publisher reads the environment's protection rules through the GitHub API and refuses publication if required reviewers are absent or the API check fails; an environment name alone is not a protection rule. Configure a GitHub plan/repository visibility that supports required reviewers ([GitHub environment prerequisites](https://docs.github.com/en/rest/deployments/environments#get-an-environment)).
2. Configure **IKV_RELEASE_PUBLIC_KEY_PEM** as a production environment secret or variable containing the **existing, independently authenticated Ed25519 public PEM** matching the signing key and launcher trust configuration. The secret takes precedence over the variable. This is not a private key and not a public key generated or learned from the downloaded artifact. Do not generate a replacement production key to make a failed verification pass.
3. Explicitly create a dedicated ledger branch in the **same chosen repository** and commit both files below before enabling publication. These are operator bootstrap files, not signed manifests; `sequence: 0` is permitted only here, never in a release manifest:

   ```text
   .ikv-release-ledger/stable.json
   {"schema":1,"channel":"stable","sequence":0}

   .ikv-release-ledger/experimental.json
   {"schema":1,"channel":"experimental","sequence":0}
   ```

   Set production environment variable **IKV_RELEASE_LEDGER_BRANCH** to that exact branch name. Neither the workflow nor publisher creates a missing branch or ledger file. Preserve this repository/branch identity for every publication and retry; changing it or resetting its files discards the trusted publication floor. Restrict human writes, force pushes, branch deletion, and ledger edits while permitting the authorized Actions token's Contents API updates. Back up the branch and its history. If releases were already issued outside this automation, audit their per-channel maximum and select a sequence above every issued/reserved value before the first automated publication; zero seed files are not permission to reuse old counters.
4. Enable the repository's Actions token to write contents for the publication job. Only this job requests `contents: write`; the build/sign job remains read-only. The publication job also grants `actions: read`, required by the authenticated [Get an environment API](https://docs.github.com/en/rest/deployments/environments?apiVersion=2022-11-28#get-an-environment--fine-grained-access-tokens) to inspect protection rules; it does not grant administration or Actions write access. No personal publishing token is required. The protected publication job waits for the successful `package` job, downloads that run's signed artifact, verifies its source commit, writes the configured public PEM to a temporary `0600` file, and removes the PEM on exit. It uses the same immutable Debian image/snapshot with pinned public [Debian GitHub CLI `2.46.0-3`](https://packages.debian.org/trixie/gh) and pinned [download-artifact v4.3.0 commit](https://github.com/actions/download-artifact/commit/d3f86a106a0bac45b974a628896c90dbdf5c8093).

There is no default or auto-derived sequence: input validation rejects an invalid sequence even when `sign=false`. For **every production signing request**, first reserve a sequence greater than all prior signing reservations in a durable operator-controlled per-channel issuance ledger, and review that reservation before approving the signing job. Never reuse a signing reservation for new bytes, including after a failed job, and never derive sequences from version ordering or a workflow run number. **The `package` job signs before the publication job's GitHub CAS reservation**: the GitHub ledger is the enforced public-release reservation floor, not an automatic signing-time allocator. Before any external publication write, `tools/publish_release.py` revalidates the exact schema-2 manifest, raw Ed25519 signature, trusted public key, package size/SHA-256, and signed package contents. It reads `.ikv-release-ledger/<channel>.json` on the configured branch and durably reserves the authenticated sequence using the GitHub Contents API's existing-file SHA compare-and-swap **before draft creation, asset uploads, or final publication**. A lower sequence, invalid/missing ledger, or concurrent stale reservation fails closed. Publication jobs are serialized per repository/channel with `cancel-in-progress: false`; serialization does not replace the CAS, which also protects against independent publishers.

A reserved counter is **never reused for new bytes**, including after failed or partial publication. Equal-sequence publication retries require identical manifest, package, and signature metadata; otherwise allocate a new version and strictly higher sequence. Preserve both issuance and publication floors across workflow runs, local rollback, and release deletion. The client's separate per-channel high-water state continues to prevent installation downgrade after rollback. Signing, including signing-only artifact delivery, does not itself update the GitHub publication ledger: preserve the operator-controlled issuance ledger and ensure every future published sequence exceeds all separately issued/reserved values too.

Publication creates a draft first, uploads the actual signed trio without replacing conflicting assets, and exposes the release only after the assets are complete. The tag is **`native-stable-<version>`** for stable or **`native-experimental-<version>`** for experimental, targeting the dispatched **`${{ github.sha }}`**, in **`${{ github.repository }}`**; that target must still be the reviewed default-branch HEAD when the publisher checks it before reservation. Stable releases are non-draft/non-prerelease; experimental releases are non-draft/prerelease. The assets are **`manifest.json`**, **`manifest.sig`**, and **`ikv-linux-x86_64-<version>.tar.gz`**, matching the launcher's downloader. Existing assets must be byte-identical for a retry; conflicting assets are refused, never overwritten. Never include copyrighted imported game assets, private keys, or user configuration.


The build executes in an immutable Debian 13 amd64 container manifest:

```text
debian@sha256:918311b7b6c4c6f68b232ba516584925f6c78ad82b6fd534b98979df6438e483
```

All build package resolution uses the [Debian snapshot dated 2026-10-06](https://snapshot.debian.org/archive/debian/20261006T000000Z/), with exact direct versions in the workflow and transitive versions frozen by the same snapshot. The snapshot's expiration check is disabled because it is historical; Debian archive signatures and package hashes remain mandatory. Initial APT uses HTTP to bootstrap CA certificates in the slim container, without disabling package authentication. GitHub actions are pinned by full commit SHA. Important versions are Debian `build-essential=12.12`, CMake `3.31.6-2`, Qt `6.8.2+dfsg-9+deb13u2`, OpenSSL `3.5.7-1~deb13u2`, Boost `1.83.0.2+b2`, PostgreSQL libpq `17.11-0+deb13u1`, Vulkan headers/loader `1.4.309.0-1`, and glslc `2025.2-1`. Updating pins is an intentional maintainer operation.

The native audio API requires SDL 3.4, so the workflow builds and statically links the public [SDL 3.4.18 source release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18), verifying archive SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3` before extraction. Debian's pinned SDL 3.2 development package supplies platform build dependencies, not the SDL implementation linked into the client. The public SDL license is included in the payload; no original game resources are supplied.

CMake builds the real native executables and installs them into a clean payload:

```text
bin/ikv-client
bin/ikv-launcher
bin/ikv-updater
bin/ikv-assets
bin/ikv-original-inspect
bin/ikv-auth
bin/ikv-server
share/ikv/shaders/                 owned source/compiled shaders
share/ikv/config/                  independent community configuration
share/ikv/database/migrations/     independent database migrations
share/ikv/web/                     independent administration UI
share/ikv/licenses/SDL3-LICENSE.txt public dependency license
```

No `original/`, imported assets, local installer, credentials, private server keys, user profiles, or user graphics settings are copied. The tar archive contains relative `bin/` and `share/` entries. Archive ownership and modification times are normalized to the source commit timestamp. The artifact includes the package, `source-commit.txt`, and `build-packages.tsv`; signing adds `manifest.json` and `manifest.sig`.

The package does **not** bundle system shared libraries. It targets Debian 13 x86-64, not arbitrary Linux ABI versions. A Debian 13 desktop installation needs the runtime packages and GPU drivers appropriate to its selected backend; the library/plugin package set can be installed with:

```sh
sudo apt-get install libqt6widgets6 libqt6network6 qt6-qpa-plugins \
  libssl3t64 libarchive13t64 libpq5 libfreetype6 libfontconfig1 \
  libgl1 libvulkan1 innoextract
```

SDL 3.4.18 is statically linked, so installing Debian's older shared SDL is not a substitute for rebuilding this package. Standard desktop audio/display platform libraries (ALSA/PulseAudio/PipeWire and X11/Wayland) and working graphics drivers must also be available for the selected session/backend. The package list does not install a graphical session or promise hardware ray-tracing support. Server operators must separately configure their own TLS credentials, database, and services; packaged executables are not preconfigured public services.

For an independently authorized operator release, the signing command used by the workflow is:

```sh
python3 tools/sign_manifest.py --root package \
  --package dist/ikv-linux-x86_64-0.2.0.tar.gz --version 0.2.0 --channel stable \
  --sequence 1 \
  --output dist/manifest.json --signature dist/manifest.sig \
  --key-file /secure/path/to/existing-production-key.pem
```

The signer uses the OpenSSL executable, not a Python cryptography package. It never generates a production key. The opt-in protected publication job invokes the publisher below using the same chosen repository, fixed ledger branch, dispatched commit, and independently configured public trust root. Without `--publish`, the publisher performs only local verification and makes **no GitHub/network calls**; adding `--publish` explicitly authorizes the ledger reservation and GitHub Release writes, so do not add it merely to validate a package:

```sh
python3 tools/publish_release.py --repo OWNER/REPOSITORY \
  --manifest dist/manifest.json --signature dist/manifest.sig \
  --package dist/ikv-linux-x86_64-0.2.0.tar.gz \
  --trust /secure/path/to/existing-operator-public.pem \
  --ledger-ref operator-release-ledger \
  --target FULL_40_HEX_SOURCE_COMMIT_SHA
```

For separately authorized publication, set `GH_TOKEN` to an appropriate contents-write, Actions-read token and add `--publish` only after completing the production environment/ledger prerequisites above. This guide does not execute publication or push anything.

The publisher defaults to `github.com`, as does this workflow. For a separately configured, legitimate GitHub Enterprise installation, explicitly supply `--host HOST[:PORT]`; it uses HTTPS with certificate validation and keeps that selected host, repository, and ledger branch fixed through reservation and publication. Keep the operator trust PEM independently authenticated on Enterprise too. An owned integration-test CA may be explicitly supplied through the local certificate trust configuration; there is no insecure HTTP or certificate-verification bypass.

`--sequence` is mandatory; the example's `1` is suitable only for a channel with no previously issued release. Set it to the next operator-allocated per-channel sequence for a real release. The signer emits only schema `2`; regenerate and sign old schema manifests with a fresh authorized sequence rather than relabeling schema `1`.

## Owned Qt GUI smoke with a real signed client

`launcher-gui-smoke` is a build-only test executable, not installed in the native release. It takes one JSON object on stdin, followed by EOF. Required nonempty strings are `install_root`, `source`, `auth_url`, `world_host`, `ca`, `trust`, `username`, and `password`; `world_port` is an integer in 1–65535. `world_host`/`world_port` are the *expected* Play arguments: the launcher derives them from the server profile and `/v1/status`. Optional `timeout_seconds` is an integer in 1–3600 (default 300). Optional strings `model_map`, `audio_path`, and `sound_map` set the actual launcher fields.

Optional `install_screenshot_path` (first-run page), `login_screenshot_path` (login page with empty fields), `graphics_screenshot_path` (Ayarlar → Grafik) and `screenshot_path` (play page after the native child has stayed alive for two seconds on its character screen) each name a **new absolute PNG path** in an existing directory owned by the invoking user, without symlink components. Invalid paths, existing outputs, or capture/write failures fail the smoke. The images are Qt widget grabs, not game framebuffers or physical-GPU proof; they are mode 0600 because account names are visible.

The smoke asserts the first-run install page, drives the real directory picker, original import/hash verification (then the login page), graphics-settings save/reload, the **Sunucu ekle** dialog and profile persistence, HTTPS registration/login (then the play page naming the account), signed-release verification, and OYNA with the `/v1/status` world port. Play's actual `ikv-client` must receive `--auth` (never `--character`), authenticate its HTTPS character listing through `--session-stdin`, print `CHARACTER_SCREEN characters=0` for the fresh account, remain running for two seconds and terminate with a normal zero exit after SIGTERM. Character creation and world entry are in-client steps outside this Qt smoke (see `analysis/porting-wave2/Frontend/capture_charsel.py`). Forced termination is a hard failure. Temporary QSettings data is removed; imported assets, graphics settings, the signed installation, screenshots, and the owned server account remain for operator inspection/cleanup. Use a fresh installation root without `assets` or `asset-manifest.json`, and a unique account name on an owned disposable database. `analysis/final_launcher_smoke.py --source . --build <full> --gl-build <gl-only> --original original --output-dir <new>` packages, signs and runs both builds end to end on the X display named by `DISPLAY`.

Prepare a real asset-free package from an already completed full native build, with the existing CMake install layout. This local-only example generates an ephemeral **test** trust key; never replace a production trust root with it:

```sh
set -eu
umask 077
export IKV_GUI_PROOF="$(mktemp -d "$HOME/.ikv-gui-proof.XXXXXX")"
cmake --install build-full --prefix "$IKV_GUI_PROOF/payload"
tar --format=ustar -czf "$IKV_GUI_PROOF/ikv-linux-x86_64-gui-proof-1.tar.gz" \
  -C "$IKV_GUI_PROOF/payload" bin share
openssl genpkey -algorithm ED25519 -out "$IKV_GUI_PROOF/signing-private.pem"
openssl pkey -in "$IKV_GUI_PROOF/signing-private.pem" \
  -pubout -out "$IKV_GUI_PROOF/trust-public.pem"
python3 tools/sign_manifest.py --root "$IKV_GUI_PROOF/payload" \
  --package "$IKV_GUI_PROOF/ikv-linux-x86_64-gui-proof-1.tar.gz" \
  --version gui-proof-1 --sequence 1 --channel stable \
  --output "$IKV_GUI_PROOF/manifest.json" --signature "$IKV_GUI_PROOF/manifest.sig" \
  --key-file "$IKV_GUI_PROOF/signing-private.pem"
build-full/ikv-updater install --manifest "$IKV_GUI_PROOF/manifest.json" \
  --signature "$IKV_GUI_PROOF/manifest.sig" \
  --package "$IKV_GUI_PROOF/ikv-linux-x86_64-gui-proof-1.tar.gz" \
  --trust "$IKV_GUI_PROOF/trust-public.pem" --root "$IKV_GUI_PROOF/install"
```

Use a trusted, nonsymlink home-directory ancestry for this mode-0700 proof directory, with enough disk space for the full original import. The updater deliberately rejects installation roots beneath ancestors writable or replaceable by other users, including a world-writable shared-mount ancestor even when the leaf directory is private. That rejection is correct: select a trusted owned location instead of relaxing updater checks or changing permissions on user/shared mount directories. Keep the installation root fresh; do not use a memory-backed runtime directory unless it has capacity for the approximately 4.064 GB asset copy.

The full-build install includes the genuine native executables and owned resources, including all eight real compiled Vulkan shaders at `share/ikv/shaders/vulkan/{scene.vert,scene.frag,scene_rt.frag,shadow.vert,shadow.frag,fullscreen.vert,post.frag,overlay.frag}.spv`. Keep those installed SPIR-V files even when this smoke uses the default OpenGL backend; do not replace them with empty files or copy original shaders into the signed payload. CMake requires `nlohmann_json >= 3.9`; it is a header-only build dependency, not a runtime JSON shared library to fabricate/bundle. Updater metadata uses strict schema 2, an unsigned integer sequence, raw 64-byte Ed25519 signature, exact signed JSON bytes, and signed file/package hashes. `gui-proof-1`/sequence `1` are valid for this fresh owned root only: every changed payload in a retained root needs a new immutable version and a higher per-channel sequence. Keep manifest/signature/package outside the payload before signing.

Set `IKV_GUI_AUTH_URL`, `IKV_GUI_WORLD_HOST`, `IKV_GUI_WORLD_PORT`, and `IKV_GUI_CA` to the actual owned HTTPS/world services and certificate trust used by them. Hostnames must match the certificate SAN; there is no insecure TLS mode. Then run the smoke on an X display you own (for example one private `Xvfb -displayfd 1`), exported as `DISPLAY`:

```sh
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
    "trust": str(proof / "trust-public.pem"),
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

For the supplied `original/app`, directory import resolves `original/app/base`: **752 regular files totaling 4,064,003,749 bytes**, excluding the seven top-level Windows executables/DLLs/manual/icon. Existing import performs a full source hash scan, a second source read while copying/hashing, and fsyncs each output; explicit Verify reads/hashes the installed corpus once more. Thus this smoke needs about **4.064 GB additional asset storage** plus its signed package/settings, and at least **12.192 GB of corpus reads plus 4.064 GB writes** (before archive/resource loading), not a zero-copy or subset import. It does not re-extract the 1.684 GB installer or modify the preserved source. Runtime duration depends on storage/cache; allow the documented timeout rather than claiming an unmeasured minimum time.

## Verification boundary

This workflow defines runnable build/package/sign/artifact and explicitly authorized publication steps; writing it is not evidence of a successful remote Actions run or a published release. The integration owner runs native build, launcher/import, trusted local-origin update/rollback, malformed-package rejection, signature/hash rejection, and publisher reservation/retry integration checks using the actual GitHub CLI, real signed bytes, an owned TLS GitHub Enterprise-shaped origin, and a native bare-Git CAS ledger. Such checks must use owned temporary origins and ephemeral test keys, not fake GitHub CLI binaries, production keys, real public publication, or official game infrastructure.
