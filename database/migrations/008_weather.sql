-- Seasons, weather and wind (wave 27, NONCANONICAL): the administrator "weather" command forces or clears a weather kind and
-- season through world_commands like the other private administration commands.
ALTER TABLE world_commands DROP CONSTRAINT world_commands_command_check;
ALTER TABLE world_commands ADD CONSTRAINT world_commands_command_check CHECK (command IN (
 'restart','shutdown','cancel','announce','kick','teleport','summon','clock','respawn','settings','validate_config','bots_spawn','bots_despawn',
 'players','items','inventory','give_item','take_item','currency','give_xp','heal','kits','give_kit','weather'));
