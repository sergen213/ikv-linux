#!/usr/bin/env python3
"""NONCANONICAL wave 30 realm/shard boundaries on a disposable PostgreSQL cluster.

Applies migrations 001-008 through a real ikv-auth, populates accounts/characters/guilds through the API and SQL, then
restarts auth with every migration so 009 runs over that populated database. realm-store-test then drives the Store
API (shards, tickets, lease handoff, bus, lockouts) and this script checks the realm HTTPS routes.
"""

import argparse
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import isolated_services  # noqa: E402


class RealmServices(isolated_services.Services):
    deferred = True

    def start_auth(self, admin=None, migrations=None):
        if self.deferred:  # bootstrap() starts auth; this test chooses the migration set itself
            self.deferred = False
            return
        args = ["--bind", "127.0.0.1", "--port", "0", "--admin-bind", "127.0.0.1", "--admin-port", "0", "--cert", self.cert,
                "--key", self.key, "--dsn-file", self.dsn_file, "--migrations", migrations or Path(self.args.source) / "database/migrations",
                "--web-root", Path(self.args.source) / "web", "--backup-dir", self.root / "backups"]
        if admin:
            args.extend(("--admin-bootstrap", admin))
        self.auth = self.start(self.args.auth, *args)
        self.auth_port = self.ready(self.auth, "auth")
        self.admin_port = self.ready(self.auth, "admin")


def account(services, user, token):
    """008-era account and live session written with SQL (the current ikv-auth already speaks the 009 schema)."""
    services.secrets.add(token)
    account_id = int(services.sql("INSERT INTO accounts(username,password_salt,password_hash) VALUES ('%s',repeat('1',32),repeat('1',64)) RETURNING id" % user).splitlines()[0])
    services.sql("INSERT INTO sessions(token_hash,account_id,expires_at) VALUES (encode(sha256('%s'::bytea),'hex'),%d,now()+interval '1 hour')" % (token, account_id))
    return account_id


def character(services, account_id, name, class_id):
    return {"id": int(services.sql("INSERT INTO characters(account_id,name,class_id) VALUES (%d,'%s',%d) RETURNING id" % (account_id, name, class_id)).splitlines()[0])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("auth", "store-test", "source"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    with RealmServices(args) as services:
        # 1. A populated 008 database: two accounts, characters and two guilds that share a name.
        old = services.root / "migrations-008"
        old.mkdir()
        for path in sorted((Path(args.source) / "database/migrations").glob("*.sql")):
            if path.name < "009":
                shutil.copy(path, old / path.name)
        services.start_auth(migrations=old)
        services.stop(services.auth)
        assert services.sql("SELECT max(name) FROM schema_migrations") == "008_weather.sql"
        first, second = "a" * 64, "b" * 64
        first_account, second_account = account(services, "realm_one", first), account(services, "realm_two", second)
        ayse = character(services, first_account, "Ayşe", 1)
        mehmet = character(services, second_account, "Mehmet", 2)
        services.sql("INSERT INTO guild_registry(id,revision,state) VALUES ('g1',1,'{\"id\":\"g1\",\"name\":\"Yeniçeri\",\"members\":[%d]}'),"
                     "('g2',1,'{\"id\":\"g2\",\"name\":\"Yeniçeri\",\"members\":[%d]}')" % (ayse["id"], mehmet["id"]))
        services.sql("INSERT INTO guild_members VALUES (%d,'g1'),(%d,'g2')" % (ayse["id"], mehmet["id"]))
        services.sql("INSERT INTO world_instances(instance,status) VALUES ('legacy-world','{\"players\":0}')")

        # 2. Migration 009 over it; the operator account becomes administrator.
        services.start_auth(admin="realm_one")
        assert services.sql("SELECT name FROM schema_migrations ORDER BY name DESC LIMIT 1") == "009_realms.sql"
        assert services.sql("SELECT string_agg(name||':'||realm_id,',' ORDER BY id) FROM characters") == "Ayşe:1,Mehmet:1"
        assert services.sql("SELECT name FROM realms WHERE id=1") == "Eminönü"
        assert services.sql("SELECT count(*) FROM guild_members") == "2", "guild membership survives 009"
        assert services.sql("SELECT realm_id||':'||port FROM world_instances WHERE instance='legacy-world'") == "1:0"
        assert services.sql("SELECT count(*) FROM information_schema.columns WHERE table_name='guild_registry' AND column_name='realm_id'") == "0", "guilds stay cross-realm"

        # 3. Per-realm character names (realm 1 default for clients without realm_id).
        admin_realm = services.api("POST", "/v1/admin/realms", {"action": "create", "name": "Galata", "kind": "pvp"}, session=first, admin=True)
        assert admin_realm == {"ok": True, "id": 2}, admin_realm
        twin = services.api("POST", "/v1/characters", {"name": "Ayşe", "class_id": 3, "realm_id": 2}, session=second)
        assert twin["realm_id"] == 2 and twin["name"] == "Ayşe"
        services.api("POST", "/v1/characters", {"name": "aYşe", "class_id": 3, "realm_id": 2}, session=first, status=409)
        services.api("POST", "/v1/characters", {"name": "Mehmet", "class_id": 1}, session=first, status=409)
        services.api("POST", "/v1/characters", {"name": "Kara", "class_id": 1, "realm_id": 55}, session=first, status=400)
        listed = services.api("GET", "/v1/characters", session=second)["characters"]
        assert [(row["name"], row["realm_id"]) for row in listed] == [("Mehmet", 1), ("Ayşe", 2)], listed
        services.api("POST", "/v1/admin/realms", {"action": "rename", "id": 2, "name": "Galata Kulesi"}, session=first, admin=True)
        services.api("POST", "/v1/admin/realms", {"action": "rename", "id": 2, "name": "Eminönü"}, session=first, admin=True, status=409)
        services.api("POST", "/v1/admin/realms", {"action": "create", "name": "X"}, session=second, admin=True, status=403)
        services.api("GET", "/v1/admin/realms", session=first, status=404)  # player listener never serves admin routes
        assert services.sql("SELECT string_agg(action,',' ORDER BY id) FROM audit_events WHERE action LIKE 'realm.%'") == "realm.create,realm.rename"

        # 4. Store API against real PostgreSQL.
        services.command([args.store_test, services.dsn_file], timeout=120)

        # 5. Realm list and shard selection over HTTPS (shard rows written here so their heartbeat age is controlled).
        services.sql("DELETE FROM world_instances")
        services.sql("INSERT INTO world_instances(instance,status,realm_id,public_host,port,capacity,players,updated_at) VALUES "
                     "('h1','{}',1,'one.example',7001,32,20,now()),('h2','{}',1,'two.example',7002,32,4,now()),"
                     "('h-dead','{}',1,'dead.example',7003,32,0,now()-interval '16 seconds'),('g1','{}',2,'galata.example',7101,8,8,now())")
        realms = services.api("GET", "/v1/realms", session=second)
        rows = {row["id"]: row for row in realms}
        assert rows[1] == {"id": 1, "name": "Eminönü", "kind": "normal", "status": "online", "population": "medium", "characters": 1}, rows[1]
        assert rows[2]["status"] == "online" and rows[2]["population"] == "full" and rows[2]["characters"] == 1 and rows[2]["kind"] == "pvp", rows[2]
        assert rows[7]["status"] == "offline" and rows[7]["characters"] == 0, rows[7]
        world = services.api("GET", "/v1/characters/%d/world" % mehmet["id"], session=second)
        assert world == {"host": "two.example", "port": 7002, "instance": "h2"}, world
        error = services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second, status=503)
        assert error["error"]["code"] == "realm_full", error
        services.sql("UPDATE world_instances SET updated_at=now()-interval '1 minute' WHERE realm_id=2")
        error = services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second, status=503)
        assert error["error"]["code"] == "realm_offline", error
        services.api("GET", "/v1/characters/%d/world" % mehmet["id"], session=first, status=404)
        # Mid-transfer: a pending ticket's destination wins over the least-loaded shard.
        services.sql("INSERT INTO transfer_tickets(ticket_hash,character_id,from_instance,to_instance,reason,expires_at) VALUES "
                     "(sha256('pending'::bytea),%d,'h2','h1','party',now()+interval '60 seconds')" % mehmet["id"])
        world = services.api("GET", "/v1/characters/%d/world" % mehmet["id"], session=second)
        assert world["instance"] == "h1" and world["host"] == "one.example", world
        listing = services.api("GET", "/v1/admin/realms", session=first, admin=True)["realms"]
        first_realm = next(row for row in listing if row["id"] == 1)
        assert first_realm["characters"] == 2 and {shard["instance"] for shard in first_realm["shards"]} == {"h1", "h2", "h-dead"}
        assert next(shard for shard in first_realm["shards"] if shard["instance"] == "h-dead")["online"] is False
    print("realm store integration passed")


if __name__ == "__main__":
    main()
