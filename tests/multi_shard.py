#!/usr/bin/env python3
"""NONCANONICAL wave 30 realms and shards end to end (docs/plans/2026-11-14-wave30-realms.md "Acceptance").

Real ikv-auth, a disposable PostgreSQL cluster and three ikv-server shards: realm A "Eminönü" served by two layers (A1, A2)
and realm B "Galata" by one shard (B1, capacity 2). Protocol-level clients cover: least-loaded login and the realm list,
per-realm names, an outdated (version 2) client, a refused phase into a full shard, cross-shard whispers, party phases from
another realm and from another layer (zone, position and inventory kept; shared kill XP), cross-realm trade on a shared shard,
cross-realm guild invites (same shard and over the bus) with guild chat and roster fan-out, a dungeon copy and its Şef lockout
surviving a phase home, a mixed-realm raid entering one raid copy after a cross-shard raid invite (minimum enforced),
administrator command routing and the 60-second guest grace sending guests home. Dungeon and raid parts need the original
support grids (--asset-root); without them they are skipped and reported.
"""

import argparse
from pathlib import Path
import json
import socket
import ssl
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from isolated_services import Services, WorldPeer, frame  # noqa: E402

PASSWORD = "Owned-multishard-Password!30"
CLIENTS = []


class Client:
    """One protocol-level player; drained without blocking so several sessions stay read while a test waits."""

    def __init__(self, services, label, session, character, port, ticket=None):
        self.services, self.label, self.session, self.character = services, label, session, character
        self.connect(port, ticket)

    def connect(self, port, ticket=None):
        self.services.world_port = port
        self.port, self.sequence = port, 0
        self.events, self.state, self.record, self.goodbye = [], None, None, None
        self.closed = self.welcomed = False
        self.peer = WorldPeer(self.services, self.session, self.character, defer_hello=True)
        payload = self.session.encode() + struct.pack("!Q", self.character) + (ticket.encode() if ticket else b"")
        self.peer.connection.sendall(frame(1, payload))
        if self not in CLIENTS:
            CLIENTS.append(self)
        wait(lambda: self.welcomed and self.state is not None or self.goodbye is not None or self.closed, f"{self.label} admission", 10)
        assert self.welcomed, f"{self.label} refused: {self.goodbye}"
        self.id = self.peer.id

    def drain(self):
        if self.closed:
            return
        connection = self.peer.connection
        connection.settimeout(0.002)
        try:
            while True:
                kind, body = self.peer.receive()
                if kind == 2:
                    self.welcomed = True
                elif kind == 6:
                    self.record, self.state = body, body["character"]["state"]
                    self.events.extend(body["events"])
                elif kind == 5:
                    self.goodbye = body
        except socket.timeout:
            pass
        except (EOFError, ConnectionError, ssl.SSLError, OSError):
            self.closed = True
        if not self.closed:
            connection.settimeout(0.25)

    def action(self, action, **fields):
        self.peer.action(action, **fields)

    def move(self, x, y):
        self.sequence += 1
        self.peer.connection.sendall(frame(7, struct.pack("!Iii", self.sequence, x, y)))
        wait(lambda: self.state and (self.state["x"], self.state["y"]) == (x, y), f"{self.label} walk to {x},{y}")

    def mark(self):
        return len(self.events)

    def find(self, since, kind, **fields):
        for event in self.events[since:]:
            if event.get("type") == kind and all(event.get(key) == value for key, value in fields.items()):
                return event
        return None

    def expect(self, since, kind, timeout=10, **fields):
        wait(lambda: self.find(since, kind, **fields) is not None, f"{self.label} event {kind} {fields}", timeout)
        return self.find(since, kind, **fields)

    def entity(self, other):
        return next((e for e in (self.record or {}).get("entities", []) if e["id"] == other.id), None)

    def close(self):
        try:
            self.peer.connection.close()
        except OSError:
            pass
        self.closed = True


def pump():
    for client in CLIENTS:
        client.drain()


def wait(predicate, what, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        pump()
        CLIENTS[0].services.pump(0) if CLIENTS else None
        if predicate():
            return
        assert time.monotonic() < deadline, "deadline awaiting " + what + "\n" + (CLIENTS[0].services.diagnostics()[-6000:] if CLIENTS else "")
        time.sleep(0.01)


def settle(seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        pump()
        time.sleep(0.01)


def phase(client, since, reason, port, realm, accepted_at, timeout=10):
    """Follow a transfer event: goodbye "transfer", reconnect with the ticket, return the measured seconds."""
    event = client.expect(since, "transfer", timeout)
    assert event["reason"] == reason and event["port"] == port and event["host"] == "" and len(event["ticket"]) == 64, event
    assert event.get("realm") == realm, event
    wait(lambda: client.goodbye is not None or client.closed, f"{client.label} transfer goodbye", 5)
    assert client.goodbye in (None, {"reason": "transfer"}), client.goodbye
    client.close()
    client.connect(port, event["ticket"])
    return time.monotonic() - accepted_at


def gameplay(services, asset_root):
    config = services.independent_gameplay()
    if asset_root is None:
        return config, None
    # Dungeon and raid zones from the playable configuration (original support grids), two copies each, reached by fixture
    # doors in eminonu; each has a weak Şef at its spawn and an exit door back to eminonu.
    full = json.loads((Path(services.args.source) / "config/gameplay.json").read_text())
    rat = next(npc for npc in config["npcs"] if npc["kind"] == "enemy")
    door = next(npc for npc in config["npcs"] if npc["kind"] == "door")
    ids = {"dungeon_door": 2147493700, "raid_door": 2147493701, "dungeon_chief": 2147493702, "raid_chief": 2147493703,
           "dungeon_exit": 2147493704, "raid_exit": 2147493705}
    zones = {"dungeon": "migrat", "raid": "buyukhol"}
    for kind, name in zones.items():
        zone = dict(full["zones"][name])
        zone["instances"] = zone["instances"][:2]
        if kind == "raid":
            zone["raid"] = {"min": 4, "lockout": 7200}
        config["zones"][name] = zone
        x, y = zone["spawn"]
        config["npcs"].append(dict(door, id=ids[kind + "_door"], source_uid=30010 + len(config["npcs"]), zone="eminonu", x=-1000, y=600 if kind == "dungeon" else -600,
                                   door={"to": name, "arrive": [x, y]}))
        config["npcs"].append(dict(door, id=ids[kind + "_exit"], source_uid=30020 + len(config["npcs"]), zone=name, x=x, y=y, door={"to": "eminonu", "arrive": [0, 0]}))
        chief = dict(rat, id=ids[kind + "_chief"], name="Şef", zone=name, x=x, y=y, hp=40, damage=1, aggro=150, respawn=72000, xp=600,
                     patrol=[], boss=True)
        config["npcs"].append(chief)
    config["activities"]["raid"] = {"groups": 4, "boss_hp": 1, "boss_damage": 1, "boss_xp": 1}
    return config, ids


def start_shard(services, path, realm, capacity, asset_root):
    args = ["--bind", "127.0.0.1", "--port", "0", "--cert", services.cert, "--key", services.key, "--dsn-file", services.dsn_file,
            "--gameplay", path, "--realm", realm, "--capacity", capacity]
    if asset_root:
        args += ["--asset-root", asset_root]
    child = services.start(services.args.server, *args)
    return child, services.ready(child, "world")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("auth", "server", "source"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--asset-root")
    parser.add_argument("--client")
    args = parser.parse_args()
    asset_root = args.asset_root if args.asset_root and Path(args.asset_root).is_dir() else None
    timings = {}
    with Services(args) as services:
        services.secrets.add(PASSWORD)
        users = {}
        for username in ("ms_admin", "ms_kemal", "ms_selim", "ms_orhan", "ms_zeki", "ms_kemalb"):
            users[username] = services.api("POST", "/v1/register", {"username": username, "password": PASSWORD})["account_id"]
        services.stop(services.auth)
        services.start_auth(admin="ms_admin")
        sessions = {name: services.api("POST", "/v1/login", {"username": name, "password": PASSWORD})["session"] for name in users}
        admin = sessions["ms_admin"]
        assert services.api("POST", "/v1/admin/realms", {"action": "create", "name": "Galata"}, session=admin, admin=True) == {"ok": True, "id": 2}
        services.api("POST", f"/v1/admin/accounts/{users['ms_zeki']}/plus", {}, session=admin, admin=True)  # Lonca kanalı needs Plus

        # Acceptance 2: per-realm names.
        def create(user, name, realm):
            return services.api("POST", "/v1/characters", {"name": name, "class_id": 1, "appearance": {}, "realm_id": realm}, session=sessions[user])
        kemal, selim = create("ms_kemal", "Kemal", 1), create("ms_selim", "Selim", 1)
        orhan, zeki = create("ms_orhan", "Orhan", 2), create("ms_zeki", "Zeki", 2)
        kemal_b = create("ms_kemalb", "Kemal", 2)
        assert kemal_b["realm_id"] == 2 and kemal_b["name"] == "Kemal", "the same name is free in another realm"
        services.api("POST", "/v1/characters", {"name": "kemal", "class_id": 1, "appearance": {}, "realm_id": 1}, session=sessions["ms_kemalb"], status=409)

        path = services.root / "multishard-gameplay.json"
        config, ids = gameplay(services, asset_root)
        path.write_text(json.dumps(config))
        _, port_1a = start_shard(services, path, 1, 32, asset_root)
        _, port_2a = start_shard(services, path, 1, 32, asset_root)
        _, port_b = start_shard(services, path, 2, 2, asset_root)
        instances = dict(tuple(row.split("|")) for row in services.sql("SELECT port::text||'|'||instance FROM world_instances WHERE port>0").splitlines())
        instance_b = instances[str(port_b)]

        # Acceptance 1: least-loaded layer, realm list with populations.
        def world(user, character):
            return services.api("GET", f"/v1/characters/{character['id']}/world", session=sessions[user])
        first = world("ms_kemal", kemal)
        assert first["port"] in (port_1a, port_2a) and first["host"] == ""
        p_kemal = Client(services, "Kemal", sessions["ms_kemal"], kemal["id"], first["port"])
        settle(1.5)  # one shard heartbeat publishes the player count
        second = world("ms_selim", selim)
        assert second["port"] != first["port"] and second["port"] in (port_1a, port_2a), "login picks the least-loaded layer"
        port_a1, port_a2 = first["port"], second["port"]
        p_selim = Client(services, "Selim", sessions["ms_selim"], selim["id"], port_a2)
        assert world("ms_orhan", orhan)["port"] == port_b and world("ms_zeki", zeki)["port"] == port_b
        p_orhan = Client(services, "Orhan", sessions["ms_orhan"], orhan["id"], port_b)
        p_zeki = Client(services, "Zeki", sessions["ms_zeki"], zeki["id"], port_b)
        settle(1.5)
        realms = {row["id"]: row for row in services.api("GET", "/v1/realms", session=sessions["ms_kemal"])}
        assert realms[1]["name"] == "Eminönü" and realms[1]["status"] == "online" and realms[1]["population"] == "low", realms
        assert realms[2]["name"] == "Galata" and realms[2]["status"] == "online" and realms[2]["population"] == "full", realms

        # Outdated (version 2) client: a version-2 goodbye naming the update, then the close.
        with services.connect(port_a1) as raw:
            raw.sendall(frame(1, sessions["ms_kemal"].encode() + struct.pack("!Q", kemal["id"]), version=2))
            raw.settimeout(3)
            data = bytearray()
            try:
                while chunk := raw.recv(65536):
                    data += chunk
            except (ConnectionError, ssl.SSLError, socket.timeout):
                pass
            length = struct.unpack("!I", data[:4])[0]
            assert data[4] == 2 and data[5] == 5, "outdated client gets a version-2 goodbye"
            goodbye = json.loads(data[6:4 + length])
            assert goodbye["code"] == "outdated_client" and goodbye["protocol"] == 3 and goodbye["reason"], goodbye

        # Acceptance 7 (second half): a full destination refuses the phase with a clear message; nothing moves.
        mark = p_kemal.mark()
        p_zeki.action("group_invite", name="Kemal", realm="Eminönü")
        invite = p_kemal.expect(mark, "group_invite")
        assert invite["name"] == "Zeki" and invite["realm"] == "Galata" and invite["from"] == 0, invite
        p_kemal.action("group_accept")
        refusal = p_kemal.expect(mark, "error", code="group")
        assert "dolu" in refusal["message"], refusal
        settle(0.5)
        assert p_kemal.find(mark, "transfer") is None and not p_kemal.closed

        # Acceptance 8: cross-shard whisper by Ad-Realm; realm scoping of names.
        mark = p_zeki.mark()
        p_kemal.action("chat", channel="private", name="Zeki", realm="Galata", text="Galata'ya selam")
        line = p_zeki.expect(mark, "chat", text="Galata'ya selam")
        assert line["channel"] == "private" and line["name"] == "Kemal" and line["realm"] == "Eminönü" and line["from"] == 0, line
        p_zeki.action("chat", channel="private", name="Kemal", text="Kemal-Galata çevrimdışı")
        assert "oyunda değil" in p_zeki.expect(mark, "error", code="target")["message"]

        # Acceptance 3: a realm-B player joins a realm-A party; position, zone and inventory survive the phase.
        before = dict(p_orhan.state)
        mark = p_orhan.mark()
        p_kemal.action("group_invite", name="Orhan", realm="Galata")
        invite = p_orhan.expect(mark, "group_invite")
        assert invite["name"] == "Kemal" and invite["realm"] == "Eminönü", invite
        accepted = time.monotonic()
        p_orhan.action("group_accept")
        timings["party_phase_realm_s"] = phase(p_orhan, mark, "party", port_a1, "Eminönü", accepted)
        assert timings["party_phase_realm_s"] < 5, timings
        wait(lambda: p_orhan.state is not None, "Orhan state on A1")
        after = p_orhan.state
        for field in ("zone", "x", "y", "inventory", "currency", "level"):
            assert after[field] == before[field], (field, before[field], after[field])
        wait(lambda: p_orhan.find(0, "group") and p_kemal.entity(p_orhan) is not None, "Orhan grouped and visible on A1")
        assert p_kemal.entity(p_orhan)["realm"] == "Galata" and p_orhan.entity(p_kemal)["realm"] == "Eminönü"
        assert sorted(p_orhan.find(0, "group")["members"]) == sorted([p_kemal.id, p_orhan.id])

        # Acceptance 4: a member of the other layer (same realm, no realm suffix) joins the same way.
        mark = p_selim.mark()
        p_kemal.action("group_invite", name="Selim")
        assert "realm" not in p_selim.expect(mark, "group_invite")
        accepted = time.monotonic()
        p_selim.action("group_accept")
        timings["party_phase_layer_s"] = phase(p_selim, mark, "party", port_a1, "Eminönü", accepted)
        assert timings["party_phase_layer_s"] < 5, timings
        wait(lambda: p_selim.find(0, "group") and len(p_selim.find(0, "group")["members"]) == 3, "three-member party")

        # Acceptance 8: cross-realm trade on the shared shard.
        before_kemal, before_orhan = p_kemal.state["currency"], p_orhan.state["currency"]
        mark_k, mark_o = p_kemal.mark(), p_orhan.mark()
        p_kemal.action("trade_request", target=p_orhan.id)
        invitation = p_orhan.expect(mark_o, "trade_invite")
        p_orhan.action("trade_accept", revision=invitation["revision"])
        opened = p_kemal.expect(mark_k, "trade", phase="open")
        assert opened["peer_realm"] == "Galata" and p_orhan.expect(mark_o, "trade", phase="open")["peer_realm"] == "Eminönü"
        p_kemal.action("trade_offer", items=[], currency=5)
        wait(lambda: any(e.get("type") == "trade" and e["own"]["currency"] == 5 for e in p_kemal.events[mark_k:]), "trade terms")
        terms = [e for e in p_kemal.events[mark_k:] if e.get("type") == "trade" and e["own"]["currency"] == 5][-1]
        p_kemal.action("trade_accept", revision=terms["revision"])
        p_orhan.action("trade_accept", revision=terms["revision"])
        p_kemal.expect(mark_k, "trade_complete")
        p_orhan.expect(mark_o, "trade_complete")
        wait(lambda: p_kemal.state["currency"] == before_kemal - 5 and p_orhan.state["currency"] == before_orhan + 5, "cross-realm trade settled")

        # Acceptance 3: shared kill XP across realms on the leader's shard.
        blade = next(item["uid"] for item in p_kemal.state["inventory"] if item["item"] == "training_blade")
        p_kemal.action("equip", uid=blade)
        wait(lambda: blade in p_kemal.state["equipment"].values(), "blade equipped")
        xp_orhan, xp_selim = p_orhan.state["xp"], p_selim.state["xp"]
        rat = next(e["id"] for e in p_kemal.record["entities"] if e["kind"] == "enemy")
        p_kemal.move(650, 0)
        p_kemal.action("attack", target=rat)
        wait(lambda: p_orhan.state["xp"] > xp_orhan and p_selim.state["xp"] > xp_selim, "group kill XP shared across realms", 20)
        p_kemal.move(0, 0)

        # Acceptance 8: cross-realm guild, same-shard and off-shard invites, guild chat and roster fan-out.
        mark_k = p_kemal.mark()
        p_kemal.action("guild_create", name="Kervan")
        p_kemal.expect(mark_k, "guild")
        mark_o = p_orhan.mark()
        p_kemal.action("guild_invite", target=p_orhan.id)
        assert p_orhan.expect(mark_o, "guild_invite")["realm"] == "Eminönü"
        p_orhan.action("guild_accept")
        p_orhan.expect(mark_o, "guild")
        mark_z, mark_k = p_zeki.mark(), p_kemal.mark()
        p_kemal.action("guild_invite", name="Zeki", realm="Galata")
        invite = p_zeki.expect(mark_z, "guild_invite")
        assert invite["realm"] == "Eminönü" and invite["guild"]["name"] == "Kervan", invite
        p_zeki.action("guild_accept")
        joined = p_zeki.expect(mark_z, "guild")["guild"]
        assert zeki["id"] in joined["members"] and len(joined["members"]) == 3
        wait(lambda: any(e.get("type") == "guild" and e["guild"] and e["guild"]["revision"] == joined["revision"] for e in p_kemal.events[mark_k:]), "roster change reached the other shard", 5)
        roster = [e for e in p_kemal.events[mark_k:] if e.get("type") == "guild" and e["guild"] and e["guild"]["revision"] == joined["revision"]][-1]["guild"]
        assert roster == joined and p_kemal.state["guild"] == joined
        assert services.sql(f"SELECT count(*) FROM guild_members WHERE guild_id='{joined['id']}'") == "3"
        mark_k, mark_o = p_kemal.mark(), p_orhan.mark()
        p_zeki.action("chat", channel="guild", text="Lonca selamı")
        for client in (p_kemal, p_orhan):
            line = client.expect(mark_k if client is p_kemal else mark_o, "chat", text="Lonca selamı")
            assert line["channel"] == "guild" and line["name"] == "Zeki" and line.get("realm") == ("Galata" if client is p_kemal else None), line

        lockout_zone = None
        if ids:
            # Acceptance 5: the mixed-realm group enters one dungeon copy; the Şef kill lockout is stored per character.
            group = (p_kemal, p_orhan, p_selim)
            for client in group:
                client.move(-950, 600)
                client.action("interact", target=ids["dungeon_door"])
            wait(lambda: all(c.state["zone"].startswith("migrat") and c.state["zone"] != "migrat" for c in group), "group enters a dungeon copy", 15)
            copy = p_kemal.state["zone"]
            assert all(c.state["zone"] == copy for c in group), [c.state["zone"] for c in group]
            chief = ids["dungeon_chief"] + int(copy[len("migrat"):]) * 65536
            p_kemal.action("attack", target=chief)
            wait(lambda: services.sql("SELECT count(*) FROM instance_lockouts WHERE zone='migrat'") == "3", "dungeon Şef lockouts stored", 30)
            lockout_zone = "migrat"
            exit_door = ids["dungeon_exit"] + int(copy[len("migrat"):]) * 65536
            for client in group:
                client.action("interact", target=exit_door)
            wait(lambda: all(c.state["zone"] == "eminonu" for c in group), "group back in eminonu", 15)

            # Acceptance 6: a mixed-realm raid; the minimum is enforced, a cross-shard raid invite phases the joiner, and the
            # whole raid lands in one raid copy.
            mark_k = p_kemal.mark()
            p_kemal.action("raid_convert")
            p_kemal.expect(mark_k, "raid")
            for client in group:
                client.move(-950, -600)
            p_kemal.action("interact", target=ids["raid_door"])
            assert "en az 4" in p_kemal.expect(mark_k, "error", code="raid")["message"]
            mark_z = p_zeki.mark()
            p_kemal.action("raid_invite", name="Zeki", realm="Galata")
            assert p_zeki.expect(mark_z, "raid_invite")["realm"] == "Eminönü"
            accepted = time.monotonic()
            p_zeki.action("raid_accept")
            timings["raid_phase_s"] = phase(p_zeki, mark_z, "party", port_a1, "Eminönü", accepted)
            wait(lambda: p_zeki.find(0, "raid") and sum(len(p) for p in p_zeki.find(0, "raid")["parties"]) == 4, "Zeki joined the raid")
            p_zeki.move(-950, -600)
            raiders = group + (p_zeki,)
            for client in raiders:
                client.action("interact", target=ids["raid_door"])
            wait(lambda: all(c.state["zone"].startswith("buyukhol") and c.state["zone"] != "buyukhol" for c in raiders), "raid enters a raid copy", 15)
            assert len({c.state["zone"] for c in raiders}) == 1, [c.state["zone"] for c in raiders]
            exit_door = ids["raid_exit"] + int(p_kemal.state["zone"][len("buyukhol"):]) * 65536
            for client in raiders:
                client.action("interact", target=exit_door)
            wait(lambda: all(c.state["zone"] == "eminonu" for c in raiders), "raid back in eminonu", 15)

        # Administrator commands reach the right shards.
        queued = services.api("POST", "/v1/admin/world", {"command": "announce", "message": "Bütün sunuculara duyuru"}, session=admin, admin=True)
        assert len(queued["command_ids"]) == 3 and queued["command_id"] == queued["command_ids"][0], queued
        id_list = ",".join(map(str, queued["command_ids"]))
        wait(lambda: services.sql(f"SELECT count(*) FROM world_commands WHERE id IN ({id_list}) AND result->>'ok'='true'") == "3", "announce on every shard")
        assert services.sql(f"SELECT count(DISTINCT target_instance) FROM world_commands WHERE id IN ({id_list})") == "3"
        queued = services.api("POST", "/v1/admin/world", {"command": "heal", "character_id": kemal["id"]}, session=admin, admin=True)
        assert len(queued["command_ids"]) == 1
        wait(lambda: services.sql(f"SELECT result->>'ok' FROM world_commands WHERE id={queued['command_id']}") == "true", "character command on its shard")
        assert services.sql(f"SELECT target_instance FROM world_commands WHERE id={queued['command_id']}") == instances[str(port_a1)]
        queued = services.api("POST", "/v1/admin/world", {"command": "announce", "message": "Galata duyurusu", "realm_id": 2}, session=admin, admin=True)
        assert len(queued["command_ids"]) == 1
        assert services.sql(f"SELECT target_instance FROM world_commands WHERE id={queued['command_id']}") == instance_b

        # Acceptance 7: guests without a group go home after 60 s (Orhan to realm B, Selim to the quieter A layer).
        guests = [(p_orhan, port_b, "Galata"), (p_selim, port_a2, "Eminönü")] + ([(p_zeki, port_b, "Galata")] if ids else [])
        marks = {client.label: client.mark() for client, _, _ in guests}
        left = time.monotonic()
        for client, _, _ in guests:
            client.action("raid_leave" if ids else "group_leave")
        wait(lambda: all(c.find(marks[c.label], "transfer") for c, _, _ in guests), "guests phased home", 80)
        timings["guest_home_s"] = round(time.monotonic() - left, 1)
        assert timings["guest_home_s"] >= 59, timings
        for client, port, realm in guests:
            phase(client, marks[client.label], "home", port, realm, left)
        if lockout_zone:
            # The dungeon lockout survived the phase to another shard.
            mark = p_orhan.mark()
            p_orhan.move(-950, 600)
            p_orhan.action("interact", target=ids["dungeon_door"])
            assert p_orhan.expect(mark, "error", code="lockout")["message"]
            assert p_orhan.state["zone"] == "eminonu"
        for client in CLIENTS:
            client.close()
    print("multi-shard integration passed " + json.dumps({key: round(value, 3) for key, value in timings.items()}) +
          ("" if ids else " (dungeon/raid parts skipped: no --asset-root)"))


if __name__ == "__main__":
    main()
