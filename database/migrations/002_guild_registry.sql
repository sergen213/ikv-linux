-- Independent community guild persistence; names/chat are evidenced, rules configured NONCANONICAL.
CREATE TABLE guild_registry (
 id text PRIMARY KEY CHECK(length(id) BETWEEN 1 AND 128),
 revision bigint NOT NULL CHECK(revision>0),
 state jsonb NOT NULL CHECK(jsonb_typeof(state)='object'),
 updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE guild_members (
 character_id bigint PRIMARY KEY REFERENCES characters(id),
 guild_id text NOT NULL REFERENCES guild_registry(id) ON DELETE CASCADE
);
CREATE INDEX guild_members_guild ON guild_members(guild_id);
