-- Realms, shards and cross-realm phasing (wave 30). The original game listed several servers (exe 0x0048dbe0
-- construct_serverlist_url); everything else here is NONCANONICAL: realm-scoped character names, shard heartbeats,
-- single-use transfer tickets, raid/dungeon lockouts and a durable shard message bus (rows + NOTIFY ikv_bus).
-- Existing characters and world heartbeats all belong to realm 1.
CREATE TABLE realms (
 id smallint PRIMARY KEY CHECK (id > 0),
 name text NOT NULL UNIQUE CHECK (length(name) BETWEEN 2 AND 32),
 kind text NOT NULL DEFAULT 'normal' CHECK (kind IN ('normal','pvp','rp')),
 sort smallint NOT NULL DEFAULT 0,
 created_at timestamptz NOT NULL DEFAULT now()
);
-- Realm 1 uses Eminönü, an attested original server name (wave 30 research, analysis/porting-wave30/Realms/); other
-- realms and renames go through POST /v1/admin/realms without editing this file.
INSERT INTO realms(id,name,sort) VALUES (1,'Eminönü',1);

ALTER TABLE characters ADD COLUMN realm_id smallint NOT NULL DEFAULT 1 REFERENCES realms(id);
DROP INDEX characters_live_name;
CREATE UNIQUE INDEX characters_live_name ON characters(realm_id, lower(name)) WHERE deleted_at IS NULL;
CREATE INDEX characters_lease ON characters(lease_instance) WHERE lease_instance IS NOT NULL;

-- Guilds and trading are cross-realm: guild_registry/guild_members stay global and accept characters of any realm.

-- A shard is one ikv-server process (32 players). Its heartbeat refreshes updated_at every second; a row older than
-- 15 seconds is dead and never chosen. Rows written only by world_status (no port) are not selectable shards.
ALTER TABLE world_instances
 ADD COLUMN realm_id smallint NOT NULL DEFAULT 1 REFERENCES realms(id),
 ADD COLUMN public_host text NOT NULL DEFAULT '' CHECK (length(public_host) <= 253),
 ADD COLUMN port integer NOT NULL DEFAULT 0 CHECK (port BETWEEN 0 AND 65535),
 ADD COLUMN kind text NOT NULL DEFAULT 'world' CHECK (kind IN ('world')),
 ADD COLUMN capacity smallint NOT NULL DEFAULT 32 CHECK (capacity BETWEEN 1 AND 32),
 ADD COLUMN players smallint NOT NULL DEFAULT 0 CHECK (players BETWEEN 0 AND 32);
CREATE INDEX world_instances_realm ON world_instances(realm_id, updated_at);

-- Raw tickets are 32 random bytes (64 hex characters) handed to the client once; only their SHA-256 is stored.
CREATE TABLE transfer_tickets (
 ticket_hash bytea PRIMARY KEY CHECK (length(ticket_hash) = 32),
 character_id bigint NOT NULL REFERENCES characters(id),
 from_instance text NOT NULL CHECK (length(from_instance) BETWEEN 1 AND 128),
 to_instance text NOT NULL CHECK (length(to_instance) BETWEEN 1 AND 128 AND to_instance <> from_instance),
 reason text NOT NULL CHECK (reason IN ('party','home','layer','instance')),
 -- Store enforces 4 KiB of compact JSON; jsonb text output adds spaces, so the hard cap here is 8 KiB.
 payload jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(payload) = 'object' AND octet_length(payload::text) <= 8192),
 created_at timestamptz NOT NULL DEFAULT now(),
 expires_at timestamptz NOT NULL,
 consumed_at timestamptz
);
CREATE INDEX transfer_tickets_character ON transfer_tickets(character_id, created_at);
CREATE INDEX transfer_tickets_expiry ON transfer_tickets(expires_at);

CREATE TABLE instance_lockouts (
 character_id bigint NOT NULL REFERENCES characters(id),
 zone text NOT NULL CHECK (length(zone) BETWEEN 1 AND 32),
 expires_at timestamptz NOT NULL,
 PRIMARY KEY (character_id, zone)
);

CREATE TABLE shard_messages (
 id bigserial PRIMARY KEY,
 to_instance text NOT NULL CHECK (length(to_instance) BETWEEN 1 AND 128),
 body jsonb NOT NULL CHECK (jsonb_typeof(body) = 'object' AND octet_length(body::text) <= 8192), -- Store: 4 KiB compact
 created_at timestamptz NOT NULL DEFAULT now(),
 consumed_at timestamptz
);
CREATE INDEX shard_messages_pending ON shard_messages(to_instance, id) WHERE consumed_at IS NULL;

-- Administrator world commands reach the right shard: target_instance names one shard (the one leasing a commanded
-- character, or an explicit choice); target_realm limits a command to a realm's shards; both NULL means any shard.
-- World-wide commands are written once per live shard, so the single pending restart/shutdown is per target shard.
ALTER TABLE world_commands
 ADD COLUMN target_instance text CHECK (target_instance IS NULL OR length(target_instance) BETWEEN 1 AND 128),
 ADD COLUMN target_realm smallint REFERENCES realms(id);
DROP INDEX world_commands_single_pending;
CREATE UNIQUE INDEX world_commands_single_pending ON world_commands(COALESCE(target_instance, '')) WHERE consumed_at IS NULL AND command IN ('restart','shutdown');
CREATE INDEX world_commands_pending ON world_commands(id) WHERE consumed_at IS NULL;
