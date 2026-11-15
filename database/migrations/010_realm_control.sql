-- Realm control (wave 31, NONCANONICAL): the database is the single source of truth for which realms a host runs and
-- with how many shards. The launcher supervisor reads this layout over GET /v1/host/layout; administrators change it
-- with POST /v1/admin/realms from the launcher or the web panel.
--   shards      desired shard (ikv-server process) count, 0 stops every shard of the realm
--   capacity    players per shard
--   enabled     false = stopped: no logins (realm_offline) and the supervisor stops its shards
--   held_shards shard indexes (1..shards) an administrator stopped one by one (stop_shard / start_shard): the supervisor
--               saves and stops them and keeps them down while the rest of the realm runs
--   deleted_at  soft delete: hidden everywhere and its name becomes free again
-- revision is bumped by a trigger on every change, so max(revision) is the layout revision a supervisor polls.
CREATE SEQUENCE realm_layout_revision;
ALTER TABLE realms
 ADD COLUMN shards smallint NOT NULL DEFAULT 1 CHECK (shards BETWEEN 0 AND 16),
 ADD COLUMN capacity smallint NOT NULL DEFAULT 32 CHECK (capacity BETWEEN 1 AND 32),
 ADD COLUMN enabled boolean NOT NULL DEFAULT true,
 ADD COLUMN held_shards smallint[] NOT NULL DEFAULT '{}'
  CHECK (cardinality(held_shards) <= 16 AND 1 <= ALL(held_shards) AND 16 >= ALL(held_shards)),
 ADD COLUMN deleted_at timestamptz,
 ADD COLUMN revision bigint NOT NULL DEFAULT nextval('realm_layout_revision'),
 ADD CONSTRAINT realms_first_kept CHECK (id <> 1 OR deleted_at IS NULL),
 ADD CONSTRAINT realms_deleted_stopped CHECK (deleted_at IS NULL OR (shards = 0 AND NOT enabled));
ALTER TABLE realms DROP CONSTRAINT realms_name_key;
CREATE UNIQUE INDEX realms_live_name ON realms(name) WHERE deleted_at IS NULL;

-- Every update bumps the revision and keeps held_shards a sorted, duplicate-free subset of 1..shards (a smaller shard
-- count or a delete drops the holds of shards that no longer exist).
CREATE FUNCTION realms_bump_revision() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 NEW.revision := nextval('realm_layout_revision');
 NEW.held_shards := ARRAY(SELECT DISTINCT h FROM unnest(NEW.held_shards) h WHERE h <= NEW.shards ORDER BY h)::smallint[];
 RETURN NEW;
END $$;
CREATE TRIGGER realms_revision BEFORE UPDATE ON realms FOR EACH ROW EXECUTE FUNCTION realms_bump_revision();

-- A shard started by a host supervisor reports its index in the realm layout (ikv-server --shard-index), so both panels
-- can tell which live process is shard N; 0 = not supervised (started by hand, by systemd or by an older launcher).
ALTER TABLE world_instances ADD COLUMN shard_index smallint NOT NULL DEFAULT 0 CHECK (shard_index BETWEEN 0 AND 16);
