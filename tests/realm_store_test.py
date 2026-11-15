#!/usr/bin/env python3
"""NONCANONICAL wave 30/31 realm/shard boundaries on a disposable PostgreSQL cluster.

Applies migrations 001-008 through a real ikv-auth, populates accounts/characters/guilds through the API and SQL, then
restarts auth with every migration so 009 and 010 run over that populated database. realm-store-test then drives the
Store API (shards, tickets, lease handoff, bus, lockouts) and this script checks the realm HTTPS routes, the wave 31 realm
control actions (create/update/start/stop/restart/delete) and the host layout channel (--host-token-file).
"""

import argparse
import http.client
import json
import os
import subprocess
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import isolated_services  # noqa: E402


class RealmServices(isolated_services.Services):
    deferred = True

    def start_auth(self, admin=None, migrations=None, host_token=None):
        if self.deferred:  # bootstrap() starts auth; this test chooses the migration set itself
            self.deferred = False
            return
        args = ["--bind", "127.0.0.1", "--port", "0", "--admin-bind", "127.0.0.1", "--admin-port", "0", "--cert", self.cert,
                "--key", self.key, "--dsn-file", self.dsn_file, "--migrations", migrations or Path(self.args.source) / "database/migrations",
                "--web-root", Path(self.args.source) / "web", "--backup-dir", self.root / "backups"]
        if host_token:
            args.extend(("--host-token-file", host_token))
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


def host_api(services, method, body=None, *, authorization=None, admin=True, status=200, path="/v1/host/layout"):
    connection = http.client.HTTPSConnection("localhost", services.admin_port if admin else services.auth_port, context=services.context, timeout=5)
    headers = {"Content-Type": "application/json"}
    if authorization is not None:
        headers["Authorization"] = authorization
    try:
        connection.request(method, path, body=None if body is None else json.dumps(body).encode(), headers=headers)
        response = connection.getresponse()
        result = json.loads(response.read(1024 * 1024) or b"null")
        assert response.status == status, services.redact(f"{method} {path}: expected {status}, got {response.status}: {result}")
        return result
    finally:
        connection.close()


def refused_token_file(services, args, path, description):
    """ikv-auth must refuse to start with this host token file and never echo its content."""
    process = subprocess.run([args.auth, "--bind", "127.0.0.1", "--port", "0", "--admin-bind", "127.0.0.1", "--admin-port", "0",
                              "--cert", services.cert, "--key", services.key, "--dsn-file", services.dsn_file,
                              "--migrations", Path(args.source) / "database/migrations", "--web-root", Path(args.source) / "web",
                              "--host-token-file", path], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
    assert process.returncode != 0, description + " was accepted"
    assert "host token file validation" in process.stderr, services.redact(process.stderr)
    assert "READY" not in process.stdout, description


def realm_control(services, args, first, second, twin, mehmet):
    """6. NONCANONICAL wave 31: desired realm layout, realm actions and the host layout channel."""
    admin = lambda body, status=200: services.api("POST", "/v1/admin/realms", body, session=first, admin=True, status=status)
    listing = lambda: {row["id"]: row for row in services.api("GET", "/v1/admin/realms", session=first, admin=True)["realms"]}
    public = lambda: {row["id"]: row for row in services.api("GET", "/v1/realms", session=second)}
    rows = listing()
    assert {key: rows[1][key] for key in ("desired_shards", "capacity", "enabled")} == {"desired_shards": 1, "capacity": 32, "enabled": True}, rows[1]
    assert isinstance(rows[1]["shards"], list), "the live shard list keeps its wave 30 name"
    assert "desired_shards" not in public()[1], "players do not see the host layout"

    # Create and update.
    created = admin({"action": "create", "name": "Kadıköy", "kind": "rp", "sort": 3, "shards": 2, "capacity": 16, "enabled": False})
    kadikoy = created["id"]
    row = listing()[kadikoy]
    assert (row["name"], row["kind"], row["desired_shards"], row["capacity"], row["enabled"], row["status"]) == ("Kadıköy", "rp", 2, 16, False, "offline"), row
    admin({"action": "create", "name": "Kadıköy"}, status=409)
    admin({"action": "create", "name": "Üsküdar", "shards": 17}, status=400)
    admin({"action": "create", "name": "Üsküdar", "capacity": 33}, status=400)
    admin({"action": "create", "name": "Üsküdar", "enabled": "yes"}, status=400)
    admin({"action": "update", "id": kadikoy, "name": "Kadıköy Rıhtımı", "kind": "pvp", "shards": 3, "capacity": 20, "enabled": True})
    row = listing()[kadikoy]
    assert (row["name"], row["kind"], row["desired_shards"], row["capacity"], row["enabled"]) == ("Kadıköy Rıhtımı", "pvp", 3, 20, True), row
    admin({"action": "update", "id": kadikoy, "capacity": 0}, status=400)
    admin({"action": "update", "id": kadikoy}, status=400)
    admin({"action": "update", "id": 999, "shards": 1}, status=404)
    admin({"action": "update", "id": kadikoy, "name": "Eminönü"}, status=409)
    admin({"action": "launch", "id": kadikoy}, status=400)

    # Stop/start: a disabled realm is offline for players and /world refuses it even with a live shard.
    services.sql("UPDATE world_instances SET updated_at=now(),players=0 WHERE instance IN ('h1','h2','g1')")
    assert services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second)["instance"] == "g1"
    admin({"action": "stop", "id": 2})
    assert listing()[2]["enabled"] is False and public()[2]["status"] == "offline"
    error = services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second, status=503)
    assert error["error"]["code"] == "realm_offline", error
    admin({"action": "start", "id": 2})
    assert public()[2]["status"] == "online"
    assert services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second)["instance"] == "g1"
    admin({"action": "update", "id": 2, "enabled": False})
    services.api("GET", "/v1/characters/%d/world" % twin["id"], session=second, status=503)
    admin({"action": "update", "id": 2, "enabled": True})

    # Restart: targeted world restart rows, one per live shard of the realm, or one shard.
    services.sql("UPDATE world_commands SET consumed_at=now() WHERE consumed_at IS NULL")
    services.sql("UPDATE world_instances SET updated_at=now() WHERE instance IN ('h1','h2','g1')")
    restarted = admin({"action": "restart", "id": 1, "delay_seconds": 30, "message": "Bakım"})
    assert len(restarted["command_ids"]) == 2, restarted
    assert services.sql("SELECT string_agg(command||':'||target_instance||':'||target_realm||':'||(arguments->>'delay_seconds')||':'||(arguments->>'message'),',' ORDER BY target_instance) "
                        "FROM world_commands WHERE consumed_at IS NULL") == "restart:h1:1:30:Bakım,restart:h2:1:30:Bakım"
    error = admin({"action": "restart", "id": 1, "instance": "h1"}, status=409)
    assert error["error"]["code"] == "command_pending", error
    assert len(admin({"action": "restart", "id": 2})["command_ids"]) == 1, "another realm's shard is not blocked"
    services.sql("UPDATE world_commands SET consumed_at=now() WHERE consumed_at IS NULL")
    one = admin({"action": "restart", "id": 1, "instance": "h2"})
    assert len(one["command_ids"]) == 1
    assert services.sql("SELECT target_instance||':'||(arguments->>'delay_seconds') FROM world_commands WHERE consumed_at IS NULL") == "h2:60"
    services.sql("UPDATE world_commands SET consumed_at=now() WHERE consumed_at IS NULL")
    error = admin({"action": "restart", "id": 1, "instance": "g1"}, status=404)
    assert error["error"]["code"] == "shard_not_found", error
    error = admin({"action": "restart", "id": kadikoy}, status=409)
    assert error["error"]["code"] == "realm_offline", error
    admin({"action": "restart", "id": 1, "delay_seconds": 3601}, status=400)

    # Total desired shards per host are capped at 64.
    services.sql("UPDATE realms SET shards=0 WHERE deleted_at IS NULL")
    for realm in (1, 2, kadikoy):
        admin({"action": "update", "id": realm, "shards": 16})
    extra = admin({"action": "create", "name": "Beşiktaş", "shards": 16})["id"]
    error = admin({"action": "create", "name": "Sarıyer", "shards": 1}, status=409)
    assert error["error"]["code"] == "shard_limit", error
    assert services.sql("SELECT count(*) FROM realms WHERE name='Sarıyer'") == "0"
    admin({"action": "update", "id": extra, "shards": 0})

    # Delete: realm 1 protected, realms with characters refused, otherwise soft delete that frees the name.
    error = admin({"action": "delete", "id": 1}, status=409)
    assert error["error"]["code"] == "realm_protected", error
    error = admin({"action": "delete", "id": 2}, status=409)
    assert error["error"]["code"] == "realm_has_characters", error
    admin({"action": "delete", "id": extra})
    assert services.sql("SELECT shards||':'||enabled||':'||(deleted_at IS NOT NULL) FROM realms WHERE id=%d" % extra) == "0:false:true"
    assert extra not in listing() and extra not in public()
    admin({"action": "update", "id": extra, "shards": 1}, status=404)
    admin({"action": "delete", "id": extra}, status=404)
    services.api("POST", "/v1/characters", {"name": "Kara", "class_id": 1, "realm_id": extra}, session=first, status=400)
    again = admin({"action": "create", "name": "Beşiktaş"})["id"]
    assert again != extra
    admin({"action": "delete", "id": again})
    assert services.sql("SELECT string_agg(DISTINCT action,',' ORDER BY action) FROM audit_events WHERE action LIKE 'realm.%'") == \
        "realm.create,realm.delete,realm.rename,realm.restart,realm.start,realm.stop,realm.update"

    # Host layout: unavailable without --host-token-file, then refused for unsafe token files.
    token = "0123456789abcdef" * 4
    host_api(services, "GET", authorization="Host " + token, status=404)
    services.secrets.add(token)
    good = services.secret_file("host-token", token + "\n")
    loose = services.secret_file("host-token-loose", token)
    os.chmod(loose, 0o644)
    refused_token_file(services, args, loose, "a 0644 token file")
    link = services.root / "host-token-link"
    link.symlink_to(good)
    refused_token_file(services, args, link, "a symlinked token file")
    refused_token_file(services, args, services.secret_file("host-token-short", "abc"), "a short token")
    refused_token_file(services, args, services.secret_file("host-token-upper", token.upper()), "an uppercase token")
    refused_token_file(services, args, services.root, "a directory")
    services.stop(services.auth)
    services.start_auth(host_token=good)

    host_api(services, "GET", status=401)
    host_api(services, "GET", authorization="Host " + "f" * 64, status=401)
    host_api(services, "GET", authorization="Host " + token[:-1], status=401)
    host_api(services, "GET", authorization="Bearer " + first, status=401)
    host_api(services, "GET", authorization="Host " + token, admin=False, status=404)
    host_api(services, "PUT", {"realms": []}, authorization="Host " + token, admin=False, status=404)
    host_api(services, "POST", {}, authorization="Host " + token, status=405)
    host_api(services, "GET", authorization="Host " + token, path="/v1/host/other", status=404)
    layout = host_api(services, "GET", authorization="Host " + token)
    assert [row["id"] for row in layout["realms"]] == [row["id"] for row in services.api("GET", "/v1/admin/realms", session=first, admin=True)["realms"]]
    assert set(layout["realms"][0]) == {"id", "name", "shards", "capacity", "enabled"} and isinstance(layout["revision"], int), layout

    host_api(services, "PUT", {"realms": [{"id": 999, "shards": 1}]}, authorization="Host " + token, status=400)
    host_api(services, "PUT", {"realms": [{"id": 1, "shards": 17}]}, authorization="Host " + token, status=400)
    host_api(services, "PUT", {"realms": [{"id": 1, "shards": 1}, {"id": 1, "shards": 2}]}, authorization="Host " + token, status=400)
    host_api(services, "PUT", {"realms": [{"id": extra, "shards": 1}]}, authorization="Host " + token, status=400)
    put = host_api(services, "PUT", {"realms": [{"id": 1, "shards": 2, "capacity": 24}, {"id": 2, "shards": 1}]}, authorization="Host " + token)
    shaped = {row["id"]: (row["shards"], row["capacity"]) for row in put["realms"]}
    assert shaped[1] == (2, 24) and shaped[2] == (1, 32) and shaped[kadikoy] == (0, 20) and shaped[7] == (0, 32), shaped
    assert put["revision"] > layout["revision"], (put["revision"], layout["revision"])
    assert host_api(services, "GET", authorization="Host " + token) == put, "PUT/GET round trip"
    assert host_api(services, "PUT", {"realms": [{"id": 1, "shards": 2, "capacity": 24}, {"id": 2, "shards": 1}]}, authorization="Host " + token)["revision"] == put["revision"]
    admin({"action": "update", "id": kadikoy, "enabled": False})
    after = host_api(services, "GET", authorization="Host " + token)
    assert after["revision"] > put["revision"] and next(row for row in after["realms"] if row["id"] == kadikoy)["enabled"] is False
    row = listing()[1]
    assert (row["desired_shards"], row["capacity"]) == (2, 24), row
    assert services.sql("SELECT count(*) FROM audit_events WHERE action='realm.layout' AND actor_id IS NULL") == "2"


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
        assert services.sql("SELECT name FROM schema_migrations ORDER BY name DESC LIMIT 1") == "010_realm_control.sql"
        assert services.sql("SELECT shards||':'||capacity||':'||enabled FROM realms WHERE id=1") == "1:32:true"
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
        realm_control(services, args, first, second, twin, mehmet)
    print("realm store integration passed")


if __name__ == "__main__":
    main()
