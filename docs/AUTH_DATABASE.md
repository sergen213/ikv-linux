# Independent accounts, HTTPS and community administration

These services implement the project's **independent** account/database/API contract; they do not reproduce or contact official authentication endpoints. Use new community credentials, never official-game passwords or imported third-party sessions. Source availability is not a successful deployment or restore test; observed execution results belong in [PROGRESS.md](PROGRESS.md).

## Service configuration

`ikv-auth` requires separate option/value arguments:

```sh
ikv-auth --bind 127.0.0.1 --port 7443 \
  --admin-bind 127.0.0.1 --admin-port 7444 \
  --cert /etc/ikv/server-chain.pem --key /etc/ikv/server-key.pem \
  --dsn-file /etc/ikv/database.dsn \
  --migrations database/migrations --web-root web --backup-dir /var/lib/ikv/backups
```

Defaults are shown for both binds/ports and the migrations/web paths. Backup-directory default is `backups`. Paths are relative to the process working directory unless absolute. Port `0` selects an ephemeral port for owned test environments. Readiness is printed only after both HTTPS listeners bind and the database migrations and bundled assets load:

```text
READY auth_port=7443 admin_port=7444
```

Provide a trusted PEM certificate chain and matching private key; clients must validate the chain and the requested hostname/IP SAN. Never disable verification. TLS 1.0/1.1 and plaintext HTTP are not supported; minimum TLS is 1.2. Protect the key and DSN outside the repository, images, release packages and logs.

The DSN file must be a nonempty regular file, **mode 0600**, owned by the process effective user, not a symlink, and at most 64 KiB. Trailing whitespace is trimmed; embedded NUL is rejected. Put the connection string in that file, not an argument. The startup error reports only the failing stage, never the libpq connection string. A systemd `LoadCredential` file that is mode 0400 must first be copied into a service-owned, mode-0600 runtime file. PostgreSQL must stay on a private socket/network and use a dedicated database role. The shared Store owns parameterized SQL, migrations, bounded pooling, account policy and trusted world persistence; see [DATABASE_SCHEMA.md](DATABASE_SCHEMA.md).

## Administration isolation and bootstrap

The **player** listener may be bound to an explicitly configured public address for community clients. It always returns 404 for `/admin*` and `/v1/admin*`, even when a request comes from loopback. The **separate administration** listener defaults to `127.0.0.1:7444`; its bind and actual remote peer must be loopback, RFC1918 IPv4 or IPv6 ULA. IPv4-mapped IPv6 peers use the corresponding IPv4 classification. Wildcard/public admin binds are rejected. Forwarded-IP headers have no authority. Private reachability does **not** grant an administrator role: Store independently checks the authenticated account on every management REST call.

Open `https://localhost:7444/admin` with a locally trusted CA and matching certificate SAN. The bundled `web/admin.html` and `web/admin.js` must both be installed under `--web-root`; there is no directory listing or arbitrary static-file service. In a container whose admin bind is loopback, administer from inside that container or use a deliberately configured private management network. Do not publish the admin listener alongside the player port.

There is no default administrator/password or public role-selection parameter. Register an independent account through `/v1/register`, then explicitly elevate **that existing username** through local service configuration:

```sh
ikv-auth --cert /etc/ikv/server-chain.pem --key /etc/ikv/server-key.pem \
  --dsn-file /etc/ikv/database.dsn --admin-bootstrap community_operator
```

Use the same bind, migrations, web-root and backup options as the normal service where needed. Remove the bootstrap option after successful initialization. It does not create an account. Bootstrap is a local administrative action, not a login backdoor. Log in to the dashboard with that account. Role errors are displayed rather than hidden.

The dashboard keeps the bearer session **only in JavaScript memory**, never cookies, local/session storage or URL parameters. Reloading requires login again. Credentials are submitted only as same-origin HTTPS JSON; the password input is cleared after login. Server-provided names, logs and status are rendered as text. Security headers disable framing, MIME sniffing, referrer disclosure and caching, and CSP permits only the bundled same-origin script and API connections. Cross-origin requests are refused; no permissive CORS header is returned.

## HTTP contract

Bodies are JSON objects with `Content-Type: application/json`. Authenticated requests use `Authorization: Bearer SESSION`, where a session is a 64-character lowercase hexadecimal token. Do not place credentials or tokens in URLs. Only GET account/character **search** may use `?search=`; other query strings are rejected. Percent-encode the search value; do not append other parameters. Successful API calls return JSON with HTTP 200. Errors use their actual HTTP status and this stable shape:

```json
{"error":{"code":"unauthorized","message":"Session expired, revoked or account banned"}}
```

HTTP parsing is Boost.Beast, not a hand-written framing parser. The player listener has eight active connection slots and four execution workers; administration has **four independent slots and two independent workers**. Silent public TLS clients and public job saturation cannot consume administration admission or execution slots. The listeners share the Store's bounded PostgreSQL pool, not their socket/job budgets. Each connection serves one request, with 10-second asynchronous TLS-handshake, HTTP-read, HTTP-write and TLS-close deadlines. Request bodies are limited to 64 KiB (413), headers to 8 KiB (431), and request targets to 2048 bytes. Malformed HTTP/JSON returns 400; non-JSON bodies return 415. Admission above that listener's cap closes the new socket; it does not create another worker or unlimited queue. Backend work retains its admission reservation until completion. No request body, password, bearer token or DSN is logged by the service.

### Player routes

| Method/path | Body or authorization | Result / semantics |
| --- | --- | --- |
| GET `/v1/health` | None | Minimal actual database readiness; no management details. |
| GET `/v1/status` | None | `{online,world}` from persisted community-world heartbeat. |
| POST `/v1/register` | `{username,password}` | `{account_id}`; independent account only, no administrator field. |
| POST `/v1/login` | `{username,password}` | `{session,expires_at}`; session is secret. |
| POST `/v1/logout` | Bearer, `{}` | Revokes the session. |
| POST `/v1/reset` | `{username,token,new_password}` | Consumes a valid administrator-issued reset token and changes the password; existing sessions are invalidated. |
| GET `/v1/characters` | Bearer | `{characters:[...]}` owned, undeleted characters. |
| POST `/v1/characters` | Bearer, `{name,class_id,appearance}` | Created character record. |
| DELETE `/v1/characters/{id}` | Bearer, `{confirm_name}` | Owner-only confirmed deletion; a live world lease or persistent guild membership prevents deletion. Leave the guild first (`409 character_in_guild`). |

Public character APIs do not accept authoritative inventory, combat, position, guild or economy outcomes. Character entry and saves belong to the authenticated world server. Character records include `id`, `name`, `class_id`, `appearance` and `state`; current database guild membership/registry is overlaid onto `state.guild` when loading, and stale embedded guild state is removed after offline kicks/leaves. State is the independent world schema, not a recovered official database format.

Usernames are ASCII lowercase `[a-z0-9_]{3,32}`. New/replacement passwords are 12–256 bytes; login accepts 1–256 bytes. Password derivation is Argon2id with 64 MiB memory, three iterations, one lane/thread, a fresh 128-bit salt and a 256-bit derived hash. Sessions are random 256-bit tokens, only their SHA-256 hashes are stored, expire after 12 hours, and are capped at eight active sessions per account. Reset tokens use the same random/hash policy, expire after 15 minutes, and are single-use; successful reset revokes all sessions and all other outstanding reset tokens. Expiration fields are ISO UTC timestamps.

Registration/login/reset apply a real-peer limit of 60 requests/minute, including malformed attempts against that peer. Credential attempts are separately limited to 12/minute **per operation + listener-prefixed peer + username**, only after complete required-body validation; malformed passwords/reset tokens or registration role injection do not charge a target username's credential quota. Player and administration peers use distinct `player:IP` and `admin:IP` keys, so public attempts cannot consume an administrator's private-listener quota even behind the same NAT. Authenticated account requests are capped at 600/minute per listener realm; backups at two/minute. Counters are process-local and use independent player and administration maps, each bounded to 4096 keys and failing closed when its own map is full; public key flooding cannot consume private counter capacity. They are not a distributed cluster limiter; restarting auth resets counters.

Character names are 2–32 UTF-8 bytes without control characters, and undeleted names are case-insensitively unique. An account can create at most eight characters. `class_id` is 1–3 in the explicitly NONCANONICAL independent schema, not evidence of original class rules. `appearance` defaults to an object `{}` and is limited to 384 encoded JSON bytes, matching the multiplayer entity wire budget; public creation rejects `state`. Initial state is `{}` until the trusted world initializes it.

### Private management routes

All routes below require a valid administrator bearer session on the **administration** listener.

| Method/path | Body | Result / semantics |
| --- | --- | --- |
| GET `/v1/admin/status` | None | `{accounts,characters,sessions,worlds:[{instance,status,updated_at,online}],logs:[{id,action,actor_id,target_id,details,created_at}],commands:[{id,command,arguments,result,actor_id,created_at,consumed}]}`; status/performance comes from real world heartbeats, not fabricated charts. `commands` is the last 30 world commands; `result` is null until the world executed it. |
| GET `/v1/admin/accounts?search=VALUE` | None | `{accounts:[{id,username,admin,banned,ban_reason,plus}]}`; account search/counts and moderation selection. |
| GET `/v1/admin/characters?search=VALUE` | None | `{characters:[...]}`; character search and moderation selection. |
| POST `/v1/admin/accounts/{id}/ban` | `{reason,duration_seconds}` | Reason 1–512 bytes; duration 0–31536000 seconds, where 0 is a permanent ban; revokes sessions. |
| POST `/v1/admin/accounts/{id}/unban` | `{}` | Removes the account ban; does not resurrect revoked sessions. |
| POST `/v1/admin/accounts/{id}/reset` | `{}` | `{token,expires_at}`; single-use secret for administrator-assisted delivery. |
| POST `/v1/admin/accounts/{id}/plus` / `/unplus` | `{}` | Grants/revokes the Plus Paket entitlement (`accounts.plus`, migration 005; audited `account.plus`/`account.unplus`). The world re-reads it at its 1 s session heartbeat: vendor 1/3 instead of 1/5, 3× Şan and Klan Parası from clan kills, 10 % instead of 20 % Karizma loss, 10 instead of 3 complaints per session (guide). NONCANONICAL source: the original sold it through the web İKV Dükkan. |
| POST `/v1/admin/moderation` | `{character_id,action,duration_seconds}` | `action` is `mute` or `unmute`; mute duration is 1–604800 seconds; server enforces moderation. |
| POST `/v1/admin/control` | `{command,delay_seconds?,message?}` | `restart`, `shutdown` (countdown 0–3600 s, default 0; optional ≤256-byte reason) or `cancel`. The world announces the countdown in game (start, 300/120/60/30/10 and the last 5 s) and then exits. Returns `{ok,command_id}`; one restart/shutdown may be pending. No arbitrary shell command. |
| POST `/v1/admin/world` | `{command,...}` | `announce {message}` (1–256 bytes, no control characters), `kick {character_id,reason?}`, `teleport {character_id,zone,x?,y?}` (spawn when x/y omitted), `summon {character_id,to_character_id}`, `clock {hour 0–23}` (runtime only), `respawn {zone?}` (dead NPCs), `validate_config {}` (re-read and construct the configured gameplay file without applying it; source support grids are only checked at real startup). Returns `{ok,command_id}`. |
| POST `/v1/admin/bots` | `{action:"spawn",count 1–50,behaviour town\|hunt\|companion,zone?,level? 1–100}` or `{action:"despawn",all:true \| zone \| ids:[1–64 ids]}` | Queues `bots_spawn`/`bots_despawn` for the world's bot host. Returns `{ok,command_id}`. |
| GET/POST `/v1/admin/settings` | POST `{xp_rate,drop_rate,regen_rate,disabled_zones}` | Persisted NONCANONICAL percent rates 10–1000 (100 = unchanged; drop chance caps at 100%) and up to 128 zone names closed to entry (doors/teleports refused with `zone_disabled`; occupants stay; in-game administrators may enter). Applied live through a `settings` command and on every world start. GET returns `{settings}` (`{}` when never set). |
| GET `/v1/admin/backups` | None | `{backups:[{name,size}]}` in configured backup directory, newest first. |
| GET `/v1/admin/backups/{name}` | None | Raw `application/sql` attachment of one owned regular `ikv-*.sql` file up to 256 MiB (larger: copy from the server). Audited as `database.backup_download`; 6/minute. |
| POST `/v1/admin/backups` | `{}` | Real PostgreSQL backup creation; `{name,size}` only after `pg_dump` succeeds. |
| POST `/v1/admin/restore` | Any object | 409 `offline_restore_required` after RBAC; deliberately no live database replacement. |

World commands are rows in `world_commands`; the world takes up to 16 per one-second heartbeat, executes them through the same `World` paths as gameplay (teleports use the normal transfer/collision/persistence path) and stores `{ok:true,...}` or `{ok:false,error}` as the result. At most 64 commands may be pending (`429 command_queue_full`). Every queued command is audited as `world.COMMAND` with its arguments.

**CSRF/RBAC.** Every management route is served only on the private listener and checked by Store for an administrator bearer session. The session is an `Authorization` header held in page memory, never an ambient cookie, so a cross-site page cannot attach it; additionally any foreign `Origin` is refused (403) and request bodies must be `application/json` (415 otherwise), which a cross-site form cannot send without a CORS preflight that is never granted. `tests/local_multiplayer.py` (`admin_web`) asserts 404 on the player listener, 401 without a session, 403 for non-administrators, 403 for a foreign Origin, 415 for text bodies and that refused requests enqueue nothing, for every new route.

The auth CLI passes `--backup-dir` to Store using the private `IKV_BACKUP_DIR` environment setting. `restart` requires a supervisor with on-failure restart: the world exits 75. `shutdown` exits 0 and should stay stopped. Dashboard confirmations are safeguards against operator mistakes, not a substitute for REST authorization.

Account-search records are `{id,username,admin,banned,ban_reason}`. Character records also expose `moderation:{muted,muted_until}`, with an ISO UTC timestamp or null. World `status` publishes actual `instance`, `players`, `connections`, `tick`, `tick_ms`, `uptime_seconds`, `memory_kib` (resident set), `world_port`, `logs` (last 64 action results), `zones` (`{zone,players,bots,npcs,disabled}`), `online` (non-bot players with id/character/position/level/hp), `bots`, `rates`, `clock_hour` and `countdown` (`{command,seconds}` or null); the dashboard shows these values rather than estimating performance.

The dashboard (`web/admin.html`, `web/admin.js`, no external assets) has Turkish tabs with English tooltips: Genel bakış (overview), Oyuncular (online players, account/character search, ban/unban, mute, kick, teleport/summon, saved-state inspection, reset tokens), Botlar, Dünya (announce, restart/shutdown countdown and cancel, day clock, configuration dry run), İçerik (zone entry switches, NPC respawn), Günlükler (world log, command results, audit), Yedekler (create/list/download, offline restore instructions) and Ayarlar (rates). World-command buttons wait for the world's recorded result and show it.

## Assisted password reset

An administrator verifies the account holder through the community's own out-of-band process, selects the account, and issues a reset token. Deliver the raw token through a private channel, never public chat, audit details or a query string. The holder submits the token, username and replacement password as a JSON body to `/v1/reset`. The dashboard shows the newly issued secret only transiently; clear it after delivery or log out. No public endpoint issues or reveals reset tokens. Expired/consumed tokens fail; do not claim email delivery, identity-proofing or MFA that this service does not implement.

## Real backup and offline restore

Install PostgreSQL `pg_dump` and `psql` compatible with the database server. Store invokes fixed tools without an arbitrary shell and writes resolved libpq connection options, including TLS-key passwords, to an ephemeral owned mode-0600 `/tmp/ikv-pg-service-*` service file. Child-only `PGSERVICEFILE`/`PGSERVICE` select that file; DSN/passwords never enter argv/logs. The actual selected host/address/port/database are pinned, original service indirection is removed, and automatic cleanup removes the temporary file after the tool. Newline-containing connection-option values fail closed with `backup_error` because libpq service files cannot encode them. Configure one authoritative PostgreSQL endpoint: multihost comma-separated host/address/port lists and different selected endpoints across the four pool connections are rejected.

Dumps are real **plain SQL** `ikv-*.sql` files (mode 0600) in a mode-0700 directory. Creation uses `pg_dump --format=plain --clean --if-exists --no-owner --no-privileges --file FILE`; restoration uses `psql --no-psqlrc --set ON_ERROR_STOP=1 --single-transaction --file FILE`. Both tools have a five-minute deadline and nonzero exit is failure. The restored role/ownership comes from the destination configuration, not a privileged owner from the source. SQL backups are trusted operator inputs: never restore an untrusted downloaded script. Backups contain account hashes, session/reset hashes, characters and audit data; use protected off-host storage and encryption at rest according to community policy. Do not distribute backups with game releases.

A successful dump is **not** proof of restorability. Exercise recovery on a disposable database first, with its own role/DSN, and validate counts and actual account/character access. Never test restoration against a live community database.

For intended restoration, stop **both** auth and world, retain an independent pre-restore backup, and run the explicit local tool:

```sh
ikv-auth --dsn-file /etc/ikv/restore-database.dsn \
  --restore-backup /var/lib/ikv/backups/ikv-SELECTED_BACKUP.sql
```

Offline mode accepts only those two options: it does not need certificates, does not bind HTTPS, does not bootstrap an administrator, and does not apply startup migrations before restoring. It prints `RESTORED database backup` only after Store restoration returns successfully. Failure is nonzero and must be investigated before restarting services. Use the correct destination DSN: restoration replaces that database's data. Confirm the intended destination and backup yourself; a path in the web UI never authorizes a restore. Start normal auth/world processes only after restoration and validation are complete. The dashboard explains this procedure instead of pretending that a live-restore button is safe.

Every live auth/world Store holds a shared PostgreSQL advisory process lock. Offline restoration first releases its own shared locks and must acquire the exclusive lock; another active service causes `restore_busy` instead of data replacement. The exclusive lock remains held while `psql` runs in a single transaction. A stale heartbeat does not override a live process lock.

Network-stage 10-second deadlines do not impose a 10-second total database-operation budget. Backup creation can wait for the bounded five-minute PostgreSQL tool; its connection admission slot stays reserved throughout that work, even if the caller disconnects. The dashboard allows 310 seconds for the backup POST. If the browser/network fails before receiving the result, check the real backup list before creating another dump; do not infer failure or success from a lost response.

## Acceptance evidence

Integration must exercise registration/login/logout, administrator RBAC on the private listener, public management-route 404 isolation, TLS certificate/host validation, malformed and oversized HTTP, reset expiry/single use/session revocation, bans, ownership and live-character deletion protection, persisted character saves, real backup and disposable offline restore. Additional browser checks cover transient secrets, CSP, keyboard navigation, empty/error states and actual action confirmations. This document states source/API behavior and operating instructions; it does not assert those checks passed.
