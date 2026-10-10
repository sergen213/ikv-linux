# Independent PostgreSQL persistence schema

Persistence is implemented by [`Store`](../include/ikv/store.hpp), [`src/store.cpp`](../src/store.cpp), [`001_private_world.sql`](../database/migrations/001_private_world.sql) and [`002_guild_registry.sql`](../database/migrations/002_guild_registry.sql). This is an independent community-server schema, **not** a recovered official İKV database. Original mechanics and IDs require separate asset/protocol evidence; generic JSON state does not establish original-game compatibility. Source implementation and runtime acceptance are different: observed results belong in [PROGRESS.md](PROGRESS.md).

## Implemented tables

| Table | Ownership and persisted fields |
| --- | --- |
| `schema_migrations` | Applied SQL filename/checksum bookkeeping; modified applied migrations are rejected. |
| `accounts` | Generated account ID; unique lowercase username; Argon2id salt/hash; explicit administrator flag; timed ban/reason; creation time. No plaintext password. |
| `sessions` | SHA-256 token hash; account foreign key; expiry/revocation/creation times. Raw session tokens are returned once to the client, never persisted here. |
| `password_resets` | SHA-256 single-use reset-token hash; account foreign key; expiry/consumption time. No public reset-token issuance. |
| `characters` | Generated character ID/account owner; name/class; JSON object appearance and authoritative state; world lease/expiry; soft-deletion and creation/update times. |
| `world_instances` | Instance identity, actual JSON status/performance heartbeat and update time. |
| `world_commands` | Fixed command names (`restart`, `shutdown`, `cancel`, `announce`, `kick`, `teleport`, `summon`, `clock`, `respawn`, `settings`, `validate_config`, `bots_spawn`, `bots_despawn`; migration 004), validated JSON `arguments`, world-recorded JSON `result`, issuing administrator, creation/consumption time. At most one pending `restart`/`shutdown`. No arbitrary executable or shell string. |
| `world_settings` | Single row: administrator XP/drop/regeneration percent rates and zones closed to entry (NONCANONICAL), issuing administrator, update time; applied by the world at startup and on `settings` commands. |
| `chat_moderation` | Character key, timed mute and issuing administrator. |
| `audit_events` | Action, optional actor/target, JSON details, creation time; raw credentials/session/reset tokens must not enter details. |
| `guild_registry` | Guild ID, positive revision, authoritative JSON object roster/state and update time; disbanded guilds retain tombstone state. |
| `guild_members` | Unique character membership key and guild foreign key; database-backed membership survives reconnect and offline removal. |

The SQL migration enforces account username format, salt/hash lengths, session/reset hash lengths, character class range and object-shaped appearance/state, valid lease-pair nullability, foreign keys and case-insensitive uniqueness of undeleted character names. API bounds additionally limit names, passwords, appearance/state sizes and account character count. Class IDs 1–3 are an explicitly NONCANONICAL independent representation, not reconstructed original class semantics.

Inventory, equipment, position, stats, skills, quests, progression, currency and other trusted gameplay fields live inside `characters.state` according to the independent world configuration/schema. Public account/character REST cannot submit authoritative outcomes. Guild names/chat have original evidence; the independent guild rules are configured NONCANONICAL, with separate persistent membership/registry tables. This implementation does not claim separate recovered tables for every original-game subsystem or import official-server state.

## Database access and migration policy

Store uses libpq parameterized queries for runtime values and a fixed four-connection pool. Connect timeout is five seconds, pool admission is bounded, statement timeout is ten seconds and lock timeout is five seconds. The pool requires one authoritative PostgreSQL endpoint: comma-separated `host`/`hostaddr`/`port` lists and connections selecting different endpoints are rejected. Auth and world each create their own Store. SQL migrations are trusted local deployment inputs, applied under an advisory migration lock and transaction with checksum bookkeeping; never upload/run an arbitrary migration through the dashboard. Do not edit an already-applied migration: create a new versioned SQL migration and review the data transition instead.

The connection string comes from an effective-user-owned regular mode-0600 `--dsn-file`, not argv or logs. Keep PostgreSQL private, use a dedicated database role and protect backup paths. Full credentials, HTTPS listener isolation, API fields, Argon2id parameters, rate limits, administrator bootstrap and assisted reset instructions are in [AUTH_DATABASE.md](AUTH_DATABASE.md).

## Authenticated character leases

The world validates the bearer account and character ownership before entry. Atomic entry checks that the character exists, is undeleted, is not banned and has no live lease; it then installs the instance and a 30-second expiry. A concurrent second entry cannot own the same character. Store's process-local lease map binds saves/releases to that process's own acquired characters; raw sessions are not stored in leases.

World heartbeats refresh live leases and publish status; an expired process-local lease produces `lease_lost` and rolls back the heartbeat transaction rather than silently renewing lost ownership. Normal disconnect releases the lease; after a crash, expiry permits recovery. Public character deletion requires the exact confirmation name and rejects a live lease. It also returns 409 `character_in_guild` while the character has persistent guild membership: leave the guild before deletion. Expired/revoked/banned sessions are rejected during authentication, and the world periodically revalidates authorization; deleting or changing public JSON cannot bypass ownership.

The deployment contract is **one world process per community database**, with multiple zone instances/entities inside that process. Separate auth and world processes share PostgreSQL. This is not an advertised multi-world cluster or a distributed lease/command broker.

## Atomic gameplay saves

`save_character` delegates to `save_characters` with a one-element batch. A save batch sorts character IDs for deterministic row locking, verifies owner/undeleted/live lease state, and updates all character state in one transaction. A trusted world's final save remains allowed under its live lease after logout or an account ban, preventing loss of already-authoritative gameplay; new entry and session authentication still reject banned accounts. A failed validation or SQL statement rolls back the batch instead of committing half of a trade. The world must persist both changed participants before acknowledging a transaction and must restore its previous in-memory state if persistence fails.

Guild roster changes are committed in the **same transaction** as character statistics/items. Creation uses revision 1; changed guild state must advance the revision by one, while identical JSON at the current revision is accepted. Rosters are capped at 256 members, and unique `guild_members.character_id` prevents concurrent guild membership. Character loading/entry overlays the current registry into `state.guild`, or removes that field when no membership exists, so offline kicks/leaves cannot resurrect stale embedded state. Last-member departure preserves a disband tombstone with `members:[]` and `leader:0`.

Guild-save transactions use a global advisory lock: an intentional serialized ceiling for the current 32-live-player world, not a scalable distributed guild service. Upgrade the locking strategy only when measured contention requires it. Revision/membership checks and all roster/character mutations roll back together on failure.

Only trusted world code uses these save APIs. Authentication Store API exposes creation/listing/deletion, not arbitrary character-state updates. Boundaries still require tests for duplicate entry, failed trade/save rollback, stale leases and crash recovery; a JSON blob alone is not duplication protection.

## Backup, restoration and data lifecycle

Backups are real fixed-tool `pg_dump` plain-SQL `ikv-*.sql` files, restricted to mode 0600 inside a mode-0700 directory. Database-tool credentials and all resolved libpq options are written to an ephemeral owned mode-0600 `/tmp/ikv-pg-service-*` file, selected by child-only `PGSERVICEFILE`/`PGSERVICE` environment variables. The selected host/address/port/database are pinned; the original service indirection is removed, and the file is removed automatically after the tool. Credentials are never placed in argv/logs. Newline-containing connection-option values fail closed because the service-file format cannot encode them. Backup/restore tools have bounded execution deadlines; nonzero status is failure. Dumps include sensitive account/session/reset hashes and game/audit records. Protect, encrypt and retain them according to the community's explicit policy; do not package them with releases or original assets.

Restoration is an explicit **offline local CLI operation**, never an unsafe live dashboard mutation. Stop auth and world, verify the intended destination DSN and trusted backup, preserve a pre-restore backup, then use `ikv-auth --dsn-file FILE --restore-backup FILE`. Every live Store holds a shared PostgreSQL advisory process lock; offline restore requires the exclusive lock and rejects another active service with `restore_busy`. The lock remains held throughout single-transaction `psql` restoration. A recent or stale heartbeat alone cannot prove that all processes stopped. The HTTPS restore route gives an RBAC-checked `offline_restore_required` response. Exact tool arguments and operating procedure are in [AUTH_DATABASE.md](AUTH_DATABASE.md).

A successful dump does not prove recovery. Exercise restoration against a disposable database and validate actual account login, character state, table constraints and counts before relying on a backup. This document does not assert that recovery, migration upgrades or retention/privacy cleanup were executed. Automatic audit/hash retention deletion, encrypted-at-rest database configuration and a community identity-verification policy remain deployment decisions rather than hidden features.
