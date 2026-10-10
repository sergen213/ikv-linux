-- Independent community schema, not the original game's database.
CREATE TABLE accounts (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 username text NOT NULL UNIQUE CHECK (username ~ '^[a-z0-9_]{3,32}$'),
 password_salt text NOT NULL CHECK (length(password_salt)=32),
 password_hash text NOT NULL CHECK (length(password_hash)=64),
 admin boolean NOT NULL DEFAULT false,
 banned_until timestamptz,
 ban_reason text NOT NULL DEFAULT '',
 created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE sessions (
 token_hash text PRIMARY KEY CHECK (length(token_hash)=64),
 account_id bigint NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
 expires_at timestamptz NOT NULL,
 revoked_at timestamptz,
 created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX sessions_account ON sessions(account_id);
CREATE TABLE password_resets (
 token_hash text PRIMARY KEY CHECK (length(token_hash)=64),
 account_id bigint NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
 expires_at timestamptz NOT NULL,
 consumed_at timestamptz
);
CREATE TABLE characters (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 account_id bigint NOT NULL REFERENCES accounts(id),
 name text NOT NULL CHECK (length(name) BETWEEN 2 AND 32),
 class_id integer NOT NULL CHECK (class_id BETWEEN 1 AND 3),
 appearance jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(appearance)='object'),
 state jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(state)='object'),
 lease_instance text,
 lease_expires_at timestamptz,
 deleted_at timestamptz,
 created_at timestamptz NOT NULL DEFAULT now(),
 updated_at timestamptz NOT NULL DEFAULT now(),
 CHECK ((lease_instance IS NULL)=(lease_expires_at IS NULL))
);
CREATE UNIQUE INDEX characters_live_name ON characters(lower(name)) WHERE deleted_at IS NULL;
CREATE INDEX characters_account ON characters(account_id);
CREATE TABLE world_instances (
 instance text PRIMARY KEY,
 status jsonb NOT NULL,
 updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE world_commands (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 command text NOT NULL CHECK (command IN ('restart','shutdown')),
 actor_id bigint NOT NULL REFERENCES accounts(id),
 created_at timestamptz NOT NULL DEFAULT now(),
 consumed_at timestamptz
);
CREATE UNIQUE INDEX world_commands_single_pending ON world_commands((1)) WHERE consumed_at IS NULL;
CREATE TABLE chat_moderation (
 character_id bigint PRIMARY KEY REFERENCES characters(id),
 muted_until timestamptz NOT NULL,
 actor_id bigint NOT NULL REFERENCES accounts(id)
);
CREATE TABLE audit_events (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 action text NOT NULL,
 actor_id bigint REFERENCES accounts(id),
 target_id bigint,
 details jsonb NOT NULL DEFAULT '{}',
 created_at timestamptz NOT NULL DEFAULT now()
);
