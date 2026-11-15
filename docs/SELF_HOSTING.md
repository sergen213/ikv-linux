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

### Several realms and shards (NONCANONICAL, wave 30)

The original game listed several independent servers in its launcher (`launcher.exe` `0x0048dbe0`; see `analysis/porting-wave30/Realms/original_servers.md`). Here a **realm** is a row in the shared database (characters and their names belong to one home realm; guilds and trading are cross-realm) and a **shard** is one `ikv-server` process serving one realm with at most 32 players. A realm may have several shards (layers); login picks the least-loaded live shard of the character's home realm. Everything runs against **one** `ikv-auth` and **one** PostgreSQL database; there is no extra coordinator service. One realm with one shard (the commands above, `--realm` defaulting to 1, the seeded `Eminönü`) behaves exactly as before.

1. Create realms in the management panel's **Sunucular** tab (or `POST /v1/admin/realms {"action":"create","name":"Galata","sort":1}`, [AUTH_DATABASE](AUTH_DATABASE.md)) with a name, kind, shard count (0–16), capacity per shard (1–32) and **Hemen başlat**. The tab lists every realm with its state (Çalışıyor / Başlatılıyor / Durduruldu), desired vs running shard count, launcher population label, character count and the shards seen within the last hour (instance, address, players/capacity, heartbeat age). **Düzenle** edits name, kind, shards and capacity inline; **Başlat** / **Durdur** enable or disable the realm; **Yeniden başlat** restarts the realm or one shard after an announced countdown (players are saved); every layout shard has its own **Parça N** row with **Durdur** / **Başlat** (`stop_shard` / `start_shard`: the shard's players are saved and only that shard stays down until **Başlat**, applied by a launcher-hosted server within seconds; on a manual installation stop or start that unit yourself); **Sil** deletes a realm without characters (not realm 1). A launcher-hosted server applies the stored layout within seconds; when a realm still runs fewer shards than desired after 30 s the tab shows which `ikv-server@REALM-SHARD` units to start on a manual installation (NONCANONICAL, wave 31).
2. Start one `ikv-server` per shard, each on its **own world port**, all with the same certificate, DSN file and gameplay file:

```sh
# realm 1 (Eminönü): two shards (layers)
build/ikv-server --bind 0.0.0.0 --port 7000 --realm 1 --public-host game.example.org --capacity 32 \
  --cert /private/tls-cert --key /private/tls-key --dsn-file /private/dsn --gameplay config/gameplay.json --asset-root /owned/readable/original
build/ikv-server --bind 0.0.0.0 --port 7001 --realm 1 --public-host game.example.org --capacity 32 ...same files...
# realm 2 (Galata): one shard on another machine
build/ikv-server --bind 0.0.0.0 --port 7000 --realm 2 --public-host galata.example.org --capacity 32 ...same files...
```

- `--realm ID` (1–32767) must name an existing realm; the shard refuses to start otherwise.
- `--public-host NAME` is the address players connect to for this shard. Empty (the default) tells clients to use the account service's HTTPS host, which is right only when the shard runs on that host. The certificate must cover every public host name.
- `--capacity N` (1–32, default 32) is the advertised player limit used for shard selection and the population label (`Sakin`/`Normal`/`Yoğun`/`Dolu`).
- Each shard writes a heartbeat row every second; a row older than 15 s is never chosen, so a stopped shard drops out by itself. A realm without a live shard shows as `Kapalı` in the launcher and the panel, and entering it returns `realm_offline`.
- Open every shard's world port in the firewall, plus the account port (7443). The management port (7444) stays private.
- Administrator `world_commands` reach the right shard: a command about a character goes to the shard leasing it, world-wide commands (announce, settings, **restart**, **shutdown**, …) are written once per live shard (`world_commands.target_instance`, migration 009); the panel's player tables list the players of every live shard.

The launcher lists the realms of the selected server profile after login and passes the chosen realm to the client ([LAUNCHER_UPDATES](LAUNCHER_UPDATES.md#using-the-launcher)). The launcher's own **Sunucu Kur** hosting runs any number of realms and shards too (**Diyarlar** tab, below).

**systemd:** [`ikv-server@.service`](../deploy/ikv-server@.service) is a templated world unit whose instance name is `REALM-SHARD`; `/etc/ikv/world-REALM-SHARD.env` sets `IKV_REALM`, `IKV_WORLD_PORT`, optionally `IKV_CAPACITY` (default 32), `IKV_BIND` (default `127.0.0.1`) and `IKV_SERVER_ARGS` (e.g. `--public-host game.example.org`, `--bots 0`). Examples for realm 1 × 2 and realm 2 × 1 are in [`deploy/world-env/`](../deploy/world-env). The single `ikv-server.service` equals `ikv-server@1-1` on port 7000; run one or the other.

```sh
sudo install -m644 deploy/ikv-server@.service /etc/systemd/system/
sudo install -m600 deploy/world-env/world-1-1.env deploy/world-env/world-1-2.env deploy/world-env/world-2-1.env /etc/ikv/
sudo systemctl daemon-reload
sudo systemctl enable --now ikv-server@1-1.service ikv-server@1-2.service
# realm 2 must exist first (management panel or POST /v1/admin/realms), then:
sudo systemctl enable --now ikv-server@2-1.service
```

**Docker Compose:** the `realms` profile of [`deploy/compose.release.yaml`](../deploy/compose.release.yaml) adds `world-1-2` (second shard of realm 1, port 7001) and `world-2-1` (realm 2, port 7002) beside the base `world` (realm 1 shard 1, port 7000): `docker compose -f compose.yaml -f deploy/compose.release.yaml --profile realms up -d`. Each container listens on the port it publishes, because the shard registers that port for clients; set `IKV_PUBLIC_HOST` to the address players reach the host at. The entrypoint reads `IKV_REALM`, `IKV_WORLD_PORT`, `IKV_CAPACITY` and `IKV_PUBLIC_HOST`.

## Launcher-hosted local server (Sunucu Kur / Sunucu Yönetimi)

The Qt launcher's header **server chip** (`Sunucu Kur` until a server exists, then `● Sunucu: Çalışıyor · N oyuncu`, `○ Sunucu: Kapalı`, `◌ Başlatılıyor…` or `✕ Hata`; also opened by the install page's opt-in **Bu bilgisayarda yerel sunucu da kur** after a successful import) opens one dialog. Implementation: `src/launcher_server.cpp`. It needs the imported original assets (the world reads `<install-root>/assets`), `ikv-auth`/`ikv-server` beside the launcher (source build, signed release or AppImage), the installed or source-tree `config/gameplay.json`, `database/migrations` and `web`, `openssl`, and PostgreSQL server tools (`initdb`, `pg_ctl`, `psql` from `PATH` or Debian's newest `/usr/lib/postgresql/N/bin`; the AppImage bundles them). Missing tools are reported in Turkish with the install command.

**Guided creation.** One form: **İsim**, **Yönetici hesabı** (defaults to the OS user name), **Yönetici parolası** (typed, 12+ characters; no default), **Bot sayısı** (default 10, NONCANONICAL server bots) and **Yerel** / **LAN** (all IPv4 interfaces; switchable later). **Gelişmiş** shows the ports (default 7443 account, 7444 management, 7000 world; 0 = OS-assigned) and an optional LAN DNS name. **Sunucuyu Kur ve Başlat** runs the listed steps with ✓/●/✗ marks: requirements, free ports (a requested port that is in use moves to the next free one, logged), keys and certificates, the private PostgreSQL, account service, administrator account, world, launcher login. Afterwards the **Localhost** profile points at the actual auth origin and `secrets/client-ca.pem`, the launcher logs in with the new administrator (play page), and the panel opens an administrator session on the management API. A failed creation leaves `host.json` without `admin`; the form then keeps the access mode and pressing the button again retries (registering, or proving the existing account by login, then bootstrapping).

Everything lives in Qt's AppLocalData (`~/.local/share/IKVCommunity/IKVLauncher/server`), mode0700:

| Path | Content |
| --- | --- |
| `secrets/` | Mode0600 files: a new owned **development** CA (`tls-ca`, `development-ca-key`, 10 years, `CN=IKV LAUNCHER LOCAL CA - NOT PRODUCTION`), a 30-day `tls-cert`/`tls-key` with SANs `localhost`, `127.0.0.1` (LAN mode: + every up non-loopback IPv4 except `docker*`/`br-*`/`virbr*`/`veth*`, the host name and the optional LAN DNS name), `client-ca.pem` (public copy for clients/browsers), `dsn` and `host-token` (NONCANONICAL wave 31: 64 random hex characters, created by the supervisor at its first wave 31 start, also for older installs; `ikv-auth --host-token-file` accepts it as `Authorization: Host <token>` for `/v1/host/layout` on the management listener only). Same material as `deploy/prepare-secrets.py --ownership current`; the leaf is reissued from the retained CA at each start when it expires within 2 days or its SANs differ from the current access mode/addresses (e.g. a new DHCP address), so clients keep trusting the same CA. |
| `pg/`, `sock/` | A new cluster: `listen_addresses=''`, Unix socket only in the0700 `sock/` with `unix_socket_permissions=0700`. `pg_hba.conf` allows only the OS user as superuser by `peer` and role `ikv` by SCRAM; `ikv` is NOSUPERUSER/NOCREATEDB/NOCREATEROLE and owns database `ikv`, with a random 256-bit password that exists only in `dsn` (sent to `psql` on stdin). |
| Windows (wave 19, NONCANONICAL) | Same layout under `%LOCALAPPDATA%\IKVCommunity\IKVLauncher\server` with no `sock/`: the packaged EDB PostgreSQL 16 (`bin\pgsql`) listens on `127.0.0.1` only, on a port the OS reports free at creation; `pg_hba.conf` allows superuser `postgres` (random password in `secrets\pg-superuser`, `initdb --pwfile`) and role `ikv` by SCRAM over `127.0.0.1/32`; `dsn` is `host=127.0.0.1 port=N`. Certificates come from the packaged `openssl.exe` (`OPENSSL_CONF=NUL`). No mode/owner checks: the per-user profile ACL is the boundary. The supervisor is the launcher re-run detached (`--daemon 1`, password via the one-shot `secrets\bootstrap-password`), locks are `LockFileEx`, children run in a kill-on-close job object, **Durdur** writes `stop.request`, and auth/world stop on stdin EOF (`--stop-on-stdin-eof`) so the world saves characters. Wine smoke: `analysis/porting-wave19/WinServer/wine_host_smoke.py`. |
| `backups/` | `ikv-auth --backup-dir` (pg_dump, mode0600). |
| `host.json` | Non-secret settings: name, bind (`127.0.0.1` = this PC only, `0.0.0.0` = LAN; servers provisioned before wave 17 with one interface address keep it and their SANs), host (LAN: the optional `lan_name`, else the lowest-metric default-route IPv4; written by the supervisor), SANs, ports, bot count, administrator username, `keep_alive`, `autostart`, `layout_in_db`. A wave 30 `host.json` may still carry `realms` (`[{"id":1,"shards":2,"capacity":32},…]`); since wave 31 the realm layout lives in the database and this key is written there once (`PUT /v1/host/layout`) at the first wave 31 start, after which `layout_in_db` is `true` and `realms` is ignored. |
| `state.json`, `server.log` | Written by the supervisor: phase (`starting`, `database`, `auth`, `register`, `bootstrap`, `world`, `running`, `stopping`, `stopped`, `failed` + Turkish message), pid and actual ports (`world_port` = the first shard), `shards` (`realm`, `index`, `port`, `pid`, `capacity`, `state` `starting`/`running`/`failed`/`stopped`/`held`, `problem`), `layout` (the last database layout: `id`, `name`, `shards`, `capacity`, `enabled`, `held_shards`) and `layout_problem`; service output (rotated at 4 MiB). |
| `supervisor.lock`, `launcher.lock` | `flock` locks: the supervisor holds `supervisor.lock` exclusively (content: its pid); every open launcher holds `launcher.lock` shared. |

**Supervisor.** The server runs in one detached process, `ikv-launcher --host-supervisor DIR --asset-root PATH` (inside an AppImage `"$APPIMAGE" --host-supervisor …`, so it has its own mount). It daemonizes (double fork, `setsid`), takes `supervisor.lock` (a second start fails with "zaten çalışıyor"), starts PostgreSQL, `ikv-auth` (management always `127.0.0.1`, `--host-token-file secrets/host-token`), then one `ikv-server` per shard of the realm layout (log prefix `ikv-server[REALM-SHARD]`), each with `--realm`, `--capacity`, its own `--port` and `--shard-index` (realm 1 shard 1 always on the world port, which no other shard takes even while that shard is stopped; every further shard on the next free port above it, skipping the account/management ports and the other shards' ports, and keeping it across restarts). `ikv-server` binds its world port before it opens the database and loads zones; when another process took the port in the meantime it exits with code 98 (`server port in use`), which is not a crash: the shard moves to the next free port (realm 1 shard 1 retries the world port every second), giving up after 20 refusals in a row and, in LAN mode, `--public-host` = the LAN address; the bots (`--bots N`) live on the first shard only. NONCANONICAL wave 31: the layout is the database's (`realms` `shards`/`capacity`/`enabled`, migration 010), read from `GET /v1/host/layout` every 2 seconds, so realms created, resized, stopped or deleted in Sunucu Yönetimi **or the web panel** take effect within about 2 seconds: added and enabled shards start, removed, disabled or deleted ones save and stop, shards in `held_shards` (per-shard **Durdur**) save and stay down until released, a crashed shard restarts, and a shard given up after failures is retried when the layout changes; a fresh install has the database default (realm 1, one 32-player shard). A wave 30 `host.json` `realms` layout is written into the database once (above). A capacity change applies at the shard's next restart. A shard that fails before its first READY, or crashes more than 3 times in 10 minutes, is given up (`state.json` `failed` with the reason) while the others keep running; only a first start in which every shard fails stops the server (once running, the account service stays up even with no shard, e.g. every realm stopped). On first creation the administrator password arrives on the supervisor's stdin only: it registers that account over the pinned CA, restarts auth once with `--admin-bootstrap USERNAME` and records `admin`. Services are its children with `PR_SET_PDEATHSIG`. A shard exit 75 (management **restart**, also the realm/shard **Yeniden başlat** of the panels) relaunches that shard; exit 0 (management **shutdown**, written for every shard) stops it and the server stops after the last one; crashes after the first successful start are restarted up to 3 times in 10 minutes per shard (auth likewise). SIGTERM (**Durdur**) stops every shard in parallel (each saves), then auth (SIGTERM, exit0), then `pg_ctl stop -m fast`, and records `stopped`. **Automatic updates:** every 30 seconds (and at once when a newer launcher writes `upgrade.request`) the supervisor looks for a newer verified release in its install root; it then queues one announced restart per live shard (`world_commands.target_instance`; `host.json` `update_delay_seconds`, default 60; players see the countdown), every shard saves and exits 75 (a shard that has not closed 120 s after the countdown is stopped with SIGTERM, which also saves), and once all have closed the supervisor stops auth and PostgreSQL and hands over to the new release's supervisor (`state.json` phase `upgrading`, then the new `version`); with no shard running it hands over at once. This keeps working with the launcher closed (keep-alive) and repeats for every release ([Launcher updates](LAUNCHER_UPDATES.md#self-update)).

**Başlatıcı kapanınca da çalışsın** (`keep_alive`, off by default): without it the supervisor stops within about a second once no launcher holds `launcher.lock` (also after a launcher crash), and a closing launcher waits for that clean stop. With it the server keeps running; the next launcher sees the held `supervisor.lock`, reads `state.json` and reattaches (status chip, profile, log tail, panel). The pid is only signalled while its lock is held, so a reused pid is never killed. **Başlatıcı açılınca yerel sunucuyu otomatik başlat** (`autostart`) starts a stopped server when the launcher opens.

**Sunucu Yönetimi** (the same dialog once an administrator exists) is a native client of the existing management API at `https://127.0.0.1:<admin-port>`. It trusts exactly `secrets/client-ca.pem` (Qt CA list replaced, peer verification on; no system store, no bypass), so no browser or CA import is needed. Status without login comes from `/v1/status` (online, players); everything else needs a real administrator session, kept in memory only. NONCANONICAL wave 31: when the launcher is logged in on its **Localhost** profile and that profile is this server, the panel reuses that session (no second login; **Çıkış** only stops using it, the launcher stays logged in); otherwise, or when that account is not an administrator, **Yönetici girişi** with the password (`/v1/login` on the management port; prefilled after creation) remains:

- **Durum**: state and addresses, uptime, online players, bots, zones, tick/last tick time/Hz/connections, memory, accounts/characters/sessions, restart countdown; **Başlat**, **Durdur**, **Yeniden başlat** (management `control` restart with an in-game announcement delay; without a session a full stop + start) and **Geri sayımı iptal et**; the keep-alive and auto-start options; **LAN'a açık** switches between this PC only and LAN at any time (a running server is stopped and started again; management stays `127.0.0.1`); the addresses line lists the `https://IP:port` origins other PCs use as their server profile and the TCP ports a firewall must allow (when `/etc/ufw/ufw.conf` says `ENABLED=yes`, it says ufw is on; the rules themselves need root to read); **CA dosyasını kaydet** saves the public `client-ca.pem` (never a key) for the other PC's profile; `ikv-launcher --host-lan on|off DIR` is the same switch for scripts, applied at the next start; **Yönetim paneli (tarayıcı, gelişmiş)** still opens the web dashboard (trust `client-ca.pem` in that browser explicitly for this owned issuer only); **Sunucuyu sil**.
- **Diyarlar** (NONCANONICAL wave 30/31): every realm (`/v1/admin/realms`) with its kind, state (**Çalışıyor** / **Durduruldu** / **Başlatılıyor** / **Hata**, from the supervisor's shards; "— kapalı" for a stopped realm), running / desired shards and players / capacity, and every shard of the supervisor (realm, number, port, players/capacity as registered, state or failure reason). **Diyar ekle…** (name, normal/pvp/rp, shard count 0–16, capacity 1–32 per shard, **Hemen başlat**), **Düzenle…** (the same fields), **Başlat** / **Durdur** (`update enabled`; stopping saves every player of the realm), **Diyarı yeniden başlat** with the **duyuru N sn** countdown (`restart`: announced to the online players, saved, back within seconds) and **Sil…** (after confirmation; realm 1 and realms with characters are refused with the reason) all go to `POST /v1/admin/realms`, so the web panel's **Sunucular** tab shows the same. In the shard table **Parçayı yeniden başlat** (announced `restart` of that instance), **Parçayı durdur** (saves its players; the shard stays down while its realm keeps running) and **Parçayı başlat** (a stopped or failed shard); the stop is the database hold `stop_shard` / `start_shard`, the same as the web panel's per-shard **Durdur** / **Başlat**, so either panel can release it. Without a running server or an admin session every button is disabled and its tooltip says why. At most 64 shards in total.
- **Oyuncular**: announcement (`world announce`), online players (of every shard) with **Oyundan at** (`world kick`) and **Hesabı yasakla** (`accounts/{id}/ban`, reason + 1 h/1 day/7 days/permanent, confirmation), and the **live bot count** (`bots spawn` ≤50 / `despawn` ≤64 ids per command; also stored for the next start).
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

## Automatic updates

Launcher-hosted servers update themselves (above). Dedicated servers run `deploy/ikv-auto-update.sh` periodically; it uses only signed releases and changes nothing when a check fails:

1. `ikv-launcher --update-once --server --install-root /var/lib/ikv-release` checks the channel and installs a newer signed AppImage (manifest, Ed25519 signature, per-channel sequence highwater) into `/var/lib/ikv-release/releases/<version>` — exit 10 when it installed one, 0 when the root's current release is the newest, 1 on failure (the running release is kept).
2. A newly current release is extracted once (`--appimage-extract`, no FUSE needed by the sandboxed services) to `extracted/<version>`, and the `active` symlink is switched atomically.
3. `ikv-auth` restarts (it applies new migrations), then every live shard (`ikv-server.service`, each `ikv-server@REALM-SHARD.service`, each Compose `world*` service) receives its own management **restart** command (`world_commands.target_instance`) with a countdown (`IKV_UPDATE_DELAY`, default 60 s) on behalf of the first administrator account: every online player sees **Sunucu N saniye içinde yeniden başlatılacak. Sunucu vX sürümüne güncelleniyor; karakterleriniz kaydedilir.**, each shard saves every character and exits 75, and systemd (`Restart=on-failure`) or Compose (`restart: on-failure`) starts it again from the new release. Without an administrator account (or a live shard) every shard is restarted by SIGTERM (`systemctl try-restart ikv-server.service 'ikv-server@*.service'`, or `compose restart` of every running `world*` service), which also saves.

**systemd** (root-owned release root; the services keep running as `ikv`):

```sh
sudo install -d -m 0755 /var/lib/ikv-release /usr/local/libexec/ikv
sudo install -m 0755 deploy/ikv-auto-update.sh /usr/local/libexec/ikv/
sudo install -m 0644 deploy/ikv-update.service deploy/ikv-update.timer /etc/systemd/system/
sudo install -D -m 0644 deploy/release-units/ikv-auth.service.d/10-release.conf /etc/systemd/system/ikv-auth.service.d/10-release.conf
sudo install -D -m 0644 deploy/release-units/ikv-server.service.d/10-release.conf /etc/systemd/system/ikv-server.service.d/10-release.conf
# Shards (ikv-server@REALM-SHARD): the same for the template
sudo install -D -m 0644 deploy/release-units/ikv-server@.service.d/10-release.conf /etc/systemd/system/ikv-server@.service.d/10-release.conf
# First release: run the check once with a launcher (a source build's or a downloaded AppImage's).
sudo env IKV_LAUNCHER="/path/to/IKV-Linux-X.Y.Z-x86_64.AppImage ikv-launcher" /usr/local/libexec/ikv/ikv-auto-update.sh
sudo systemctl daemon-reload && sudo systemctl enable --now ikv-update.timer
```

The drop-ins run `/var/lib/ikv-release/active/AppRun ikv-auth|ikv-server` with the release's own `config/gameplay.json`, migrations and web panel; later checks use the active release's launcher. `/etc/ikv/update.env` may set `IKV_UPDATE_DELAY`, `IKV_UPDATE_ARGS` (`--channel`, `--repo`, `--trust`, `--update-ca`, `--release-api`) and `IKV_DSN_FILE` (default `/etc/ikv/dsn`; the password goes to `psql` through `PGPASSWORD`, never argv).

**Docker Compose:** `docker compose -f compose.yaml -f deploy/compose.release.yaml up -d` mounts `/var/lib/ikv-release` read-only and sets `IKV_RELEASE=/opt/ikv-release/active`, which makes `deploy/container-entrypoint.sh` run the release's `AppRun ikv-auth|ikv-server`. Run the script with `IKV_UPDATE_MODE=compose IKV_COMPOSE_DIR=/path/to/checkout` from a host timer or cron; it restarts `auth` and announces to every live shard through `docker compose exec database psql`.

**Windows:** the launcher-hosted server (Sunucu Yönetimi) updates itself exactly as on Linux; the installed launcher installs the signed Windows package and the supervisor hands over to the new release's `bin\ikv-launcher.exe`.

Proof boundary: `tests/update_integration.py` runs `ikv-auto-update.sh` against an owned HTTPS origin with `systemctl`/`psql` stand-ins (install, extraction, `active` switch, auth restart, the queued announced restart, no-op when current, tampered manifest refused with nothing touched); `tests/hosted_update.py` proves the launcher-hosted upgrade with real PostgreSQL/auth/world: by default a wave 30 `host.json` layout (realm 1 on two shards) migrated once into the database plus realm 2 with one shard created over `/v1/admin/realms` and applied live, every shard registered (`/v1/realms`, `/v1/admin/realms`), a SIGKILLed shard restarted on its port, and one connected player per shard who sees the announcement, is saved and whose shard runs the new release (`--single` keeps a pre-wave-30 `host.json`); `analysis/porting-wave31/Control/control_smoke.py` drives the wave 31 Diyarlar controls on a real launcher-hosted server; `systemd-analyze verify` passed for the units with stand-in binaries. The production systemd units, the Compose override and the Windows supervisor handover were not run on a real host by these tests.

## Offline tools and distribution

`ikv-assets` and `ikv-original-inspect` are separate read-only research utilities, not network services and not included in the server image. [Development](DEVELOPMENT.md#offline-original-data-research) gives their commands and original-wire limits. Asset-free release artifacts and Ed25519 signed updates require the user's explicit repository/channel/trust root; no public release or production signing key has been created by this deployment definition. Original installation/assets/settings remain outside executable release replacement.
