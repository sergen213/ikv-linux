-- Private web administration commands for the authoritative world (independent schema; NONCANONICAL).
ALTER TABLE world_commands DROP CONSTRAINT world_commands_command_check;
ALTER TABLE world_commands ADD CONSTRAINT world_commands_command_check CHECK (command IN (
 'restart','shutdown','cancel','announce','kick','teleport','summon','clock','respawn','settings','validate_config','bots_spawn','bots_despawn'));
ALTER TABLE world_commands ADD COLUMN arguments jsonb NOT NULL DEFAULT '{}', ADD COLUMN result jsonb;
DROP INDEX world_commands_single_pending;
CREATE UNIQUE INDEX world_commands_single_pending ON world_commands((1)) WHERE consumed_at IS NULL AND command IN ('restart','shutdown');
CREATE TABLE world_settings (
 id integer PRIMARY KEY CHECK (id = 1),
 settings jsonb NOT NULL,
 actor_id bigint NOT NULL REFERENCES accounts(id),
 updated_at timestamptz NOT NULL DEFAULT now()
);
