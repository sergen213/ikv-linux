#!/usr/bin/env python3
"""Exercise owned authenticated native services against disposable PostgreSQL."""

from contextlib import ExitStack
import hashlib
import http.client
import json
from pathlib import Path
import random
import socket
import ssl
import struct
import sys
import time
import traceback

from isolated_services import Services, WorldPeer, arguments, frame, hello, world_admission_invariants


PASSWORD = "Independent-test-Password!29"
NEW_PASSWORD = "Independent-reset-Password!73"


def expect_error(services, method, path, body=None, *, session=None, status=400, admin=False, source_ip=None):
    result = services.api(method, path, body, session=session, status=status, admin=admin, source_ip=source_ip)
    assert isinstance(result.get("error"), dict) and result["error"].get("code"), "API error lacks structured code"
    return result["error"]["code"]


def raw_http(services, request, expected):
    with services.connect(services.auth_port) as connection:
        connection.settimeout(5)
        connection.sendall(request)
        response = http.client.HTTPResponse(connection)
        response.begin()
        assert response.status == expected, ("HTTP parser status", expected, response.status)
        result = json.loads(response.read())
        assert result["error"]["code"]


def parser_and_tls(services):
    raw_http(services, b"POST /v1/register HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Type: application/json\r\n\r\n{", 400)
    raw_http(services, b"POST /v1/register HTTP/1.1\r\nHost: localhost\r\nContent-Length: 65537\r\n\r\n", 413)
    raw_http(services, b"GET /v1/status HTTP/1.1\r\nHost: localhost\r\nX-Long: " + b"x" * 9000 + b"\r\n\r\n", 431)
    raw_http(services, b"POST /v1/register HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n{}", 400)
    for context, hostname in ((ssl.create_default_context(), "localhost"), (services.context, "wrong-host.invalid")):
        try:
            connection = services.connect(services.auth_port, context=context, hostname=hostname)
        except ssl.SSLCertVerificationError:
            pass
        else:
            connection.close()
            raise AssertionError("untrusted CA or wrong SAN was accepted")
    with services.connect(services.auth_port, hostname="127.0.0.1"):
        pass
    services.api("GET", "/v1/status")
    expect_error(services, "GET", "/v1/status?session=not-a-session", status=400)


def authentication(services):
    services.secrets.update((PASSWORD, NEW_PASSWORD))
    users = {}
    for username in ("test_admin", "test_alice", "test_bob", "test_reset", "test_banned"):
        registered = services.api("POST", "/v1/register", {"username": username, "password": PASSWORD})
        users[username] = {"id": registered["account_id"]}
    expect_error(services, "POST", "/v1/register", {"username": "test_alice", "password": PASSWORD}, status=409)
    expect_error(services, "POST", "/v1/register", {"username": "invalid name", "password": PASSWORD})
    expect_error(services, "POST", "/v1/register", {"username": "role_injection", "password": PASSWORD, "admin": True})
    expect_error(services, "POST", "/v1/login", {"username": "test_alice", "password": "incorrect-password"}, status=401)
    services.stop(services.auth)
    services.start_auth(admin="test_admin")
    for username, user in users.items():
        user["session"] = services.api("POST", "/v1/login", {"username": username, "password": PASSWORD})["session"]
        assert len(user["session"]) == 64 and all(c in "0123456789abcdef" for c in user["session"])
    admin = users["test_admin"]["session"]
    alice = users["test_alice"]["session"]
    expect_error(services, "GET", "/v1/admin/status", session=admin, status=404)
    expect_error(services, "GET", "/v1/admin/status", status=401, admin=True)
    expect_error(services, "GET", "/v1/admin/status", session=alice, status=403, admin=True)
    status = services.api("GET", "/v1/admin/status", session=admin, admin=True)
    assert status["accounts"] == len(users) and status["characters"] == 0
    assert services.sql("SELECT count(*) FROM accounts WHERE admin") == "1"
    salts = services.sql("SELECT count(DISTINCT password_salt) FROM accounts")
    assert salts == str(len(users)), "equal passwords did not receive independent random salts"
    assert services.sql("SELECT count(*) FROM accounts WHERE password_hash='" + PASSWORD + "'") == "0"
    for user in users.values():
        hashed = hashlib.sha256(user["session"].encode()).hexdigest()
        assert services.sql("SELECT count(*) FROM sessions WHERE token_hash='" + hashed + "'") == "1"
        assert services.sql("SELECT count(*) FROM sessions WHERE token_hash='" + user["session"] + "'") == "0"
    reset = users["test_reset"]
    expired = services.api("POST", f"/v1/admin/accounts/{reset['id']}/reset", {}, session=admin, admin=True)["token"]
    expired_hash = hashlib.sha256(expired.encode()).hexdigest()
    services.sql("UPDATE password_resets SET expires_at=now()-interval '1 second' WHERE token_hash='" + expired_hash + "'")
    expect_error(services, "POST", "/v1/reset", {"username": "test_reset", "token": expired, "new_password": NEW_PASSWORD})
    token = services.api("POST", f"/v1/admin/accounts/{reset['id']}/reset", {}, session=admin, admin=True)["token"]
    expect_error(services, "POST", "/v1/reset", {"username": "test_reset", "token": "0" * 64, "new_password": NEW_PASSWORD})
    services.api("POST", "/v1/reset", {"username": "test_reset", "token": token, "new_password": NEW_PASSWORD})
    expect_error(services, "POST", "/v1/reset", {"username": "test_reset", "token": token, "new_password": PASSWORD})
    expect_error(services, "GET", "/v1/characters", session=reset["session"], status=401)
    reset["session"] = services.api("POST", "/v1/login", {"username": "test_reset", "password": NEW_PASSWORD})["session"]
    services.api("POST", "/v1/logout", {}, session=reset["session"])
    expect_error(services, "GET", "/v1/characters", session=reset["session"], status=401)
    # Forgotten tokens (the launcher keeps its session only in memory) must not lock the account out at the cap: the oldest gives way.
    logins = [services.api("POST", "/v1/login", {"username": "test_reset", "password": NEW_PASSWORD}, source_ip="127.0.0.6")["session"] for _ in range(9)]
    expect_error(services, "GET", "/v1/characters", session=logins[0], status=401)
    assert services.api("GET", "/v1/characters", session=logins[-1])["characters"] == []
    assert services.sql(f"SELECT count(*) FROM sessions WHERE account_id={reset['id']}") == "8"
    banned = users["test_banned"]
    services.api("POST", f"/v1/admin/accounts/{banned['id']}/ban", {"reason": "synthetic moderation check", "duration_seconds": 0}, session=admin, admin=True)
    expect_error(services, "GET", "/v1/characters", session=banned["session"], status=401)
    expect_error(services, "POST", "/v1/login", {"username": "test_banned", "password": PASSWORD}, status=401)
    services.api("POST", f"/v1/admin/accounts/{banned['id']}/unban", {}, session=admin, admin=True)
    banned["session"] = services.api("POST", "/v1/login", {"username": "test_banned", "password": PASSWORD})["session"]
    expired_session = hashlib.sha256(banned["session"].encode()).hexdigest()
    services.sql("UPDATE sessions SET expires_at=now()-interval '1 second' WHERE token_hash='" + expired_session + "'")
    expect_error(services, "GET", "/v1/characters", session=banned["session"], status=401)
    # Plus Paket entitlement (migration 005) is administrator-granted only and listed on the accounts page.
    assert expect_error(services, "POST", f"/v1/admin/accounts/{banned['id']}/plus", {}, session=users["test_alice"]["session"], status=403, admin=True) == "forbidden"
    services.api("POST", f"/v1/admin/accounts/{banned['id']}/plus", {}, session=admin, admin=True)
    listed = services.api("GET", "/v1/admin/accounts?search=test_banned", session=admin, admin=True)["accounts"]
    assert [a["plus"] for a in listed if a["username"] == "test_banned"] == [True], "Plus grant not stored"
    services.api("POST", f"/v1/admin/accounts/{banned['id']}/unplus", {}, session=admin, admin=True)
    assert services.sql(f"SELECT plus FROM accounts WHERE id={int(banned['id'])}") == "f", "Plus revoke not stored"
    return users


def auth_security_regressions(services, users):
    admin = users["test_admin"]["session"]
    with ExitStack() as stalled:
        peers = [stalled.enter_context(socket.create_connection(("127.0.0.1", services.auth_port), timeout=2)) for _ in range(8)]
        services.until(lambda: services.accepted(services.auth, peers), "eight native accepted public pre-TLS sockets", timeout=3)
        started = time.monotonic()
        assert services.api("GET", "/v1/health", admin=True)["ok"] is True
        private_login = services.api("POST", "/v1/login", {"username": "test_admin", "password": PASSWORD}, admin=True)["session"]
        private_status = services.api("GET", "/v1/admin/status", session=private_login, admin=True)
        assert private_status["accounts"] == len(users)
        assert time.monotonic() - started < 5, "management waited for public handshake expiry"
        assert services.accepted(services.auth, peers), "public stalls expired before management completed"
        services.api("POST", "/v1/logout", {}, session=private_login, admin=True)
    # Privilege injection must be rejected before charging any credential quota;
    # an attacker's chosen username must not throttle another route/IP/listener.
    for username in ("test_admin", "test_banned"):
        for _ in range(12):
            expect_error(services, "POST", "/v1/register", {"username": username, "password": PASSWORD, "admin": True}, source_ip="127.0.0.2")
    private_login = services.api("POST", "/v1/login", {"username": "test_admin", "password": PASSWORD}, admin=True, source_ip="127.0.0.3")["session"]
    assert services.api("GET", "/v1/admin/status", session=private_login, admin=True, source_ip="127.0.0.3")["accounts"] == len(users)
    services.api("POST", "/v1/logout", {}, session=private_login, admin=True, source_ip="127.0.0.3")
    credentials = {"username": "test_banned", "password": PASSWORD}
    victim_login = services.api("POST", "/v1/login", credentials, source_ip="127.0.0.3")["session"]
    assert services.api("GET", "/v1/characters", session=victim_login, source_ip="127.0.0.3")["characters"] == []
    for _ in range(11):
        expect_error(services, "POST", "/v1/login", {"username": "test_banned", "password": "Incorrect-regression-password"}, status=401, source_ip="127.0.0.3")
    assert expect_error(services, "POST", "/v1/login", credentials, status=429, source_ip="127.0.0.3") == "rate_limited"
    # The valid-shape login cap remains enforced, but neither the same account on
    # the management listener nor the reset operation inherits that exhausted key.
    management_victim = services.api("POST", "/v1/login", credentials, admin=True, source_ip="127.0.0.3")["session"]
    expect_error(services, "GET", "/v1/admin/status", session=management_victim, status=403, admin=True, source_ip="127.0.0.3")
    token = services.api("POST", f"/v1/admin/accounts/{users['test_banned']['id']}/reset", {}, session=admin, admin=True)["token"]
    services.api("POST", "/v1/reset", {"username": "test_banned", "token": token, "new_password": NEW_PASSWORD}, source_ip="127.0.0.3")
    expect_error(services, "GET", "/v1/characters", session=victim_login, status=401, source_ip="127.0.0.3")
    expect_error(services, "GET", "/v1/characters", session=management_victim, status=401, admin=True, source_ip="127.0.0.3")
    users["test_banned"]["session"] = services.api("POST", "/v1/login", {"username": "test_banned", "password": NEW_PASSWORD}, source_ip="127.0.0.4")["session"]


def characters(services, users):
    result = []
    for username, name in (("test_alice", "SyntheticAlice"), ("test_bob", "SyntheticBob")):
        user = users[username]
        created = services.api("POST", "/v1/characters", {"name": name, "class_id": 1, "appearance": {}}, session=user["session"])
        assert created["account_id"] == user["id"] and created["name"] == name
        result.append(created)
    alice = users["test_alice"]["session"]
    bob = users["test_bob"]["session"]
    for session, own in ((alice, result[0]), (bob, result[1])):
        listed = services.api("GET", "/v1/characters", session=session)["characters"]
        assert [character["id"] for character in listed] == [own["id"]]
    expect_error(services, "DELETE", f"/v1/characters/{result[0]['id']}", {"confirm_name": result[0]["name"]}, session=bob, status=404)
    expect_error(services, "DELETE", f"/v1/characters/{result[0]['id']}", {"confirm_name": "wrong"}, session=alice)
    expect_error(services, "POST", "/v1/characters", {"name": "BadClass", "class_id": 99, "appearance": {}}, session=alice)
    expect_error(services, "POST", "/v1/characters", {"name": "ForgedState", "class_id": 1, "appearance": {}, "state": {"currency": 999999}}, session=alice)
    disposable = services.api("POST", "/v1/characters", {"name": "DeleteMe", "class_id": 1, "appearance": {}}, session=alice)
    services.api("DELETE", f"/v1/characters/{disposable['id']}", {"confirm_name": "DeleteMe"}, session=alice)
    assert services.sql(f"SELECT deleted_at IS NOT NULL FROM characters WHERE id={disposable['id']}") == "t"
    return result


def state(peer, predicate=lambda state: True):
    record = peer.until(lambda kind, body: kind == 6 and predicate(body["character"]["state"]))
    return record["character"]["state"]


def event(client, name, **fields):
    record = client.until(lambda kind, body: kind == 6 and any(e.get("type") == name and all(e.get(k) == v for k, v in fields.items()) for e in body["events"]))
    return next(e for e in record["events"] if e.get("type") == name and all(e.get(k) == v for k, v in fields.items()))


def move(peer, x, y, sequence):
    peer.connection.sendall(frame(7, struct.pack("!Iii", sequence, x, y)))
    return state(peer, lambda current: (current["x"], current["y"]) == (x, y))


def quantities(saved):
    result = {}
    uids = []
    for item in saved["inventory"]:
        assert item["quantity"] > 0
        result[item["item"]] = result.get(item["item"], 0) + item["quantity"]
        uids.append(item["uid"])
    assert len(uids) == len(set(uids)), "duplicate inventory identity"
    return result


def saved(services, character):
    return json.loads(services.sql(f"SELECT state::text FROM characters WHERE id={character}"))


def rejected_peer(services, payload):
    # Malformed peers get a bare close; an authenticated refusal gets exactly one goodbye {"reason"} (server_main refuse()).
    with services.connect(services.world_port) as connection:
        connection.sendall(payload)
        connection.settimeout(2)
        received = bytearray()
        try:
            while chunk := connection.recv(65536):
                received += chunk
        except (ConnectionResetError, ssl.SSLEOFError):
            pass
        if received:
            length = struct.unpack("!I", received[:4])[0]
            assert len(received) == length + 4 and received[4:6] == bytes((2, 5)) and json.loads(received[6:])["reason"], "malformed or unauthorized peer received a record"


def revoked_world_peer(peer, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            peer.receive()
        except socket.timeout:
            continue
        except (EOFError, ConnectionResetError, ssl.SSLEOFError):
            return
    raise AssertionError("revoked or banned identity remained live in the world")


def world(services, users, characters):
    services.start_world()
    gameplay = json.loads((services.root / "independent-gameplay.json").read_text())
    combat_range = gameplay["combat_range"]
    alice, bob = users["test_alice"]["session"], users["test_bob"]["session"]
    aid, bid = (character["id"] for character in characters)
    rejected_peer(services, hello(alice, bid))
    rejected_peer(services, hello("0" * 64, aid))
    with ExitStack() as stack:
        first = stack.enter_context(WorldPeer(services, alice, aid))
        second = stack.enter_context(WorldPeer(services, bob, bid))
        assert first.id != second.id
        initial_record = first.until(lambda kind, body: kind == 6)
        initial_a, initial_b = initial_record["character"]["state"], state(second)
        questgiver, merchant, enemy = (next(entity["id"] for entity in initial_record["entities"] if entity["kind"] == kind) for kind in ("questgiver", "merchant", "enemy"))
        assert all(identity >= 0x80000000 for identity in (questgiver, merchant, enemy))
        services.until(lambda: services.api("GET", "/v1/status")["world"].get("player_count") == 2, "public authoritative player count")
        assert quantities(initial_a)["healing_tonic"] == 3
        rejected_peer(services, hello(alice, aid))
        expect_error(services, "DELETE", f"/v1/characters/{aid}", {"confirm_name": "SyntheticAlice"}, session=alice, status=409)
        for payload in (struct.pack("!I", 0), struct.pack("!I", 65536), frame(1), frame(1, b"x" * 72, version=1), frame(3, struct.pack("!Ibb", 1, 1, 0)), frame(8, b"not-json")):
            rejected_peer(services, payload)
        generator = random.Random(0x1A2B3C)
        for _ in range(32):
            payload = generator.randbytes(generator.randrange(0, 128))
            rejected_peer(services, frame(generator.randrange(1, 256), payload, version=generator.randrange(3, 256)))
        # Simultaneous idle TLS peers must not starve authenticated simulation.
        with ExitStack() as load:
            for _ in range(8):
                load.enter_context(services.connect(services.world_port))
            state(first)
        first.action("state", currency=999999, inventory=[])
        event(first, "error")
        assert state(first)["currency"] == initial_a["currency"]
        first.action("chat", channel="local", text="Owned synthetic chat: Türkçe ışık")
        event(second, "chat", text="Owned synthetic chat: Türkçe ışık")
        first.action("moderate", target=second.id, operation="kick")
        event(first, "error")
        state(second)
        move(first, 200, 0, 1)
        move(second, 200, 0, 1)
        first.until(lambda kind, body: kind == 4 and {(p["id"], p["x"], p["y"]) for p in body["players"]} == {(first.id, 200, 0), (second.id, 200, 0)})
        first.action("interact", target=questgiver)
        dialogue = event(first, "dialogue", target=questgiver)
        assert "first_hunt" in dialogue["quests"] and dialogue["text"]
        first.action("quest_accept", target=questgiver, quest="first_hunt")
        state(first, lambda current: "first_hunt" in current["quests"])
        blade = next(item["uid"] for item in initial_a["inventory"] if item["item"] == "training_blade")
        first.action("equip", uid=blade)
        state(first, lambda current: blade in current["equipment"].values())
        first.action("skill_learn", skills={"depar": 1})
        state(first, lambda current: current["skills"].get("depar") == 1)
        first.action("skill", target=first.id, skill="depar")
        warded = state(first, lambda current: any(buff["id"] == "depar" for buff in current["buffs"]))
        assert warded["mana"] == initial_a["mana"] - gameplay["skills"]["depar"]["mana"]
        first.action("skill", target=enemy, skill="meteorit")
        event(first, "error")
        assert state(first)["mana"] == warded["mana"]
        first.connection.sendall(frame(7, struct.pack("!Iii", 2, 650, 0)))
        # Player arrival alone does not put a patrolling enemy in combat range.
        first.until(lambda kind, body: kind == 6 and
                    (body["character"]["state"]["x"], body["character"]["state"]["y"]) == (650, 0) and
                    any(entity["id"] == enemy and entity["hp"] > 0 and
                        (entity["x"] - body["character"]["state"]["x"]) ** 2 +
                        (entity["y"] - body["character"]["state"]["y"]) ** 2 <= combat_range ** 2
                        for entity in body["entities"]))
        # One click-to-attack request: the server keeps swinging at the weapon interval until the kill.
        first.action("attack", target=enemy)
        # The crit-free fixture blade leaves the rat wounded but alive after the first swing; the kill needs a repeated swing.
        first.until(lambda kind, body: kind == 6 and any(entity["id"] == enemy and 0 < entity["hp"] < entity["max_hp"] for entity in body["entities"]))
        state(first, lambda current: current["quests"]["first_hunt"]["progress"] >= 1)
        move(first, 800, 0, 3)
        first.action("loot", target=enemy)
        for slot, entry in enumerate(event(first, "loot_open", target=enemy)["items"]):
            if entry:
                first.action("loot_take", target=enemy, slot=slot)
        state(first, lambda current: quantities(current).get("rat_tail", 0) >= 1)
        move(first, 200, 0, 4)
        first.action("quest_complete", target=questgiver, quest="first_hunt")
        rewarded = state(first, lambda current: "first_hunt" in current.get("quests_done", []))
        assert "first_hunt" not in rewarded["quests"]
        assert (rewarded["level"], rewarded["xp"]) == (2, 30)
        assert rewarded["currency"] == initial_a["currency"] + 30
        first.action("quest_complete", target=questgiver, quest="first_hunt")
        event(first, "error")
        assert state(first)["currency"] == rewarded["currency"], "quest reward was duplicated"
        move(first, 400, 0, 5)
        first.action("buy", target=merchant, item="healing_tonic", quantity=1)
        bought = state(first, lambda current: quantities(current)["healing_tonic"] == quantities(rewarded)["healing_tonic"] + 1)
        assert bought["currency"] < rewarded["currency"]
        first.action("buy", target=merchant, item="healing_tonic", quantity=-1)
        event(first, "error")
        assert state(first)["currency"] == bought["currency"]
        move(second, 400, 0, 2)
        # Admission enriches live state only; equip persists Bob's defaults without
        # spending currency or changing progression before the DB trade baseline.
        second.action("equip", uid=next(item["uid"] for item in initial_b["inventory"] if item["item"] == "training_blade"))
        event(second, "ack", action="equip")
        before_a, before_b = state(first), state(second)
        assert all(before_b[field] == initial_b[field] for field in ("currency", "level", "xp", "mana")), "baseline equip changed Bob's economy or progression"
        tonic = next(item["uid"] for item in before_a["inventory"] if item["item"] == "healing_tonic")
        first.action("trade_request", target=second.id)
        invitation = event(second, "trade_invite", **{"from": first.id})
        event(first, "trade", phase="invited", revision=invitation["revision"])
        second.action("trade_accept", revision=invitation["revision"])
        event(first, "trade", phase="open", revision=invitation["revision"])
        event(second, "trade", phase="open", revision=invitation["revision"])
        first.action("trade_offer", items=[{"uid": tonic, "quantity": 100000}], currency=7)
        event(first, "error")
        assert saved(services, aid)["currency"] == before_a["currency"]
        assert saved(services, bid)["currency"] == before_b["currency"]
        first.action("trade_offer", items=[{"uid": tonic, "quantity": 1}], currency=7)
        terms_a = event(first, "trade", phase="open")
        terms_b = event(second, "trade", phase="open", revision=terms_a["revision"])
        assert terms_a["revision"] > invitation["revision"]
        assert terms_a["peer"] == second.id and terms_b["peer"] == first.id
        assert terms_a["peer_name"] == characters[1]["name"] and terms_b["peer_name"] == characters[0]["name"]
        assert terms_a["initiator"] == terms_b["initiator"] == first.id
        assert terms_a["own"] == terms_b["other"] and terms_a["other"] == terms_b["own"]
        assert terms_b["other"]["currency"] == 7
        assert terms_b["other"]["items"] == [{"uid": tonic, "item": "healing_tonic", "name": initial_record["catalogs"]["items"]["healing_tonic"]["name"], "quantity": 1}]
        assert not terms_a["own"]["accepted"] and not terms_a["other"]["accepted"]
        first.action("trade_accept", revision=terms_a["revision"])
        accepted_a = event(first, "trade", revision=terms_a["revision"])
        accepted_b = event(second, "trade", revision=terms_a["revision"])
        assert accepted_a["own"]["accepted"] and not accepted_a["other"]["accepted"]
        assert accepted_b["other"]["accepted"] and not accepted_b["own"]["accepted"]
        assert saved(services, aid)["currency"] == before_a["currency"]
        assert saved(services, bid)["currency"] == before_b["currency"]
        second.action("trade_offer", items=[], currency=0)
        revised_a = event(first, "trade", phase="open")
        revised_b = event(second, "trade", revision=revised_a["revision"])
        assert revised_a["revision"] > terms_a["revision"]
        assert not revised_a["own"]["accepted"] and not revised_a["other"]["accepted"]
        assert revised_a["own"] == revised_b["other"] and revised_a["other"] == revised_b["own"]
        first.action("trade_accept", revision=terms_a["revision"])
        assert event(first, "error")["code"] == "trade_revision"
        second.action("trade_accept")
        assert event(second, "error")["code"] == "trade_revision"
        assert saved(services, aid)["currency"] == before_a["currency"]
        assert saved(services, bid)["currency"] == before_b["currency"]
        first.action("trade_accept", revision=revised_a["revision"])
        second.action("trade_accept", revision=revised_b["revision"])
        event(first, "trade_complete", peer=second.id)
        event(second, "trade_complete", peer=first.id)
        # The acknowledgement follows ONE transaction, so both persisted rows must
        # already reflect the transfer, including exact currency conservation.
        after_a, after_b = saved(services, aid), saved(services, bid)
        assert after_a["currency"] == before_a["currency"] - 7
        assert after_b["currency"] == before_b["currency"] + 7
        assert quantities(after_a)["healing_tonic"] == quantities(before_a)["healing_tonic"] - 1
        assert quantities(after_b)["healing_tonic"] == quantities(before_b)["healing_tonic"] + 1
        combined_uids = [item["uid"] for current in (after_a, after_b) for item in current["inventory"]]
        assert len(combined_uids) == len(set(combined_uids))
        first.action("trade_accept", revision=revised_a["revision"])
        event(first, "error")
        assert saved(services, aid)["currency"] == after_a["currency"]
        first.action("trade_request", target=second.id)
        next_invitation = event(second, "trade_invite", **{"from": first.id})
        assert next_invitation["revision"] > revised_a["revision"]
        second.action("trade_accept", revision=revised_a["revision"])
        assert event(second, "error")["code"] == "trade_revision"
        first.action("trade_cancel")
        event(first, "ack", action="trade_cancel")
        first.action("guild_create", name="SyntheticGuild")
        guild = state(first, lambda current: current.get("guild", {}).get("name") == "SyntheticGuild")["guild"]
        assert guild["leader_character_id"] == aid and guild["members"] == [aid]
        first.action("guild_invite", target=second.id)
        event(second, "guild_invite", **{"from": first.id})
        second.action("guild_accept")
        state(second, lambda current: set(current.get("guild", {}).get("members", [])) == {aid, bid})
        state(first, lambda current: set(current.get("guild", {}).get("members", [])) == {aid, bid})
        assert services.sql(f"SELECT count(*) FROM guild_members WHERE character_id IN ({aid},{bid})") == "2"
        # Guide "Lonca kanalı": Plus Paket only; an administrator's grant reaches the world within a session heartbeat.
        admin = users["test_admin"]["session"]
        first.action("chat", channel="guild", text="Owned persistent guild chat")
        event(first, "error", code="plus")
        services.api("POST", f"/v1/admin/accounts/{users['test_alice']['id']}/plus", {}, session=admin, admin=True)
        granted_tick = first.until(lambda kind, body: kind == 6)["tick"]
        first.until(lambda kind, body: kind == 6 and body["tick"] >= granted_tick + 24)
        first.action("chat", channel="guild", text="Owned persistent guild chat")
        event(second, "chat", channel="guild", text="Owned persistent guild chat")
        first.connection.sendall(frame(7, struct.pack("!Iii", 6, 1300, 500)))
        event(first, "correction", reason="collision")
        collision = state(first)
        assert not (1000 <= collision["x"] <= 1200 and 300 <= collision["y"] <= 900), "authoritative movement entered configured solid collision"
        move(first, 0, 0, 7)
        # A destination burst is coalesced to one route per tick (the newest held): the peer stays connected and ends at the last click.
        first.connection.sendall(b"".join(frame(7, struct.pack("!Iii", sequence, 100 * (sequence % 3), 0)) for sequence in range(8, 407)) +
                                 frame(7, struct.pack("!Iii", 407, 300, 0)))
        state(first, lambda current: (current["x"], current["y"]) == (300, 0))
        move(first, 0, 0, 408)
        first.action("zone", zone="old_town")
        event(first, "error", code="permission")
        first.action("interact", target=2147493652)  # tests/independent_content.json <loc> door eminonu -> old_town
        state(first, lambda current: current["zone"] == "old_town" and (current["x"], current["y"]) == (0, 0))
        second.until(lambda kind, body: kind == 4 and first.id not in {p["id"] for p in body["players"]})
        first.action("interact", target=2147493653)  # door old_town -> eminonu
        state(first, lambda current: current["zone"] == "eminonu" and (current["x"], current["y"]) == (0, 0))
        services.api("POST", "/v1/admin/moderation", {"character_id": aid, "action": "mute", "duration_seconds": 60}, session=admin, admin=True)
        muted_tick = first.until(lambda kind, body: kind == 6)["tick"]
        first.until(lambda kind, body: kind == 6 and body["tick"] >= muted_tick + 24)
        first.action("chat", channel="local", text="This muted message must fail")
        event(first, "error")
        services.api("POST", "/v1/admin/moderation", {"character_id": aid, "action": "unmute", "duration_seconds": 0}, session=admin, admin=True)
    services.until(lambda: services.sql(f"SELECT count(*) FROM characters WHERE id IN ({aid},{bid}) AND lease_instance IS NOT NULL") == "0", "released character leases")
    persisted = {identity: saved(services, identity) for identity in (aid, bid)}
    services.stop(services.world)
    services.start_world()
    for session, identity in ((alice, aid), (bob, bid)):
        with WorldPeer(services, session, identity) as reconnect:
            current = state(reconnect)
            assert current["currency"] == persisted[identity]["currency"]
            assert quantities(current) == quantities(persisted[identity])
            assert current["quests"] == persisted[identity]["quests"] and current["quests_done"] == persisted[identity]["quests_done"]
            assert current["guild"] == persisted[identity]["guild"]
            assert all(current[field] == persisted[identity][field] for field in ("zone", "x", "y", "level", "xp", "hp", "mana"))
    services.until(lambda: services.sql(f"SELECT count(*) FROM characters WHERE id IN ({aid},{bid}) AND lease_instance IS NOT NULL") == "0", "reconnect lease release")
    native = services.start(services.args.client, "--headless", "--host", "localhost", "--port", services.world_port, "--ca", services.ca, "--session-stdin", "--character", aid, "--ticks", "5", stdin=alice + "\n")
    services.until(lambda: native["process"].poll() is not None and native["eof"], "native persisted client exit")
    assert native["process"].returncode == 0, services.diagnostics()
    assert any(record.get("self") and record.get("players") for record in native["records"])
    assert any(record.get("type") == "state" and "first_hunt" in record["state"]["character"]["state"].get("quests_done", []) for record in native["records"])
    services.until(lambda: services.sql(f"SELECT lease_instance IS NULL FROM characters WHERE id={aid}") == "t", "native client lease release")
    with WorldPeer(services, alice, aid) as revoked:
        services.api("POST", "/v1/logout", {}, session=alice)
        revoked_world_peer(revoked)
    users["test_alice"]["session"] = services.api("POST", "/v1/login", {"username": "test_alice", "password": PASSWORD})["session"]
    with WorldPeer(services, bob, bid) as banned_peer:
        services.api("POST", f"/v1/admin/accounts/{users['test_bob']['id']}/ban", {"reason": "owned live-session revocation check", "duration_seconds": 0}, session=admin, admin=True)
        revoked_world_peer(banned_peer)
    services.api("POST", f"/v1/admin/accounts/{users['test_bob']['id']}/unban", {}, session=admin, admin=True)
    users["test_bob"]["session"] = services.api("POST", "/v1/login", {"username": "test_bob", "password": PASSWORD})["session"]
    return persisted


def admin_raw(services, method, path, body=None, headers=None):
    connection = http.client.HTTPSConnection("localhost", services.admin_port, context=services.context, timeout=5)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read(), response.getheader("Content-Type")
    finally:
        connection.close()


def admin_command(services, admin, path, body):
    command_id = services.api("POST", path, body, session=admin, admin=True)["command_id"]
    found = {}

    def recorded():
        time.sleep(0.2)  # stay well below the 600/minute administrator request cap
        for command in services.api("GET", "/v1/admin/status", session=admin, admin=True)["commands"]:
            if command["id"] == command_id and command["result"] is not None:
                found["result"] = command["result"]
        return "result" in found
    services.until(recorded, f"world result for {body}")
    return found["result"]


def admin_web(services, users, characters):
    """Private server-control routes: RBAC/CSRF on every route, validation, and real world effects."""
    admin, alice = users["test_admin"]["session"], users["test_alice"]["session"]
    aid, bid = (character["id"] for character in characters)
    routes = [("POST", "/v1/admin/world", {"command": "announce", "message": "rbac"}),
              ("POST", "/v1/admin/control", {"command": "cancel"}),
              ("POST", "/v1/admin/bots", {"action": "despawn", "all": True}),
              ("POST", "/v1/admin/settings", {"xp_rate": 100, "drop_rate": 100, "regen_rate": 100, "disabled_zones": []}),
              ("GET", "/v1/admin/settings", None),
              ("GET", "/v1/admin/backups/ikv-1-00.sql", None)]
    queued = services.sql("SELECT count(*) FROM world_commands")
    for method, path, body in routes:
        expect_error(services, method, path, body, session=admin, status=404)  # public listener never serves management
        expect_error(services, method, path, body, status=401, admin=True)
        expect_error(services, method, path, body, session=alice, status=403, admin=True)
        encoded = None if body is None else json.dumps(body).encode()
        # Cross-site forgery: a foreign Origin is refused even with a valid administrator bearer...
        status, _, _ = admin_raw(services, method, path, encoded, {"Authorization": "Bearer " + admin, "Origin": "https://attacker.invalid", "Content-Type": "application/json"})
        assert status == 403, (path, status)
        if body is not None:
            # ...and a form/text body (what a cross-site form can send without preflight) is rejected before routing.
            status, _, _ = admin_raw(services, method, path, encoded, {"Authorization": "Bearer " + admin, "Content-Type": "text/plain"})
            assert status == 415, (path, status)
    assert services.sql("SELECT count(*) FROM world_commands") == queued, "refused request enqueued a world command"
    for path, body in (("/v1/admin/world", {"command": "shell", "message": "x"}), ("/v1/admin/world", {"command": "announce", "message": ""}),
                       ("/v1/admin/world", {"command": "announce", "message": "line\nbreak"}), ("/v1/admin/world", {"command": "kick"}),
                       ("/v1/admin/world", {"command": "clock", "hour": 24}), ("/v1/admin/control", {"command": "restart", "delay_seconds": 3601}),
                       ("/v1/admin/bots", {"action": "spawn", "count": 0, "behaviour": "town"}), ("/v1/admin/bots", {"action": "spawn", "count": 1, "behaviour": "cheat"}),
                       ("/v1/admin/bots", {"action": "despawn", "ids": []}), ("/v1/admin/bots", {"action": "despawn", "all": "yes"}),
                       ("/v1/admin/settings", {"xp_rate": 5, "drop_rate": 100, "regen_rate": 100, "disabled_zones": []}),
                       ("/v1/admin/settings", {"xp_rate": 100, "drop_rate": 100, "regen_rate": 100, "disabled_zones": "old_town"})):
        expect_error(services, "POST", path, body, session=admin, admin=True)
    with ExitStack() as stack:
        first = stack.enter_context(WorldPeer(services, alice, aid))
        second = stack.enter_context(WorldPeer(services, users["test_bob"]["session"], bid))
        start = state(first)
        assert start["zone"] == "eminonu"
        def live_world():
            time.sleep(0.2)
            return next(w for w in services.api("GET", "/v1/admin/status", session=admin, admin=True)["worlds"] if w["online"])["status"]
        # The heartbeat publishes status once per second, after it executes queued commands.
        services.until(lambda: {aid, bid} <= {player["character_id"] for player in live_world()["online"]}, "online players in status")
        world = live_world()
        assert world["memory_kib"] > 0 and world["uptime_seconds"] >= 0 and world["tick_ms"] >= 0 and world["countdown"] is None
        assert {zone["zone"] for zone in world["zones"]} >= {"eminonu", "old_town"}
        message = "Owned web announcement: Türkçe duyuru"
        command_id = services.api("POST", "/v1/admin/world", {"command": "announce", "message": message}, session=admin, admin=True)["command_id"]
        event(first, "announce", text=message)
        event(second, "announce", text=message)
        assert services.sql(f"SELECT result->>'ok' FROM world_commands WHERE id={command_id}") == "true"
        assert abs(admin_command(services, admin, "/v1/admin/world", {"command": "clock", "hour": 3})["clock_hour"] - 3) < 0.05
        assert first.until(lambda kind, body: kind == 6 and "clock" in body and abs(body["clock"]["hour"] - 3) < 0.05)
        applied = admin_command(services, admin, "/v1/admin/settings", {"xp_rate": 200, "drop_rate": 100, "regen_rate": 100, "disabled_zones": ["old_town", "missing_zone"]})
        assert applied["ok"] and applied["unknown_zones"] == ["missing_zone"]
        assert services.api("GET", "/v1/admin/settings", session=admin, admin=True)["settings"]["disabled_zones"] == ["missing_zone", "old_town"]
        services.until(lambda: (lambda world: world["rates"]["xp_rate"] == 200 and next(zone for zone in world["zones"] if zone["zone"] == "old_town")["disabled"])(live_world()), "published rates and closed zone")
        first.action("interact", target=2147493652)  # door eminonu -> old_town is closed for non-administrators
        assert event(first, "error")["code"] == "zone_disabled"
        assert state(first)["zone"] == "eminonu"
        refused = admin_command(services, admin, "/v1/admin/world", {"command": "teleport", "character_id": aid, "zone": "old_town"})
        assert refused["ok"] is False and "zone_disabled" in refused["error"]
        admin_command(services, admin, "/v1/admin/settings", {"xp_rate": 100, "drop_rate": 100, "regen_rate": 100, "disabled_zones": []})
        moved = admin_command(services, admin, "/v1/admin/world", {"command": "teleport", "character_id": aid, "zone": "old_town", "x": 100, "y": 0})
        assert moved["ok"] and moved["zone"] == "old_town"
        state(first, lambda current: (current["zone"], current["x"], current["y"]) == ("old_town", 100, 0))
        bob_state = state(second)
        assert admin_command(services, admin, "/v1/admin/world", {"command": "summon", "character_id": aid, "to_character_id": bid})["ok"]
        state(first, lambda current: (current["zone"], current["x"], current["y"]) == (bob_state["zone"], bob_state["x"], bob_state["y"]))
        assert admin_command(services, admin, "/v1/admin/world", {"command": "teleport", "character_id": aid, "zone": "nowhere"})["ok"] is False
        assert isinstance(admin_command(services, admin, "/v1/admin/world", {"command": "respawn"})["respawned"], int)
        assert admin_command(services, admin, "/v1/admin/world", {"command": "respawn", "zone": "nowhere"})["ok"] is False
        validated = admin_command(services, admin, "/v1/admin/world", {"command": "validate_config"})
        assert validated["ok"] and validated["zones"] >= 2, validated
        for body in ({"action": "spawn", "count": 1, "behaviour": "town", "zone": "eminonu", "level": 3}, {"action": "despawn", "all": True}):
            assert "ok" in admin_command(services, admin, "/v1/admin/bots", body)  # bot semantics are covered by the bot suite
        assert admin_command(services, admin, "/v1/admin/control", {"command": "restart", "delay_seconds": 120, "message": "owned test"})["delay_seconds"] == 120
        event(first, "announce")
        services.until(lambda: (live_world()["countdown"] or {}).get("command") == "restart", "restart countdown")
        assert admin_command(services, admin, "/v1/admin/control", {"command": "cancel"})["cancelled"] is True
        assert services.world["process"].poll() is None, "cancelled restart stopped the world"
        assert admin_command(services, admin, "/v1/admin/world", {"command": "kick", "character_id": bid})["ok"]
        revoked_world_peer(second)
        assert admin_command(services, admin, "/v1/admin/world", {"command": "kick", "character_id": bid})["ok"] is False  # already gone
    services.until(lambda: services.sql(f"SELECT count(*) FROM characters WHERE id IN ({aid},{bid}) AND lease_instance IS NOT NULL") == "0", "admin web lease release")
    audit = services.sql("SELECT string_agg(DISTINCT action, ',') FROM audit_events WHERE action LIKE 'world.%'")
    assert {"world.announce", "world.kick", "world.teleport", "world.settings", "world.restart", "world.cancel"} <= set(audit.split(","))


def equipment_admission_invariant(services, users):
    configuration = services.independent_gameplay()
    merchant_definition = next(npc for npc in configuration["npcs"] if npc["kind"] == "merchant")
    for index in range(1, 9):
        item = f"invariant_slot_{index}"
        configuration["items"][item] = {"name": f"Owned equipment slot {index}", "stack": 1, "price": 1, "slot": f"slot{index}", "level": 1}
        merchant_definition["shop"].append(item)
    configuration["items"]["invariant_slot_replacement"] = {"name": "Owned replacement slot 1", "stack": 1, "price": 1, "slot": "slot1", "level": 1}
    merchant_definition["shop"].append("invariant_slot_replacement")
    gameplay = services.secret_file("equipment-gameplay.json", json.dumps(configuration))
    session = users["test_alice"]["session"]
    character = services.api("POST", "/v1/characters", {"name": "EquipmentInvariant", "class_id": 1, "appearance": {}}, session=session)
    services.stop(services.world)
    try:
        services.start_world(gameplay=gameplay)
        with WorldPeer(services, session, character["id"]) as peer:
            current = state(peer)
            blade = next(item["uid"] for item in current["inventory"] if item["item"] == "training_blade")
            peer.action("equip", uid=blade)
            state(peer, lambda current: current["equipment"].get("weapon") == blade)
            move(peer, merchant_definition["x"], merchant_definition["y"], 1)
            for index in range(1, 8):
                item = f"invariant_slot_{index}"
                peer.action("buy", target=merchant_definition["id"], item=item, quantity=1)
                purchased = state(peer, lambda current: quantities(current).get(item, 0) == 1)
                uid = next(entry["uid"] for entry in purchased["inventory"] if entry["item"] == item)
                peer.action("equip", uid=uid)
                equipped = state(peer, lambda current: current["equipment"].get(f"slot{index}") == uid)
                assert len(equipped["equipment"]) == index + 1
            peer.action("buy", target=merchant_definition["id"], item="invariant_slot_replacement", quantity=1)
            purchased = state(peer, lambda current: quantities(current).get("invariant_slot_replacement", 0) == 1)
            replacement = next(entry["uid"] for entry in purchased["inventory"] if entry["item"] == "invariant_slot_replacement")
            peer.action("equip", uid=replacement)
            replaced = state(peer, lambda current: current["equipment"].get("slot1") == replacement)
            assert len(replaced["equipment"]) == 8
            peer.action("buy", target=merchant_definition["id"], item="invariant_slot_8", quantity=1)
            purchased = state(peer, lambda current: quantities(current).get("invariant_slot_8", 0) == 1)
            ninth = next(entry["uid"] for entry in purchased["inventory"] if entry["item"] == "invariant_slot_8")
            before = saved(services, character["id"])["equipment"]
            peer.action("equip", uid=ninth)
            assert event(peer, "error")["code"] == "equipment"
            assert saved(services, character["id"])["equipment"] == before
            assert len(state(peer)["equipment"]) == 8
        services.until(lambda: services.sql(f"SELECT lease_instance IS NULL FROM characters WHERE id={character['id']}") == "t", "equipment character lease release")
        with WorldPeer(services, session, character["id"]) as reconnect:
            restored = state(reconnect)
            assert restored["equipment"] == before and len(restored["equipment"]) == 8
        services.until(lambda: services.sql(f"SELECT lease_instance IS NULL FROM characters WHERE id={character['id']}") == "t", "equipment reconnect lease release")
        services.api("DELETE", f"/v1/characters/{character['id']}", {"confirm_name": "EquipmentInvariant"}, session=session)
    finally:
        services.stop(services.world)
        services.start_world()


def backup_restore(services, users, characters):
    admin = users["test_admin"]["session"]
    services.until(lambda: services.sql("SELECT count(*) FROM characters WHERE lease_instance IS NOT NULL") == "0", "idle backup leases")
    backup = services.api("POST", "/v1/admin/backups", {}, session=admin, admin=True)
    listing = services.api("GET", "/v1/admin/backups", session=admin, admin=True)["backups"]
    assert any(entry["name"] == backup["name"] and entry["size"] == backup["size"] for entry in listing)
    name = backup["name"]
    assert Path(name).name == name and backup["size"] > 0
    path = services.root / "backups" / name
    assert path.is_file() and path.stat().st_size == backup["size"]
    assert path.stat().st_mode & 0o777 == 0o600
    status, payload, content_type = admin_raw(services, "GET", f"/v1/admin/backups/{name}", headers={"Authorization": "Bearer " + admin})
    assert status == 200 and payload == path.read_bytes() and content_type == "application/sql", "backup download differs from the server file"
    for target in (f"/v1/admin/backups/{name[:-4]}.txt", "/v1/admin/backups/ikv-0-00.sql", "/v1/admin/backups/..%2Fikv-0.sql"):
        expect_error(services, "GET", target, session=admin, status=404, admin=True)
    expect_error(services, "GET", f"/v1/admin/backups/{name}", session=users["test_alice"]["session"], status=403, admin=True)
    expected = services.sql("SELECT jsonb_agg(jsonb_build_object('id',id,'account_id',account_id,'name',name,'state',state) ORDER BY id)::text FROM characters")
    account_count = services.sql("SELECT count(*) FROM accounts")
    unsafe_restore = services.start(services.args.auth, "--dsn-file", services.dsn_file, "--restore-backup", path)
    services.until(lambda: unsafe_restore["process"].poll() is not None and unsafe_restore["eof"], "live-database restore refusal")
    assert unsafe_restore["process"].returncode > 0, "restore was permitted while owned services held live database locks"
    assert services.sql("SELECT count(*) FROM accounts") == account_count
    services.stop(services.world)
    services.stop(services.auth)
    services.sql("CREATE DATABASE ikv_restore")
    restore_dsn = services.dsn.replace("dbname=ikv_test", "dbname=ikv_restore")
    restore_file = services.secret_file("restore-dsn", restore_dsn + "\n")
    services.command([services.args.auth, "--dsn-file", restore_file, "--restore-backup", path], timeout=30)
    restored = services.sql("SELECT jsonb_agg(jsonb_build_object('id',id,'account_id',account_id,'name',name,'state',state) ORDER BY id)::text FROM characters", dsn=restore_dsn)
    assert json.loads(restored) == json.loads(expected), "backup did not restore exact authoritative character records"
    assert services.sql("SELECT count(*) FROM accounts", dsn=restore_dsn) == account_count
    services.dsn, services.dsn_file = restore_dsn, restore_file
    services.start_auth()
    session = services.api("POST", "/v1/login", {"username": "test_alice", "password": PASSWORD})["session"]
    services.start_world()
    with WorldPeer(services, session, characters[0]["id"]) as peer:
        current = state(peer)
        original = next(character["state"] for character in json.loads(expected) if character["id"] == characters[0]["id"])
        assert current["currency"] == original["currency"] and quantities(current) == quantities(original)
        assert current["quests"] == original["quests"]
        assert current["guild"] == original["guild"]
        assert all(current[field] == original[field] for field in ("zone", "x", "y", "level", "xp", "hp", "mana"))


def main():
    args = arguments(__doc__)
    with Services(args) as services:
        try:
            parser_and_tls(services)
            users = authentication(services)
            auth_security_regressions(services, users)
            owned_characters = characters(services, users)
            world(services, users, owned_characters)
            admin_web(services, users, owned_characters)
            equipment_admission_invariant(services, users)
            world_admission_invariants(services, users["test_admin"]["session"])
            backup_restore(services, users, owned_characters)
        except BaseException as error:
            print(services.redact("".join(traceback.format_exception(
                type(error), error, error.__traceback__, limit=-8, chain=False)))[:8192], file=sys.stderr)
            print(services.diagnostics(), file=sys.stderr)
            raise AssertionError("owned integration suite failed; sanitized diagnostics above") from None
    print("authenticated TLS, persistence, authority, economy and backup/restore checks passed")


if __name__ == "__main__":
    main()
