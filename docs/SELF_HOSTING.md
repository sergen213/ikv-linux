# Self-hosting the independent native services

## Scope and proof boundary

The deployable backend consists of `ikv-auth` (player HTTPS and separate private management HTTPS), `ikv-server` (authoritative authenticated TLS world), and PostgreSQL. It uses the custom v2 protocol, not a recovered original handshake. Gameplay definitions with unknown original rules are explicitly NONCANONICAL. One community world process owns multiple zone instances; this is not a horizontally distributed world/lease design.

Current parent-observed **fresh Source deployment acceptance passed99.87seconds combined**: a newly built asset-free image/new disposable Compose project and an owned current-user-adapted systemd fixture used the current Source0/1 configuration and genuine external assets. Compose proved CA-verified native peers, UID10001/read-only-root/zero capabilities/no-new-privileges, private management/internal database, all five application-role elevation flags false, Source1 transfer/persistence and world-only restart/reentry. The user-systemd adaptation proved Source1 transfer/restart/reentry and strict sandboxing, but uses its disposable PostgreSQL bootstrap superuser; it is **not Compose least-privilege-role or unmodified privileged production-unit proof**. See the concrete scopes below and [progress](PROGRESS.md#current-parent-integration-evidence). Container custom dump+TOC was inspected, **not restored**; isolated-suite plain-SQL restore is a separate result. No service is automatically installed, started or exposed by editing these definitions.

## Build

The backend-only build requires C++20/CMake, Threads, Boost headers, nlohmann JSON, PostgreSQL/libpq, OpenSSL >=3.2 **and the ARGON2ID KDF provider**, and Zlib. There is no weaker password-hash fallback. `ikv-server`/`ikv-world` link the actual CPU `ikv-scene` and `ikv-assets-lib` for Source descriptors/support grids; these do not require the client renderer, SDL3, OpenGL, Vulkan, FreeType, Fontconfig, Qt or LibArchive. See [README](../README.md) for the full client/launcher dependency set.

```sh
cmake -S . -B build-server -DCMAKE_BUILD_TYPE=Release \
  -DIKV_BUILD_CLIENT=OFF -DIKV_BUILD_LAUNCHER=OFF \
  -DIKV_BUILD_ASSET_TOOLS=OFF -DIKV_BUILD_ORIGINAL_TOOLS=OFF -DBUILD_TESTING=OFF
cmake --build build-server --parallel
sudo cmake --install build-server --prefix /usr/local
openssl list -kdf-algorithms
```

`ARGON2ID` must be present in the selected runtime provider list. CMake version checks alone do not prove the runtime provider is enabled. The runtime image uses Debian trixie/OpenSSL3.5 and its Dockerfile checks the actual provider while building. It includes a PostgreSQL17 `pg_dump`/`pg_restore` client, matching the Compose PostgreSQL17 server. Package downloads are public dependencies only; no original assets enter the image/context.

## Direct native services

Provision an owned PostgreSQL database with a login role owning only its application database, not a PostgreSQL superuser. Configure SCRAM authentication on a local Unix socket or deliberately isolated database network. An example private DSN file contains `host=/owned/socket dbname=ikv user=ikv password=...`; mode **exactly0600**, owned by the running service user. Never pass DSN/passwords as argv or log them.

Provide an operator-owned TLS certificate/private key and explicit CA with correct DNS/IP SANs. The client verifies CA, hostname/IP and TLS; never use `--insecure`, verification bypasses or an official-server default. For owned localhost acceptance, the test fixture creates its own short-lived CA and SAN localhost/127.0.0.1 certificate, cleans them on exit and never touches production credentials.

```sh
build/ikv-auth --bind 127.0.0.1 --port 7443 --admin-bind 127.0.0.1 --admin-port 7444 \
  --cert /private/tls-cert --key /private/tls-key --dsn-file /private/dsn \
  --migrations database/migrations --web-root web --backup-dir /private/backups
build/ikv-server --bind 127.0.0.1 --port 7000 --cert /private/tls-cert \
  --key /private/tls-key --dsn-file /private/dsn --gameplay config/gameplay.json \
  --asset-root /owned/readable/original
```

Start auth first so migrations exist. The readiness records are `READY auth_port=N admin_port=N` and `READY world_port=N`; port0 selects a local OS-assigned port. Native client/launcher profiles explicitly select the owned service, CA and character. Sessions enter the client through stdin only.

`--asset-root` is mandatory for the shipped Source-associated gameplay. The common read-only resolver requires genuine `objects.dat` at the root, `base/`, or `app/base/`; even a support-only backend fixture must supply that genuine archive, not an empty/fake anchor or a special zone-only resolver. The recovered **71-entry Source registry (IDs0..70)** selects **18 support grids (IDs0..17)**; the world caches loaded grids by support ID, so registry IDs sharing a support grid do not duplicate it. Gameplay selects the registry through each zone's `source_registry_id` in `config/gameplay.json`, not legacy server zone flags.

The shipped gameplay configures 15 Source-associated zones (registry IDs 0-6, 26, 36, 46, 66-70; support grids 0-7 and 11-17), generated by `tools/generate_content.cpp` and connected by original `<loc>` door triggers ([server architecture](SERVER_ARCHITECTURE.md#configured-world-and-source-provenance)). The world therefore needs those 15 `zoneNN.bin`/`zoneNN_height.bin` pairs (about 20 MiB of resident support data each). `eminonu` spawns at `[385500,805500]` in the Yeni Cami courtyard. Names, the native 1000/source-unit mapping, spawns, bounds, stats and rules remain **NONCANONICAL** where the generator says so. At admission, a saved character in a zone that no longer exists (for example the retired `old_town`) moves to the default spawn.

For host-native client/launcher `--ca`, host `curl --cacert`, and browser trust import, use the public `deploy/secrets/client-ca.pem` exported below—not the service-owned `tls-ca` or either private key. Browser trust configuration must be explicitly scoped to this owned development issuer.

The player listener returns404 for `/admin` and `/v1/admin` even to an administrator. Management is separately bound on loopback/private infrastructure and requires the account's server-assigned admin role. Register an independent account, stop auth and restart it once with `--admin-bootstrap EXISTING_USERNAME` to elevate that explicitly named account locally; remove the bootstrap argument after use. There is no default admin/password or public role parameter. [Authentication/database](AUTH_DATABASE.md) defines management/reset and moderation routes.

## Launcher-hosted local server (Sunucu Kur / Sunucu Yönetimi)

The Qt launcher's header **server chip** (`Sunucu Kur` until a server exists, then `● Sunucu: Çalışıyor · N oyuncu`, `○ Sunucu: Kapalı`, `◌ Başlatılıyor…` or `✕ Hata`; also opened by the install page's opt-in **Bu bilgisayarda yerel sunucu da kur** after a successful import) opens one dialog. Implementation: `src/launcher_server.cpp`. It needs the imported original assets (the world reads `<install-root>/assets`), `ikv-auth`/`ikv-server` beside the launcher (source build, signed release or AppImage), the installed or source-tree `config/gameplay.json`, `database/migrations` and `web`, `openssl`, and PostgreSQL server tools (`initdb`, `pg_ctl`, `psql` from `PATH` or Debian's newest `/usr/lib/postgresql/N/bin`; the AppImage bundles them). Missing tools are reported in Turkish with the install command.

**Guided creation.** One form: **İsim**, **Yönetici hesabı** (defaults to the OS user name), **Yönetici parolası** (typed, 12+ characters; no default), **Bot sayısı** (default 10, NONCANONICAL server bots) and **Yerel** / **LAN** (all IPv4 interfaces; switchable later). **Gelişmiş** shows the ports (default 7443 account, 7444 management, 7000 world; 0 = OS-assigned) and an optional LAN DNS name. **Sunucuyu Kur ve Başlat** runs the listed steps with ✓/●/✗ marks: requirements, free ports (a requested port that is in use moves to the next free one, logged), keys and certificates, the private PostgreSQL, account service, administrator account, world, launcher login. Afterwards the **Localhost** profile points at the actual auth origin and `secrets/client-ca.pem`, the launcher logs in with the new administrator (play page), and the panel opens an administrator session on the management API. A failed creation leaves `host.json` without `admin`; the form then keeps the access mode and pressing the button again retries (registering, or proving the existing account by login, then bootstrapping).

Everything lives in Qt's AppLocalData (`~/.local/share/IKVCommunity/IKVLauncher/server`), mode0700:

| Path | Content |
| --- | --- |
| `secrets/` | Mode0600 files: a new owned **development** CA (`tls-ca`, `development-ca-key`, 10 years, `CN=IKV LAUNCHER LOCAL CA - NOT PRODUCTION`), a 30-day `tls-cert`/`tls-key` with SANs `localhost`, `127.0.0.1` (LAN mode: + every up non-loopback IPv4 except `docker*`/`br-*`/`virbr*`/`veth*`, the host name and the optional LAN DNS name), `client-ca.pem` (public copy for clients/browsers) and `dsn`. Same material as `deploy/prepare-secrets.py --ownership current`; the leaf is reissued from the retained CA at each start when it expires within 2 days or its SANs differ from the current access mode/addresses (e.g. a new DHCP address), so clients keep trusting the same CA. |
| `pg/`, `sock/` | A new cluster: `listen_addresses=''`, Unix socket only in the0700 `sock/` with `unix_socket_permissions=0700`. `pg_hba.conf` allows only the OS user as superuser by `peer` and role `ikv` by SCRAM; `ikv` is NOSUPERUSER/NOCREATEDB/NOCREATEROLE and owns database `ikv`, with a random 256-bit password that exists only in `dsn` (sent to `psql` on stdin). |
| Windows (wave 19, NONCANONICAL) | Same layout under `%LOCALAPPDATA%\IKVCommunity\IKVLauncher\server` with no `sock/`: the packaged EDB PostgreSQL 16 (`bin\pgsql`) listens on `127.0.0.1` only, on a port the OS reports free at creation; `pg_hba.conf` allows superuser `postgres` (random password in `secrets\pg-superuser`, `initdb --pwfile`) and role `ikv` by SCRAM over `127.0.0.1/32`; `dsn` is `host=127.0.0.1 port=N`. Certificates come from the packaged `openssl.exe` (`OPENSSL_CONF=NUL`). No mode/owner checks: the per-user profile ACL is the boundary. The supervisor is the launcher re-run detached (`--daemon 1`, password via the one-shot `secrets\bootstrap-password`), locks are `LockFileEx`, children run in a kill-on-close job object, **Durdur** writes `stop.request`, and auth/world stop on stdin EOF (`--stop-on-stdin-eof`) so the world saves characters. Wine smoke: `analysis/porting-wave19/WinServer/wine_host_smoke.py`. |
| `backups/` | `ikv-auth --backup-dir` (pg_dump, mode0600). |
| `host.json` | Non-secret settings: name, bind (`127.0.0.1` = this PC only, `0.0.0.0` = LAN; servers provisioned before wave 17 with one interface address keep it and their SANs), host (LAN: the optional `lan_name`, else the lowest-metric default-route IPv4; written by the supervisor), SANs, ports, bot count, administrator username, `keep_alive`, `autostart`. |
| `state.json`, `server.log` | Written by the supervisor: phase (`starting`, `database`, `auth`, `register`, `bootstrap`, `world`, `running`, `stopping`, `stopped`, `failed` + Turkish message), pid and actual ports; service output (rotated at 4 MiB). |
| `supervisor.lock`, `launcher.lock` | `flock` locks: the supervisor holds `supervisor.lock` exclusively (content: its pid); every open launcher holds `launcher.lock` shared. |

**Supervisor.** The server runs in one detached process, `ikv-launcher --host-supervisor DIR --asset-root PATH` (inside an AppImage `"$APPIMAGE" --host-supervisor …`, so it has its own mount). It daemonizes (double fork, `setsid`), takes `supervisor.lock` (a second start fails with "zaten çalışıyor"), starts PostgreSQL, `ikv-auth` (management always `127.0.0.1`), then `ikv-server` (`--bots N`). On first creation the administrator password arrives on the supervisor's stdin only: it registers that account over the pinned CA, restarts auth once with `--admin-bootstrap USERNAME` and records `admin`. Services are its children with `PR_SET_PDEATHSIG`. A world exit 75 (management **restart**) relaunches the world; exit 0 (management **shutdown**) stops the server; crashes after the first successful start are restarted up to 3 times in 10 minutes. SIGTERM (**Durdur**) stops world, then auth (SIGTERM, exit0), then `pg_ctl stop -m fast`, and records `stopped`.

**Başlatıcı kapanınca da çalışsın** (`keep_alive`, off by default): without it the supervisor stops within about a second once no launcher holds `launcher.lock` (also after a launcher crash), and a closing launcher waits for that clean stop. With it the server keeps running; the next launcher sees the held `supervisor.lock`, reads `state.json` and reattaches (status chip, profile, log tail, panel). The pid is only signalled while its lock is held, so a reused pid is never killed. **Başlatıcı açılınca yerel sunucuyu otomatik başlat** (`autostart`) starts a stopped server when the launcher opens.

**Sunucu Yönetimi** (the same dialog once an administrator exists) is a native client of the existing management API at `https://127.0.0.1:<admin-port>`. It trusts exactly `secrets/client-ca.pem` (Qt CA list replaced, peer verification on; no system store, no bypass), so no browser or CA import is needed. Status without login comes from `/v1/status` (online, players); everything else needs a real administrator session from `/v1/login` on the management port (prefilled after creation; otherwise **Yönetici girişi** with the password), kept in memory only:

- **Durum**: state and addresses, uptime, online players, bots, zones, tick/last tick time/Hz/connections, memory, accounts/characters/sessions, restart countdown; **Başlat**, **Durdur**, **Yeniden başlat** (management `control` restart with an in-game announcement delay; without a session a full stop + start) and **Geri sayımı iptal et**; the keep-alive and auto-start options; **LAN'a açık** switches between this PC only and LAN at any time (a running server is stopped and started again; management stays `127.0.0.1`); the addresses line lists the `https://IP:port` origins other PCs use as their server profile and the TCP ports a firewall must allow (when `/etc/ufw/ufw.conf` says `ENABLED=yes`, it says ufw is on; the rules themselves need root to read); **CA dosyasını kaydet** saves the public `client-ca.pem` (never a key) for the other PC's profile; `ikv-launcher --host-lan on|off DIR` is the same switch for scripts, applied at the next start; **Yönetim paneli (tarayıcı, gelişmiş)** still opens the web dashboard (trust `client-ca.pem` in that browser explicitly for this owned issuer only); **Sunucuyu sil**.
- **Oyuncular**: announcement (`world announce`), online players with **Oyundan at** (`world kick`) and **Hesabı yasakla** (`accounts/{id}/ban`, reason + 1 h/1 day/7 days/permanent, confirmation), and the **live bot count** (`bots spawn` ≤50 / `despawn` ≤64 ids per command; also stored for the next start).
- **Hesaplar**: account search with role, ban state/reason and Plus; ban/unban.
- **Oranlar**: experience/loot/regeneration rates (10–1000 %) and open zones (`settings`).
- **Yedekler**: **Yedek al** (`backups` POST), the list, and **Seçili yedeği geri yükle…**: after confirmation the server is stopped, restored offline with `ikv-auth --dsn-file … --restore-backup FILE` (the only restore path the API allows) and started again.
- **Kayıt**: `server.log` (supervisor and service output) plus panel actions.

API errors are shown in Turkish (known codes mapped, server messages otherwise; connection/TLS/timeout failures explained). **Sunucuyu sil** (stopped server, confirmation required) stops a leftover cluster and permanently deletes this server's accounts, characters, database, backups and keys, and resets the Localhost profile. Docker Compose is not driven by the launcher; use the section below for it. For an internet-facing server use the direct/Compose/systemd deployments, not the launcher.

Owned proof: `python3 analysis/host_server_smoke.py --build build-dir --original /absolute/genuine/app/base --output-dir NEW_DIR [--bots N]` reflink-stages `objects.dat` and the `zoneNN`/`zoneNN_height` support files, starts its own Xvfb with a disposable HOME/XDG profile and a free base port (never 7000/7443/7444), and runs `launcher-gui-smoke` in `host-server` mode: install opt-in + real import, guided creation with the requested account port deliberately occupied, all steps ✓, mode0700/0600 and no-secret-in-log checks, Unix-socket-only PostgreSQL, automatic launcher login + panel admin session, browser dashboard over the pinned CA, admin role on management and 404 on the player port, live status, live bot change, broadcast consumed by the world, backup, restore, keep-alive detach and reattach to the same supervisor, admin re-login, stop, auto-start, closing the launcher stops a non-kept server, and delete. Evidence and screenshots: `analysis/porting-wave10/HostControl/`.

## Docker Compose

[`compose.yaml`](../compose.yaml) has real `database`, `auth` and `world` services. The image is built from an allowlist of clean-room source/config/migrations/dashboard files. Original archives, installer, tools, analysis, credentials and build outputs are excluded by [`.dockerignore`](../.dockerignore).

Set `IKV_ASSET_ROOT` to an absolute, legitimate local asset directory **before** rendering/starting Compose. Its world bind is read-only at `/srv/ikv/original`, rejects a missing source path (`create_host_path:false`), and the entrypoint passes `--asset-root /srv/ikv/original` with installed `config/gameplay.json`. Auth does not mount original assets. UID10001 must be able to traverse the mounted directory and read the selected files. Do not chmod/chown the original installation to achieve this.

For the shipped Source0/1 backend, the disposable helper stages exactly five genuine files: `objects.dat` **607,138,652bytes**, `zone00.bin` and `zone01.bin` **4,194,304bytes each**, and `zone00_height.bin` and `zone01_height.bin` **16,777,216bytes each** (**41,943,040bytes for the four grids**). It uses `cp --reflink=auto` with real-copy fallback, newly owned mode0755 directories and mode0644 copies, no hardlinks. Source hashes and original device/inode/ownership/mode/size/mtime/link metadata remain unchanged. This is a legitimate local fixture copy, **not redistribution**, and is not enough for a graphical client's textures/models/audio. Other configured Source support IDs require their corresponding genuine grids. Keep all originals outside image/build/release contents.

For an **owned development deployment only**, the real provisioning tool creates a new private directory, random independent passwords, DSN and short-lived local development CA/service certificate with explicit SANs:

```sh
python3 deploy/prepare-secrets.py --directory deploy/secrets --dns localhost --ip 127.0.0.1
```

It refuses existing paths, prints no password/private-key or certificate contents, and sets real host ownership using root or a short-lived explicitly owned Docker helper with no network and only the new directory mounted. The helper is removed; only CHOWN/FOWNER/DAC_OVERRIDE capabilities are granted. Before the ownership handoff, it exports the public issuer to `client-ca.pem`, owned by the invoking user with mode0600. The directory remains current-user-owned0700, service `tls-ca` remains UID10001 mode0400, and `development-ca-key` remains current-user-owned0600; never distribute the key or use this development issuer as public/production trust. `--ownership current` instead produces mode0600 native disposable files, not Compose UID mappings. Additional `--dns`/`--ip` values are only for deliberately owned service names/interfaces.

For an existing owned deployment lacking the host/client copy, export only its existing public issuer:

```sh
python3 deploy/prepare-secrets.py --directory deploy/secrets --export-client-ca
curl --fail --cacert deploy/secrets/client-ca.pem https://localhost:7443/v1/health
```

Export-only requires an existing nonsymlink directory owned by the invoking user with mode0700. It never recreates the directory, replaces an existing `client-ca.pem`, regenerates credentials, or weakens source permissions. If `tls-ca` is unreadable, the short-lived explicitly owned Docker helper uses a read-only directory mount, no network, read-only root filesystem and only DAC_OVERRIDE after dropping all capabilities; its fixed command reads only `/secrets/tls-ca`. Public PEM output is bounded and parsed by OpenSSL before exclusive mode0600 creation; certificate contents are not printed. `--ownership-image` selects the owned helper image if needed.

For production, provision approved TLS material independently and prepare an ignored private `deploy/secrets/` directory containing:

| File | Content / required rootful-Docker host ownership |
| --- | --- |
| `db-password` | Independent PostgreSQL **postgres superuser** bootstrap password, root-owned0600; never used by auth/world. |
| `app-db-password` | Independent application password, UID/GID999:999 mode0400, read by PostgreSQL initialization as its postgres user. |
| `dsn` | `host=database port=5432 dbname=ikv user=ikv password=APP_PASSWORD`; UID/GID10001:10001 mode0400. |
| `tls-cert` | Service certificate with localhost/127.0.0.1 SANs (plus any explicitly configured LAN hostname); UID/GID10001:10001 mode0400. |
| `tls-key` | Matching private key; UID/GID10001:10001 mode0400. |
| `tls-ca` | Issuer trust chain; UID/GID10001:10001 mode0400. |
| `client-ca.pem` | Public issuer copy for host/client/browser trust; invoking-user-owned mode0600, not mounted as a service secret. |

Use random independent passwords and copy existing approved TLS material, not a production key from a test fixture. Keep the enclosing directory private. Compose file-backed secrets are **bind mounts**: declaring `uid`, `gid`, `mode` does not change host ownership. Set the real file ownership, for example:

```sh
sudo chown 10001:10001 deploy/secrets/dsn deploy/secrets/tls-cert deploy/secrets/tls-key deploy/secrets/tls-ca
sudo chmod 0400 deploy/secrets/dsn deploy/secrets/tls-cert deploy/secrets/tls-key deploy/secrets/tls-ca
sudo chown 999:999 deploy/secrets/app-db-password
sudo chmod 0400 deploy/secrets/app-db-password
sudo chown root:root deploy/secrets/db-password
sudo chmod 0600 deploy/secrets/db-password
```

This exact numeric ownership is for the specified rootful images; rootless Docker user-namespace mappings require a separately configured host mapping, not world-readable secrets. The rootless configuration is not claimed validated. `IKV_SECRETS_DIR` may select a private external directory. The service entrypoint copies readable secrets to its UID10001-owned private `/run/ikv` tmpfs and creates the required mode0600 DSN. No secret is baked into the image or put in process argv/environment.

On a new PostgreSQL named volume, `deploy/init-database.sh` creates `ikv` as NOSUPERUSER/NOCREATEDB/NOCREATEROLE and makes it owner of database `ikv`. It reads the application secret using psql's input, not password argv. An existing database volume is **never silently reinitialized**: changing secret files alone does not rotate its stored passwords. Perform explicit password rotation and coordinated DSN changes under maintenance.

```sh
export IKV_ASSET_ROOT=/absolute/path/to/owned/readable/original
docker compose config --quiet
docker compose build
docker compose up -d --wait --wait-timeout 90
docker compose ps
docker compose logs auth world
```

Default host publication is loopback7443 and7000. PostgreSQL has **no published port**, is only on the internal backend network, and admin7444 binds container loopback with **no published port**. To deliberately serve an owned LAN, set `IKV_BIND` to its specific interface address and configure firewall/TLS SANs; do not publish management or PostgreSQL. Internet exposure still requires operator-reviewed ingress, certificates, rate policies and security testing—not merely `0.0.0.0`.

Healthchecks: PostgreSQL `pg_isready`; auth actual CA-verified `GET /v1/health` with a database query; world CA/SAN-verified TLS handshake. World health proves listener/certificate readiness **only**, not a session, gameplay correctness or heartbeat. Gameplay/session/ownership checks are in the integration suite. Docker does not automatically restart merely unhealthy containers. Both native services have `restart:on-failure`; world management `restart` exits75 and is restarted, while intentional `shutdown` exits0 and remains stopped. Resource/PID limits, read-only filesystems, private tmpfs, dropped capabilities, no-new-privileges and bounded logs are configured.

The database and backups are named persistent volumes. The image creates `/var/lib/ikv/backups` owned10001 before volume copy-up; do not attach an incorrectly owned preexisting backup volume. `docker compose down` removes containers/networks but preserves volumes. **Never use `down -v` on user data.**

### Optional disposable fresh-project acceptance

Use the dedicated helper below rather than running destructive cleanup against an existing community project. It requires a fresh image tag/ID built from current backend inputs, a **new unused project name**, distinct unused loopback private ports and owned fixture secrets/assets. The helper never builds or pulls images; preload the owned `postgres:17-bookworm` image, which also supplies its network-disabled ownership helper. The host smoke requires Python3, OpenSSL, Docker Compose and the current actual native `ikv-client`; that client is not an image compile dependency. Choose unused local ports before running:

```sh
proof_id="ikv-source-$(date +%s)-$$"
docker build --target runtime --tag "ikv-source-backend:${proof_id}" .
python3 analysis/compose_native_smoke.py \
  --owned-stack --project "$proof_id" --image "ikv-source-backend:${proof_id}" \
  --client "$PWD/build-full/ikv-client" --asset-root "$PWD/original" \
  --auth-port 17443 --world-port 17000 --output "analysis/${proof_id}.compose.json"
```

Replace `"$PWD/original"` with your absolute legitimate root if different; do not use an existing project or reuse an output path. The helper records the inspected fresh image ID and verifies the running auth/world images match it, renders the production Compose definition with only the image tags and private published ports changed, provisions new secrets/public CA, stages the genuine five files itself, and starts with `--no-build --pull never`. Its cleanup removes only its created project containers/networks/volumes and staged files after bind consumers stop; it retains originals and the parent-built image.

For an already running **owned** project, omission of `--owned-stack` preserves services/volumes and the created synthetic accounts/characters. The default project is `ikv-full-proof`; select the actual project, public CA, already mounted asset root and published ports explicitly:

```sh
python3 analysis/compose_native_smoke.py --project YOUR_EXISTING_OWNED_PROJECT \
  --client "$PWD/build-full/ikv-client" --ca /absolute/private/secrets/client-ca.pem \
  --asset-root /absolute/already/mounted/asset-root \
  --auth-port 7443 --world-port 7000 --output analysis/new-existing-compose-proof.json
```

This existing-project mode is **nondestructive by default**, not permission to delete user data. For standalone local asset preparation without Docker/service startup, `python3 analysis/compose_native_smoke.py --asset-root "$PWD/original" --prepare-asset-root /absolute/NEW_OWNED_ASSET_ROOT` exclusively creates the mode0755 staging root and mode0644 five-file copies described above. Its parent directory must already exist; those copies are retained until the operator removes only that owned fixture after all bind consumers stop.

Current primary evidence `analysis/ikv-source-current-a3b89e0d8fd1.compose.json` records the fresh image `sha256:da44ed6f12743edd476144df14e497cc95a92eb55fcc158d261ef7c0bd728ccc`, auth/world UID10001/read-only-root/Cap0/NNP, private admin/internal DB and observed application role `ikv` with **superuser/createdb/createrole/bypassrls/replication all false**. Two real native peers transferred to Source1`[-109500,0]` with140snapshots each/**132shared ticks**, persisted while live, exited0/released leases, then reentered after **world-only restart** with40snapshots each/**40shared ticks**; auth/database starts remained unchanged. Actual UID10001 could read all five staged files; staged/original metadata and SHA256 remained unchanged. Project containers/volumes/networks, fixture secrets and staged files were removed; original root and parent-built image were retained.

The parent retained `.inputs-before.sha256`/`.inputs-after.sha256` (**78inputs**) and `.native-before.sha256`/`.native-after.sha256` (**3native binaries**) under `analysis/ikv-source-current-a3b89e0d8fd1.*`, unchanged throughout the combined99.87second Compose/user-systemd proof. Later Qt acceptance-fixture edits occurred after that completed guard interval and are not Dockerfile COPY inputs. A subsequent Vulkan RT client-only retirement fix changed the full client, not backend auth/world/config/image or GL-only client; this backend deployment scope remains valid, but its recorded client hash does not identify that later client. Signed-launcher and temporal-rendering results are independent evidence in [progress](PROGRESS.md#current-parent-integration-evidence), not inherited from deployment. The fresh unchanged-source/ELF ASan+UBSan suite passed14/14 in192.35seconds wall/192.20seconds CTest (`artifact://1009`), retaining leak/error halting settings; the earlier graceful-stop/world-poll timeout is historical, intermittent and unattributed, not erased or explained by a production fix. Current full-native/GL-only matrix acceptance is tracked separately in progress; no container-restore pass is claimed here.

Private management from the host uses an explicitly authorized local operation inside the auth container (or a deliberately managed private tunnel), e.g.:

```sh
docker compose exec auth curl --fail --cacert /run/ikv/tls-ca https://localhost:7444/admin
```

Dashboard/account operations additionally require an administrator session, never a URL token. Pass authorization using a protected stdin/configuration mechanism; do not put it into process-visible curl argv. There is no arbitrary shell-command REST endpoint.

## Backup and restore

Admin backups execute real `pg_dump` with DSN secrets offargv; files go to the private backup directory/volume. A success result is tied to the dump exit status and completed file, not an invented backup marker. Copy backups to a separate protected storage system under operator policy; the Compose volume alone is not disaster recovery. Dump tools must match or exceed the PostgreSQL server major version.

For maintenance restore, stop auth/world; select an explicitly owned backup, and invoke the implemented offline command as the service user:

```sh
build/ikv-auth --dsn-file /private/disposable-restore-dsn --restore-backup /private/backup.dump
```

First restore into an **owned disposable database**, verify accounts/characters/state and row counts, and only then plan any production restore. Restore is destructive to the selected database; do not execute against an unknown/user database or confuse a test with authorization to replace production. The registered isolated suite's plain-SQL restore is separate from the current Compose **22,486byte custom dump/56TOC entries**, whose `restore_tested` is **false**; successful dump creation/TOC inspection is not container recovery acceptance. See [test evidence](network/NETWORK_TESTING.md).

## Systemd alternative

The supplied [`ikv-auth.service`](../deploy/ikv-auth.service) and [`ikv-server.service`](../deploy/ikv-server.service) are **system** units replacing the old anonymous user unit. They assume binaries/data installed under `/usr/local`, PostgreSQL's distro service named `postgresql.service`, a deliberately created system user/group `ikv`, private root-owned `/etc/ikv/{dsn,tls-cert,tls-key}`, and provisioned PostgreSQL. Adapt only the explicit PostgreSQL unit name on distros with versioned units; do not silently remove the database dependency.

`LoadCredential` securely supplies secrets; mode0400 systemd credentials are copied by `/usr/bin/install -m0600` into private service-owned RuntimeDirectories before native DSN validation. Auth's StateDirectory gives private `/var/lib/ikv` backup storage. There are no DSN/password argv or environment values. Both ports remain loopback by default; management never shares the player listener.

The world unit explicitly passes `--asset-root ${IKV_ASSET_ROOT}`. Its default is `/srv/ikv/original`; `/etc/ikv/world.env` may set `IKV_ASSET_ROOT=/absolute/operator/provisioned/path`. Provision legitimate files readable by `ikv` without modifying originals. The path must be accessible under `ProtectHome=yes`/`ProtectSystem=strict`; a home-directory asset root is not silently exposed by weakening those protections. Keep the installed Source0/1 gameplay config and required support files consistent.

After deliberate provisioning and installation, an operator may run:

```sh
sudo install -m644 deploy/ikv-auth.service deploy/ikv-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ikv-auth.service ikv-server.service
sudo systemctl status ikv-auth.service ikv-server.service
sudo journalctl -u ikv-auth.service -u ikv-server.service
```

Units request strict filesystem/home/kernel protection, private tmp, empty capability set, no-new-privileges, address-family/resource limits and on-failure restart. Current parent evidence `analysis/ikv-source-current-a3b89e0d8fd1.user-systemd.json` records actual owned current-user/unique-port/private-PostgreSQL adaptation: LoadCredential→0600RuntimeDSN, narrowly bound credential/runtime/state/asset aliases with strict protections retained, two native Source1 transfers with simultaneous snapshots, restart/persisted one-character reentry, raw support samples and unchanged genuine original asset/config metadata+SHA256. The fixture's PostgreSQL bootstrap role is a **superuser**, unlike Compose's observed least-privilege application role. Owned units/directories were stopped/removed, never enabled. **Unmodified privileged root units/production deployment are not claimed exercised**; operators must verify exact host sandbox/cgroup and recovery behavior.

### Optional owned user-systemd Source acceptance

This helper requires an existing **non-root** user's active systemd manager/bus, owned `XDG_RUNTIME_DIR`, Python3/OpenSSL/PostgreSQL tools and current native auth/server/client binaries. It does not start a user manager, enable units or install system services. Use a readable current-user-owned absolute original root without whitespace, quotes, backslashes, colons or systemd specifiers, and a new evidence output:

```sh
python3 analysis/native_systemd_smoke.py --source "$PWD" --build-dir "$PWD/build-full" \
  --asset-root "$PWD/original" \
  --output "analysis/source-user-systemd-$(date +%s)-$$.json"
```

`--asset-root` is optional **only for the helper**: omitting it deliberately selects a disposable assetless mechanics config, not the production Source0/1 config or production server defaults. The command above keeps the unchanged installed production gameplay, binds originals read-only into its unique strict sandbox, uses OS-assigned loopback ports and a disposable Unix-socket-only PostgreSQL cluster. It stops/removes only its owned units, links, runtime/state/install directories and database; originals remain untouched. Its bootstrap PostgreSQL superuser fixture is not a production role recommendation. The current proof records two simultaneous native Source1 transfers, persisted restart and **one-character reentry**, not a two-character systemd reentry result.

## Offline tools and distribution

`ikv-assets` and `ikv-original-inspect` are separate read-only research utilities, not network services and not included in the server image. [README](../README.md) gives their commands and original-wire limits. Asset-free release artifacts and Ed25519 signed updates require the user's explicit repository/channel/trust root; no public release or production signing key has been created by this deployment definition. Original installation/assets/settings remain outside executable release replacement.
