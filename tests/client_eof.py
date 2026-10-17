#!/usr/bin/env python3
"""Exercise authenticated native TLS framing against disposable real services."""

from collections import deque
from contextlib import AbstractContextManager, ExitStack
import json
import os
from pathlib import Path
import select
import re
import signal
import shutil
import socket
import ssl
import struct
import threading
import time
import traceback

from isolated_services import Services, WorldPeer, arguments, frame


class WorldProxy(AbstractContextManager):
    """Forward real records; mutate one record or omit the bootstrap state."""

    def __init__(self, services, fault=None, *, cert=None, reject_tls=False, forward=False, fault_kind=None, omit_initial_state=False):
        self.services, self.fault = services, fault
        self.reject_tls, self.forward = reject_tls, forward
        self.fault_kind = fault_kind
        self.omit_initial_state = omit_initial_state
        self.world_clean_eof = False
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.load_cert_chain(str(cert or services.cert), str(services.key))
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.sockets = [self.listener]
        self.stop = threading.Event()
        self.established = threading.Event()
        self.release = threading.Event()
        self.records = []
        self.errors = []
        self.handshaken = self.rejected = self.closed_cleanly = False
        self.uploaded = 0
        self.target = self.injected = None
        self.thread = threading.Thread(target=self.serve, name="native-world-tls-proxy")

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        try:
            deadline = time.monotonic() + (120 if self.forward else 15)
            while not self.stop.is_set():
                assert time.monotonic() < deadline, "proxy accept deadline"
                try:
                    raw, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            self.sockets.append(raw)
            raw.settimeout(5)
            client = self.context.wrap_socket(raw, server_side=True, do_handshake_on_connect=False)
            self.sockets.append(client)
            try:
                client.do_handshake()
            except (ssl.SSLError, ConnectionResetError):
                if not self.reject_tls:
                    raise
                self.rejected = True
                return
            self.handshaken = True
            assert not self.reject_tls, "native client trusted a rejected certificate"
            client.settimeout(1)
            world = self.services.connect(self.services.world_port, strict_tls=self.forward)
            self.sockets.append(world)
            world.settimeout(1)
            pending = bytearray()
            kinds = set()
            read_retry = None
            while not self.stop.is_set():
                assert time.monotonic() < deadline, "proxy stream deadline"
                if read_retry is None and not self.forward and self.established.is_set() and self.release.is_set():
                    if self.fault is None:
                        self.orderly_eof(client)
                        return
                    if len(pending) >= 4:
                        size = struct.unpack("!I", pending[:4])[0]
                        assert 2 <= size <= 65535, "invalid actual world frame length"
                        if len(pending) >= 4 + size:
                            self.target = bytes(pending[:4 + size])
                            assert self.target[4] == 2, "unexpected actual world wire version"
                            if self.fault_kind is not None and self.target[5] != self.fault_kind:
                                self.records.append(self.target)
                                client.sendall(self.target)
                                del pending[:4 + size]
                                continue
                            self.injected = self.fault(self.target)
                            assert self.injected != self.target, "fault did not alter actual record"
                            client.sendall(self.injected)
                            self.orderly_eof(client)
                            return
                while read_retry is None and (self.forward or not self.established.is_set()) and len(pending) >= 4:
                    size = struct.unpack("!I", pending[:4])[0]
                    assert 2 <= size <= 65535, "invalid actual world frame length"
                    if len(pending) < 4 + size:
                        break
                    record = bytes(pending[:4 + size])
                    del pending[:4 + size]
                    assert record[4] == 2, "unexpected actual world wire version"
                    assert record[5] in (2, 4, 5, 6), "unexpected actual world record type"
                    if not self.records:
                        assert record[5] == 2, "world did not welcome authenticated native client first"
                    if self.omit_initial_state and record[5] == 6:
                        continue
                    if self.omit_initial_state and record[5] == 4:
                        self.target = record
                        client.sendall(record)
                        self.orderly_eof(client)
                        return
                    self.records.append(record)
                    assert len(self.records) <= (5000 if self.forward else 64), "forwarded records exceeded bound"
                    client.sendall(record)
                    kinds.add(record[5])
                    if kinds >= {2, 4, 6}:
                        # The main thread observes parsed native JSON before releasing
                        # faults: coalesced invalid bytes must not hide good callbacks.
                        self.established.set()
                readers = [client]
                if self.forward or not self.established.is_set() or self.release.is_set():
                    readers.append(world)
                writers = [read_retry] if read_retry is not None else []
                if writers:
                    readers = []
                ready, writable, _ = select.select(readers, writers, [], 0.05)
                for connection in readers + writers:
                    if connection not in ready and connection not in writable and not connection.pending():
                        continue
                    # TCP readiness can contain only TLS control records (e.g.
                    # session tickets). Never block the thread that must relay
                    # the hello/application data needed to finish this read.
                    timeout = connection.gettimeout()
                    connection.setblocking(False)
                    try:
                        chunk = connection.recv(65536)
                    except ssl.SSLWantReadError:
                        read_retry = None
                        continue
                    except ssl.SSLWantWriteError:
                        read_retry = connection
                        break
                    finally:
                        connection.settimeout(timeout)
                    read_retry = None
                    if not chunk and connection is world and self.forward:
                        assert not pending, "actual world truncated its final frame"
                        self.world_clean_eof = True
                        world.unwrap().close()  # Acknowledge the real world's close_notify before relaying EOF.
                        self.orderly_eof(client)
                        return
                    assert chunk, "real peer closed before fault injection"
                    if connection is client:
                        self.uploaded += len(chunk)
                        world.sendall(chunk)
                    else:
                        pending.extend(chunk)
                        assert len(pending) <= 2 * 65539, "proxy frame buffer exceeded bound"
        except BaseException as error:
            if not self.stop.is_set():
                self.errors.append(self.services.redact(str(error)))
        finally:
            for connection in reversed(self.sockets):
                connection.close()

    def orderly_eof(self, connection):
        # unwrap sends close_notify, not an unauthenticated raw TCP FIN. Some
        # rejection paths close after receiving it without replying; that is OK.
        connection.settimeout(2)
        try:
            raw = connection.unwrap()
            self.sockets.append(raw)
            raw.close()
        except (ssl.SSLError, OSError):
            pass
        self.closed_cleanly = True

    def __exit__(self, *error):
        self.stop.set()
        for connection in reversed(self.sockets):
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        self.thread.join(timeout=6)
        assert not self.thread.is_alive(), "TLS proxy thread failed to stop"


def identity(services, index):
    credentials = {"username": f"native_eof_{index}", "password": "Native-EOF-test-917!"}
    services.secrets.add(credentials["password"])
    source_ip = f"127.0.0.{2 + index // 8}"
    account = services.api("POST", "/v1/register", credentials, source_ip=source_ip)
    assert isinstance(account["account_id"], int) and account["account_id"] > 0
    session = services.api("POST", "/v1/login", credentials, source_ip=source_ip)["session"]
    assert len(session) == 64 and all(c in "0123456789abcdef" for c in session)
    services.secrets.add(session)
    character = services.api("POST", "/v1/characters", {
        "name": f"EOF Test {index}", "class_id": 1, "appearance": {},
    }, session=session)
    assert character["id"] > 0 and character["account_id"] == account["account_id"]
    return session, character


def native(services, session, character, *, port=None, ca=None, host="127.0.0.1", ticks=None):
    argv = ["--headless", "--host", host, "--port", str(port or services.world_port),
            "--ca", str(ca or services.ca), "--session-stdin", "--character", str(character["id"])]
    if ticks is not None:
        argv.extend(("--ticks", str(ticks)))
    child = services.start(services.args.client, *argv, stdin=session + "\n")
    child["records"] = deque(maxlen=5000)
    return child


def finished(services, child, name):
    services.until(lambda: child["process"].poll() is not None and child["eof"], name + " native exit", timeout=18)
    return child["process"].returncode


def accepted(child):
    snapshots = [record for record in child["records"] if "players" in record]
    states = [record["state"] for record in child["records"] if record.get("type") == "state"]
    return snapshots, states


def assert_state_descriptor(state, character):
    current = state["character"]["state"]
    assert state["zone"] == current["zone"]
    assert state["source_registry_id"] is None, "independent fixture acquired a source association"
    assert state["bounds"] == [-10000, -10000, 10000, 10000]
    assert type(state["xp_interval"]) is int and 1 <= state["xp_interval"] <= 10000000
    assert state["character"]["appearance"] == character["appearance"]
    assert isinstance(state["character"]["equipment"], list)
    visible = [current] + state["entities"]
    assert all(-10000 <= entity["x"] <= 10000 and -10000 <= entity["y"] <= 10000
               for entity in visible)
    for entity in state["entities"]:
        assert isinstance(entity["appearance"], dict) and isinstance(entity["equipment"], list)
        assert isinstance(entity["alive"], bool)
        if entity["kind"] == "player":
            assert entity["class_id"] in (1, 2, 3)
        else:
            assert entity["kind"] in ("enemy", "merchant", "questgiver", "node", "anvil", "door", "summon")
            assert 1 <= entity["source_class"] <= 65535 and 0 < entity["source_uid"] <= 0xffffffff
    for equipment in ([state["character"]["equipment"]] + [entity["equipment"] for entity in state["entities"]] +
                      [event["equipment"] for event in state["events"] if event["type"] in ("equip", "unequip")]):
        assert isinstance(equipment, list)
        assert len(equipment) <= 16
        assert len({item["slot"] for item in equipment}) == len(equipment)
        assert all(0 <= item["slot"] <= 15 and 0 < item["source_uid"] <= 0xffffffff for item in equipment)
    for event in state["events"]:
        if event["type"] == "skill":
            assert isinstance(event["skill"], str) and 1 <= len(event["skill"].encode()) <= 32
        for field in ("entity", "source", "target"):
            if event["type"] in ("combat", "skill", "death", "resurrect", "equip", "unequip"):
                assert field in event
            if field in event:
                assert 0 < event[field] <= 0xffffffff


def assert_real_prefix(proxy, child, character):
    welcome = proxy.records[0]
    assert len(welcome) == 14
    self_id, welcome_tick = struct.unpack("!II", welcome[6:])
    assert self_id > 0
    expected_snapshots, expected_states = [], []
    last_tick = welcome_tick
    for record in proxy.records[1:]:
        if record[5] == 4:
            tick, count = struct.unpack("!IH", record[6:12])
            assert tick > last_tick and 1 <= count <= 32 and len(record) == 12 + count * 12
            players = [dict(zip(("id", "x", "y"), struct.unpack("!Iii", record[12 + i*12:24 + i*12]))) for i in range(count)]
            assert any(player["id"] == self_id for player in players)
            assert expected_states and expected_states[-1]["tick"] == tick, "snapshot did not follow its current state"
            state = expected_states[-1]
            expected_positions = {self_id: (state["character"]["state"]["x"], state["character"]["state"]["y"])}
            expected_positions.update({entity["id"]: (entity["x"], entity["y"])
                                       for entity in state["entities"] if entity["kind"] == "player"})
            assert {player["id"]: (player["x"], player["y"]) for player in players} == expected_positions
            expected_snapshots.append({"self": self_id, "tick": tick, "players": players})
            last_tick = tick
        elif record[5] == 6:
            state = json.loads(record[6:])
            if "catalogs" not in state:  # the world omits unchanged catalogs; native keeps the last accepted copy
                assert expected_states, "admission state lacks catalogs"
                state["catalogs"] = expected_states[-1]["catalogs"]
            assert state["self"] == self_id and state["tick"] >= last_tick
            assert state["character"]["name"] == character["name"]
            assert state["character"]["class_id"] == character["class_id"]
            resources = state["character"]["state"]
            assert 0 <= resources["hp"] <= resources["max_hp"] and resources["max_hp"] > 0
            assert 0 <= resources["mana"] <= resources["max_mana"]
            assert isinstance(state["entities"], list) and isinstance(state["events"], list)
            assert_state_descriptor(state, character)
            expected_states.append(state)
        elif record[5] == 5 and record is proxy.records[-1]:  # the world's final goodbye before close_notify (kick/refusal)
            assert json.loads(record[6:])["reason"], "world goodbye without reason"
        else:
            raise AssertionError("unexpected record after real welcome")
    snapshots, states = accepted(child)
    assert snapshots == expected_snapshots and states == expected_states, "native JSON differs from actual forwarded world records"
    assert snapshots and states, "native did not parse real snapshot and authoritative state"


def framing_case(services, index, name, fault=None, *, cert=None, host="127.0.0.1", fault_kind=None, peer=False):
    session, character = identity(services, index)
    characters = [character]
    with ExitStack() as stack:
        if peer:
            other = services.api("POST", "/v1/characters", {
                "name": f"EOF Peer {index}", "class_id": 2, "appearance": {},
            }, session=session)
            characters.append(other)
            observer = stack.enter_context(ObservedPeer(services, session, other))
            observer.until(lambda kind, body: kind == 6)
        proxy = stack.enter_context(WorldProxy(services, fault, cert=cert, fault_kind=fault_kind))
        child = native(services, session, character, port=proxy.port, host=host)
        try:
            services.until(lambda: (proxy.established.is_set() and all(accepted(child))) or
                           child["process"].poll() is not None or proxy.errors,
                           name + " authenticated native records", timeout=12)
            assert not proxy.errors, (name, proxy.errors)
            assert proxy.established.is_set() and all(accepted(child)), (name, "no authenticated bootstrap")
            assert_real_prefix(proxy, child, character)
            if peer:
                snapshot = accepted(child)[0][-1]
                assert snapshot["players"][-1]["id"] == snapshot["self"], "peer mutation did not select the native self"
            proxy.release.set()
            result = finished(services, child, name)
            assert result == (0 if fault is None else 1), (name, "unexpected native exit", result)
            services.until(lambda: proxy.closed_cleanly or proxy.errors, name + " TLS close_notify", timeout=4)
            assert proxy.closed_cleanly and not proxy.errors, (name, proxy.errors)
            if fault is not None:
                assert proxy.target is not None and proxy.injected == fault(proxy.target)
                assert_real_prefix(proxy, child, character)
        finally:
            services.stop(child)
    released(services, characters)
    print(name + ": passed")


_MISSING = object()


def state_fault(path, value):
    def mutate(record):
        assert record[5] == 6
        state = json.loads(record[6:])
        if not path:
            state = value(state)
        else:
            parent = state
            for field in path[:-1]:
                parent = parent[field]
            if value is _MISSING:
                del parent[path[-1]]
            else:
                parent[path[-1]] = value(state) if callable(value) else value
        return frame(6, json.dumps(state, ensure_ascii=False, separators=(",", ":")).encode())
    return mutate


def snapshot_fault(mode):
    def mutate(record):
        assert record[5] == 4
        tick, count = struct.unpack("!IH", record[6:12])
        players = [list(struct.unpack("!Iii", record[12 + i*12:24 + i*12])) for i in range(count)]
        if mode == "out-of-zone":
            players[-1][1] = 10001
        elif mode == "self mismatch":
            players[-1][1] += 1
        else:
            assert count == 2, "snapshot peer mutation requires the real second player"
            if mode == "peer mismatch":
                players[0][1] += 1
            else:
                assert mode == "missing peer"
                del players[0]
        return frame(4, struct.pack("!IH", tick, len(players)) +
                     b"".join(struct.pack("!Iii", *player) for player in players))
    return mutate


def session_contract_cases(services, index):
    equipment = [{"slot": 0, "source_uid": 1}]
    cases = [
        ("missing zone", ("zone",), _MISSING),
        ("zone descriptor mismatch", ("zone",), "old_town"),
        ("non-text zone", ("zone",), 1),
        ("missing source id", ("source_registry_id",), _MISSING),
        ("negative source id", ("source_registry_id",), -1),
        ("oversized source id", ("source_registry_id",), 71),
        ("boolean source id", ("source_registry_id",), True),
        ("source-associated wrong bounds", ("source_registry_id",), 0),
        ("changed same-zone source id", (), lambda state: {
            **state, "source_registry_id": 0, "bounds": [-1536000, -1536000, 1534500, 1534500]}),
        ("missing bounds", ("bounds",), _MISSING),
        ("reversed bounds", ("bounds",), [10000, -10000, -10000, 10000]),
        ("oversized bounds", ("bounds",), [-1536001, -10000, 10000, 10000]),
        ("changed same-zone bounds", ("bounds",), [-9999, -10000, 10000, 10000]),
        ("zero XP interval", ("xp_interval",), 0),
        ("overflow XP interval", ("xp_interval",), 10000001),
        ("noninteger XP interval", ("xp_interval",), 1.0),
        ("out-of-zone self", ("character", "state", "x"), 10001),
        ("out-of-zone visible entity", ("entities", 0, "y"), -10001),
        ("cosmetic sex", ("character", "appearance", "sex"), 2),
        ("cosmetic face", ("character", "appearance", "face"), 10),
        ("default male cosmetic hair", ("character", "appearance", "hair"), 18),
        ("default male cosmetic hair color", ("character", "appearance", "hair_color"), 5),
        ("default male cosmetic beard", ("character", "appearance", "beard"), 15),
        ("default male cosmetic beard color", ("character", "appearance", "beard_color"), 5),
        ("cosmetic float", ("character", "appearance", "face"), 1.0),
        ("cosmetic byte cap", ("character", "appearance"), {"unknown": "x" * 385}),
        ("invalid equipment slot", ("character", "equipment"), [{"slot": 16, "source_uid": 1}]),
        ("zero equipment source uid", ("character", "equipment"), [{"slot": 0, "source_uid": 0}]),
        ("overflow equipment source uid", ("character", "equipment"), [{"slot": 0, "source_uid": 0x100000000}]),
        ("duplicate equipment slots", ("character", "equipment"), equipment * 2),
        ("oversized equipment", ("character", "equipment"), [{"slot": i % 16, "source_uid": i + 1} for i in range(17)]),
        ("invalid NPC source class", ("entities", 0, "source_class"), 65536),
        ("zero NPC source uid", ("entities", 0, "source_uid"), 0),
        ("invalid visible cosmetics", ("entities", 0, "appearance"), {"sex": -1}),
        ("invalid visible equipment", ("entities", 0, "equipment"), [{"slot": -1, "source_uid": 1}]),
        ("nonboolean visible alive", ("entities", 0, "alive"), 1),
        ("out-of-zone correction", ("events",), [{"type": "correction", "x": 10001, "y": 0}]),
        ("non-object catalogs", ("catalogs",), []),
    ]
    for event_type, field, value in (
        ("combat", "source", 0), ("skill", "target", 0x100000000),
        ("death", "entity", -1), ("resurrect", "target", True),
        ("equip", "entity", 0), ("unequip", "source", 0),
        ("chat", "source", 0x100000000),
        ("combat", "entity", _MISSING), ("skill", "source", _MISSING), ("death", "target", _MISSING),
        ("skill", "skill", _MISSING), ("skill", "skill", ""),
        ("equip", "equipment", _MISSING),
        ("unequip", "equipment", [{"slot": 16, "source_uid": 1}]),
    ):
        cases.append((f"{event_type} invalid {field}", ("events",),
                      lambda state, kind=event_type, key=field, bad=value: [
                          {name: value for name, value in {
                              "type": kind, "entity": state["self"], "source": state["self"],
                              "target": state["self"],
                              **({"skill": "strike"} if kind == "skill" else
                                 {"equipment": []} if kind in ("equip", "unequip") else {}),
                              key: bad}.items() if value is not _MISSING}]))
    for name, path, value in cases:
        framing_case(services, index, name, state_fault(path, value), fault_kind=6)
        index += 1
    framing_case(services, index, "invalid player class",
                 state_fault(("entities",), lambda state: [
                     {**entity, "class_id": 4} if entity["kind"] == "player" else entity
                     for entity in state["entities"]]), fault_kind=6, peer=True)
    index += 1
    for mode in ("out-of-zone", "self mismatch", "peer mismatch", "missing peer"):
        framing_case(services, index, "snapshot " + mode, snapshot_fault(mode),
                     fault_kind=4, peer=mode in ("peer mismatch", "missing peer"))
        index += 1
    return index


def snapshot_requires_state(services, index):
    session, character = identity(services, index)
    with WorldProxy(services, omit_initial_state=True) as proxy:
        child = native(services, session, character, port=proxy.port)
        try:
            assert finished(services, child, "snapshot before state") == 1
            services.until(lambda: proxy.closed_cleanly or proxy.errors, "snapshot before state TLS EOF", timeout=4)
            assert proxy.closed_cleanly and not proxy.errors
            assert proxy.target is not None and proxy.target[5] == 4
            assert not any(accepted(child)), "native exposed snapshot before authoritative zone state"
        finally:
            services.stop(child)
    released(services, [character])
    print("native snapshot requires authoritative state first: passed")


def same_tick_state_causality(services, index):
    session, character = identity(services, index)
    injected_states = []
    injected_snapshot = None

    def updates(record):
        nonlocal injected_snapshot
        assert record[5] == 6
        previous = next(item for item in reversed(proxy.records) if item[5] == 4)
        tick, count = struct.unpack("!IH", previous[6:12])
        assert count == 1, "causality fixture unexpectedly has another player"
        self_id, x, y = struct.unpack("!Iii", previous[12:])
        assert self_id == struct.unpack("!I", proxy.records[0][6:10])[0]
        assert x + 1 <= 10000
        changed = json.loads(record[6:])
        changed["tick"] = tick
        changed["character"]["state"].update(x=x + 1, y=y)
        transferred = json.loads(json.dumps(changed))
        transferred.update(zone="old_town", source_registry_id=1,
                           bounds=[-1536000, -1536000, 1534500, 1534500], entities=[], events=[])
        transferred["character"]["state"].update(zone="old_town", x=-109500, y=0)
        future = json.loads(json.dumps(transferred))
        future["tick"] = tick + 4
        injected_states[:] = [changed, transferred, future]
        injected_snapshot = {"self": self_id, "tick": tick + 4,
                             "players": [{"id": self_id, "x": -109500, "y": 0}]}
        return b"".join(frame(6, json.dumps(state, ensure_ascii=False, separators=(",", ":")).encode())
                        for state in injected_states) + frame(
                            4, struct.pack("!IHIii", tick + 4, 1, self_id, -109500, 0))

    with WorldProxy(services, updates, fault_kind=6) as proxy:
        child = native(services, session, character, port=proxy.port)
        try:
            services.until(lambda: (proxy.established.is_set() and all(accepted(child))) or
                           child["process"].poll() is not None or proxy.errors,
                           "same-tick causality bootstrap", timeout=12)
            assert proxy.established.is_set() and not proxy.errors
            assert_real_prefix(proxy, child, character)
            prefix_snapshots, prefix_states = accepted(child)
            proxy.release.set()
            assert finished(services, child, "same-tick authoritative updates") == 0
            services.until(lambda: proxy.closed_cleanly or proxy.errors, "same-tick update TLS EOF", timeout=4)
            assert proxy.closed_cleanly and not proxy.errors and proxy.injected is not None
            snapshots, states = accepted(child)
            carried = [state if "catalogs" in state else {**state, "catalogs": prefix_states[-1]["catalogs"]}
                       for state in injected_states]  # native keeps catalogs that unchanged states omit
            assert states == prefix_states + carried, "native lost a legitimate same-tick state update"
            assert snapshots == prefix_snapshots + [injected_snapshot]
        finally:
            services.stop(child)
    released(services, [character])
    print("native same-tick movement and source-zone transfer preserve forward causality: passed")


def source_requires_assets(services):
    gameplay = Path(services.args.source) / "config/gameplay.json"
    config = json.loads(gameplay.read_text())
    source_ids = [zone.pop("source_registry_id") for zone in config["zones"].values()
                  if "source_registry_id" in zone]
    assert source_ids and all(type(registry) is int and 0 <= registry <= 70 for registry in source_ids)
    for zone in config["zones"].values():
        zone.pop("instances", None)  # instance copies are original registry rows too
    # Only remove the source associations: the real TLS, store and all other
    # production gameplay fields must successfully reach listening readiness.
    independent = services.root / "source-root-control.json"
    independent.write_text(json.dumps(config))
    services.start_world(gameplay=independent)
    port = services.world_port
    services.stop(services.world)
    assert services.world["process"].returncode == 0, "source-root control did not stop cleanly"
    child = services.start(services.args.server, "--bind", "127.0.0.1", "--port", port,
                           "--cert", services.cert, "--key", services.key,
                           "--dsn-file", services.dsn_file, "--gameplay", gameplay)

    def refused():
        assert not any(line.startswith("READY ") for line in child["lines"]), (
            "source-associated world became ready without original assets")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.05)
            try:
                probe.connect(("127.0.0.1", port))
            except ConnectionRefusedError:
                pass
            else:
                raise AssertionError("source-associated world accepted TCP without original assets")
        return child["process"].poll() is not None and child["eof"]

    try:
        services.until(refused, "source-associated config rejecting absent assets")
        assert child["process"].returncode != 0, "source-associated world accepted absent original assets"
    finally:
        services.stop(child)
    print("source-associated server config refuses absent asset support: passed")


def appearance_api_cases(services, index):
    session, character = identity(services, index)
    assert character["appearance"] == {}
    female = {"sex": 0, "face": 9, "hair": 20, "hair_color": 9, "beard": 24, "beard_color": 1,
              "unknown_cosmetic": "preserved"}
    male = {"sex": 1, "face": 9, "hair": 17, "hair_color": 4, "beard": 14, "beard_color": 4}
    for appearance in ({"sex": 2}, {"sex": -1}, {"face": 10}, {"face": 1.0}, {"sex": 1.0},
                       {**male, "hair": 18}, {**male, "hair_color": 5},
                       {**male, "beard": 15}, {**male, "beard_color": 5}):
        result = services.api("POST", "/v1/characters", {
            "name": f"Rejected Cosmetic {index}", "class_id": 1, "appearance": appearance,
        }, session=session, status=400)
        assert result["error"]["code"] == "invalid_request"
    result = services.api("POST", "/v1/characters", {
        "name": f"Rejected State {index}", "class_id": 1, "appearance": {}, "state": {"hp": 100},
    }, session=session, status=400)
    assert result["error"]["code"] == "invalid_request"
    for sex, appearance in enumerate((female, male)):
        created = services.api("POST", "/v1/characters", {
            "name": f"Cosmetic Limit {index} {sex}", "class_id": 1, "appearance": appearance,
        }, session=session)
        assert created["appearance"] == appearance
    print("native service cosmetic API boundaries: passed")


def rejected_certificate(services, index, name, *, ca=None, cert=None):
    session, character = identity(services, index)
    with WorldProxy(services, cert=cert, reject_tls=True) as proxy:
        child = native(services, session, character, port=proxy.port, ca=ca)
        try:
            assert finished(services, child, name) == 1, name + ": native accepted untrusted TLS"
            services.until(lambda: proxy.rejected or proxy.errors, name + " server TLS rejection", timeout=4)
            assert proxy.rejected and not proxy.handshaken and not proxy.errors, (name, proxy.errors)
            assert proxy.uploaded == 0 and not child["records"], name + ": unauthenticated records escaped TLS validation"
        finally:
            services.stop(child)
    released(services, [character])
    print(name + ": passed")


def certificates(services):
    openssl = services.binary("openssl")
    wrong_ca = services.root / "unrelated-ca.pem"
    services.command([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
                      services.root / "unrelated-ca.key", "-out", wrong_ca, "-days", "1", "-subj",
                      "/CN=Unrelated native EOF CA", "-addext", "basicConstraints=critical,CA:TRUE",
                      "-addext", "keyUsage=critical,keyCertSign,cRLSign"])
    dns_cert = services.root / "localhost-only.pem"
    extensions = services.secret_file("localhost-only.ext", "subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    services.command([openssl, "x509", "-req", "-in", services.root / "server.csr", "-CA", services.ca,
                      "-CAkey", services.root / "ca.key", "-CAcreateserial", "-out", dns_cert,
                      "-days", "1", "-extfile", extensions])
    return wrong_ca, dns_cert


class ObservedPeer(WorldPeer):
    """Decode the owned world's frames and reject unauthenticated/truncated EOF."""

    def __init__(self, services, session, character, *, receive_buffer=None):
        self.character = character
        self.records = []
        self.frame_sizes = []
        self.clean_eof = False
        super().__init__(services, session, character["id"],
                         receive_buffer=receive_buffer, strict_tls=True)

    def fill(self):
        chunk = self.connection.recv(65536)
        if not chunk:
            assert not self.buffer, "close_notify truncated an authoritative frame"
            self.clean_eof = True
            raise EOFError("authenticated world close_notify")
        self.buffer.extend(chunk)

    def receive(self):
        while len(self.buffer) < 4:
            self.fill()
        length = struct.unpack("!I", self.buffer[:4])[0]
        kind, body = super().receive()
        self.frame_sizes.append(length + 4)
        self.records.append((kind, body))
        assert len(self.records) <= 5000, "observed world records exceeded bound"
        if kind == 2:
            assert self.id > 0
        elif kind == 6:
            assert body["self"] == self.id and body["tick"] >= self.last_tick
            assert body["character"]["name"] == self.character["name"]
            assert body["character"]["class_id"] == self.character["class_id"]
            current = body["character"]["state"]
            assert 0 <= current["hp"] <= current["max_hp"] and current["max_hp"] > 0
            assert 0 <= current["mana"] <= current["max_mana"]
            assert isinstance(body["entities"], list) and isinstance(body["events"], list)
            assert_state_descriptor(body, self.character)
        elif kind == 5:  # server goodbye before a close (admin kick, refusal) carries a Turkish reason
            assert isinstance(body.get("reason"), str) and body["reason"], "goodbye without reason"
        else:
            assert kind == 4, "unexpected authoritative world record"
        return kind, body

    def __exit__(self, *error):
        if self.clean_eof:
            self.connection.close()
        else:
            super().__exit__(*error)

    def available(self):
        while self.buffer or self.connection.pending() or select.select([self.connection], [], [], 0)[0]:
            self.receive()

    def closed(self, deadline):
        while time.monotonic() < deadline:
            self.services.pump(0)
            self.connection.settimeout(min(0.1, max(0.001, deadline - time.monotonic())))
            try:
                self.receive()
            except socket.timeout:
                continue
            except EOFError:
                assert self.clean_eof and not self.buffer
                self.connection.settimeout(max(0.001, deadline - time.monotonic()))
                self.connection.unwrap().close()  # Reply with close_notify, never a post-close goodbye frame.
                return
        raise AssertionError("graceful world close exceeded its bounded deadline")


def released(services, characters):
    ids = ",".join(str(character["id"]) for character in characters)
    services.until(lambda: services.sql(
        f"SELECT count(*) FROM characters WHERE id IN ({ids}) AND lease_instance IS NOT NULL"
    ) == "0", "released transport-test character leases", timeout=5)


def abrupt_isolation(services, identities):
    (first_session, first_character), (second_session, second_character) = identities
    with ExitStack() as stack:
        first = stack.enter_context(ObservedPeer(services, first_session, first_character))
        second = stack.enter_context(ObservedPeer(services, second_session, second_character))
        first.until(lambda kind, body: kind == 6)
        second.until(lambda kind, body: kind == 6)
        before = second.until(lambda kind, body: kind == 4)["tick"]
        process = services.world["process"]
        paused = False
        try:
            os.kill(process.pid, signal.SIGSTOP)
            paused = True
            # Drain while the owned producer is stopped, then close only TCP:
            # no TLS shutdown and no unread bytes that could turn FIN into RST.
            first.connection.settimeout(0.05)
            try:
                while True:
                    first.receive()
            except socket.timeout:
                pass
            os.close(first.connection.detach())
            second.connection.sendall(frame(7, struct.pack("!Iii", 1, 80, 0)))
        finally:
            if paused:
                os.kill(process.pid, signal.SIGCONT)
        second.until(lambda kind, body: kind == 6 and
                     (body["character"]["state"]["x"], body["character"]["state"]["y"]) == (80, 0))
        snapshot = second.until(lambda kind, body: kind == 4 and body["tick"] > before and
                                first.id not in {player["id"] for player in body["players"]})
        assert {player["id"] for player in snapshot["players"]} == {second.id}
        assert services.world["process"].poll() is None, "abrupt peer killed the owned world"
        second.action("chat", channel="local", text="Healthy after unexpected TLS EOF")
        second.until(lambda kind, body: kind == 6 and any(
            event.get("type") == "chat" and event.get("text") == "Healthy after unexpected TLS EOF"
            for event in body["events"]))
    released(services, [first_character, second_character])
    print("abrupt authenticated TLS EOF cross-peer isolation: passed")


def send_queue(services, peer):
    """Prove owned Send-Q via proc and the actual receive window via TCP_INFO."""
    sample = None
    try:
        process = services.world["process"]
        assert process.poll() is None, "owned world exited during backpressure"
        inodes = set()
        for fd in (Path("/proc") / str(process.pid) / "fd").iterdir():
            try:
                target = os.readlink(fd)
            except FileNotFoundError:
                continue
            if target.startswith("socket:[") and target.endswith("]"):
                inodes.add(int(target[8:-1]))

        local = peer.connection.getpeername()
        remote = peer.connection.getsockname()
        local_endpoint = f"{struct.unpack('=I', socket.inet_aton(local[0]))[0]:08X}:{local[1]:04X}"
        remote_endpoint = f"{struct.unpack('=I', socket.inet_aton(remote[0]))[0]:08X}:{remote[1]:04X}"
        with (Path("/proc") / str(process.pid) / "net" / "tcp").open() as connections:
            assert connections.readline().split()[:4] == [
                "sl", "local_address", "rem_address", "st"], "unexpected proc TCP header"
            for line in connections:
                fields = line.split()
                assert len(fields) >= 10, "short proc TCP row"
                if (fields[1:4] == [local_endpoint, remote_endpoint, "01"] and
                        int(fields[9]) in inodes):
                    assert sample is None, "duplicate owned proc TCP connection"
                    sample = {"state": fields[3],
                              "send_queue": int(fields[4].split(":")[0], 16),
                              "timer": int(fields[5].split(":")[0], 16),
                              "snd_wnd": None, "inode": int(fields[9])}
        assert sample is not None, "proc TCP lacks the owned established world connection"
        ports = struct.pack("!HH", local[1], remote[1])
        source = socket.inet_aton(local[0]) + bytes(12)
        destination = socket.inet_aton(remote[0]) + bytes(12)
        header = struct.Struct("=IHHII")
        sock_id = struct.Struct("=4s16s16sIII")
        u32 = struct.Struct("=I")
        # Linux UAPI linux/inet_diag.h: exact SOCK_DIAG_BY_FAMILY, not a dump;
        # extension INFO (2) only, and TCP_ESTABLISHED (1).
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 4) as diagnostic:
            diagnostic.settimeout(0.2)
            diagnostic.bind((0, 0))
            port_id = diagnostic.getsockname()[0]
            request = (header.pack(72, 20, 1, 1, port_id) +
                       struct.pack("=BBBBI", socket.AF_INET, socket.IPPROTO_TCP, 0x02, 0, 1 << 1) +
                       sock_id.pack(ports, source, destination, 0, 0xffffffff, 0xffffffff))
            assert diagnostic.sendto(request, (0, 0)) == len(request), "short SOCK_DIAG request"
            reply, _, flags, sender = diagnostic.recvmsg(65536)
        assert sender == (0, 0), "SOCK_DIAG reply is not from the kernel"
        assert not flags & socket.MSG_TRUNC, "truncated SOCK_DIAG reply"
        offset = 0
        got_response = False
        while offset < len(reply):
            assert len(reply) - offset >= header.size, "short SOCK_DIAG message header"
            size, kind, flags, sequence, reply_port = header.unpack_from(reply, offset)
            assert header.size <= size <= len(reply) - offset, "invalid SOCK_DIAG message length"
            aligned_size = (size + 3) & ~3
            assert aligned_size <= len(reply) - offset, "short SOCK_DIAG message alignment"
            assert sequence == 1 and reply_port == port_id, "SOCK_DIAG reply identity mismatch"
            assert not flags & (2 | 16), "multipart or interrupted SOCK_DIAG reply"
            payload = reply[offset + header.size:offset + size]
            offset += aligned_size
            if kind == 1:  # NLMSG_NOOP
                continue
            if kind == 2:  # NLMSG_ERROR includes errno and the original header.
                assert len(payload) >= 20, "short SOCK_DIAG error"
                error = struct.unpack_from("=i", payload)[0]
                assert error < 0, "unexpected SOCK_DIAG acknowledgement"
                raise OSError(-error, os.strerror(-error))
            assert kind == 20, f"unexpected SOCK_DIAG message type {kind}"
            assert not got_response, "duplicate SOCK_DIAG connection response"
            got_response = True
            assert len(payload) >= 72, "short inet_diag_msg"
            family, state, _, _ = struct.unpack_from("=BBBB", payload)
            returned_ports, returned_source, returned_destination, interface, _, _ = sock_id.unpack_from(payload, 4)
            inode = u32.unpack_from(payload, 68)[0]
            assert (family == socket.AF_INET and state == 1 and interface == 0 and
                    (returned_ports, returned_source, returned_destination) ==
                    (ports, source, destination)), "SOCK_DIAG world-side TCP tuple/state mismatch"
            assert inode == sample["inode"], "SOCK_DIAG inode differs from the owned proc TCP connection"
            attribute = 72
            while attribute < len(payload):
                assert len(payload) - attribute >= 4, "short SOCK_DIAG attribute header"
                length, attr_flags = struct.unpack_from("=HH", payload, attribute)
                assert 4 <= length <= len(payload) - attribute, "invalid SOCK_DIAG attribute length"
                aligned_length = (length + 3) & ~3
                assert aligned_length <= len(payload) - attribute, "short SOCK_DIAG attribute alignment"
                attr_type = attr_flags & 0x3fff
                data = payload[attribute + 4:attribute + length]
                if attr_type == 2:
                    assert attr_flags == attr_type, "unsupported SOCK_DIAG attribute flags"
                    assert sample["snd_wnd"] is None, "duplicate TCP_INFO"
                    # Linux UAPI linux/tcp.h struct tcp_info: tcpi_snd_wnd
                    # at byte 228 is the scaled peer receive window in bytes.
                    assert len(data) >= 232, "TCP_INFO lacks actual tcpi_snd_wnd"
                    sample["snd_wnd"] = u32.unpack_from(data, 228)[0]
                    sample.update(
                        retransmit=data[2], backoff=data[4],
                        rto=u32.unpack_from(data, 8)[0],
                        snd_mss=u32.unpack_from(data, 16)[0],
                        unacked=u32.unpack_from(data, 24)[0],
                        retrans=u32.unpack_from(data, 36)[0],
                        total_retrans=u32.unpack_from(data, 100)[0],
                        notsent=u32.unpack_from(data, 144)[0])
                attribute += aligned_length
        assert got_response, "SOCK_DIAG reply lacks the world connection"
        assert sample["snd_wnd"] is not None, "SOCK_DIAG lacks TCP_INFO"
    except (AssertionError, OSError, ValueError, struct.error) as error:
        if sample is not None:
            peer.tcp_evidence = " ".join(f"{key}={value}" for key, value in sample.items())
        raise AssertionError(services.redact(
            f"owned world proc/TCP_INFO proof failed: {error}; "
            f"last TCP={getattr(peer, 'tcp_evidence', 'unobserved')}; "
            f"last pressure={getattr(peer, 'pressure_evidence', 'unobserved')}")) from None
    peer.tcp_evidence = " ".join(f"{key}={value}" for key, value in sample.items())
    return sample["send_queue"], sample["snd_wnd"] == 0


def pressure(services, recipient, readable_peers):
    initial = recipient.until(lambda kind, body: kind == 6)
    giver = next(entity for entity in initial["entities"] if entity["kind"] == "questgiver")
    destination = (giver["x"] - 100, giver["y"])
    recipient.connection.sendall(frame(7, struct.pack("!Iii", 1, *destination)))
    recipient.until(lambda kind, body: kind == 6 and
                    (body["character"]["state"]["x"], body["character"]["state"]["y"]) == destination)
    recipient.action("interact", target=giver["id"])
    recipient.until(lambda kind, body: kind == 6 and any(
        event.get("type") == "dialogue" and event.get("target") == giver["id"]
        for event in body["events"]))
    state_size = max(recipient.frame_sizes)
    assert state_size * 12 < 131072, "pressure stimulus exceeds bounded pending-frame budget"
    # The finite 64 KiB receive policy was negotiated before connect, honoring
    # the earlier advertised window; do not read the receiver until resume.
    assert hasattr(socket, "TCP_WINDOW_CLAMP"), "Linux backpressure fixture requires TCP_WINDOW_CLAMP"
    deadline = time.monotonic() + 90
    want_write = re.compile(
        rf"actor={recipient.id} peer=([1-9]\d*) blocked=want_write "
        r"pending=([1-9]\d*) write_size=([1-9]\d*)")
    saw_want_write = False
    ss = services.binary("ss")
    next_action = next_probe = time.monotonic()
    full_since = None
    queued = limit = pending = 0
    zero_window = False
    while time.monotonic() < deadline:
        services.pump(0)
        if not saw_want_write:
            saw_want_write = any(want_write.fullmatch(line)
                                 for line in services.world["lines"])
        for peer in readable_peers:
            peer.available()
        now = time.monotonic()
        if not saw_want_write and now >= next_action:
            # Pace real dialogue responses while progressing; freezing on
            # actual WANT_WRITE prevents a stalled action batch from bursting.
            recipient.action("interact", target=giver["id"])
            next_action = time.monotonic() + 1 / 30
        if now >= next_probe:
            next_probe = now + 0.04
            queued, zero_window = send_queue(services, recipient)
            limit = pending = 0
            tcp = ""
            if queued and (saw_want_write or zero_window):
                tcp = services.command([
                    ss, "-tinmH", "state", "established",
                    f"( sport = :{services.world_port} and dport = :{recipient.connection.getsockname()[1]} )",
                ], timeout=2)
                memory = re.search(r"\btb(\d+),.*?\bw(\d+)", tcp)
                assert memory, services.redact(
                    f"owned world ss lacks socket memory: {recipient.tcp_evidence}; ss={tcp[-4000:]!r}")
                limit, pending = map(int, memory.groups())
            recipient.pressure_evidence = services.redact(
                f"want_write={saw_want_write} zero_window={zero_window} "
                f"{recipient.tcp_evidence} tb={limit} w={pending} ss={tcp[-4000:]!r}")
            # Linux include/net/sock.h: sk_stream_is_writeable uses
            # sk_stream_min_wspace (w>>1), so writable needs tb-w >= w>>1.
            # Exact w>=tb was invalid: memory/writable gates precede that cap.
            # Accounting alone (including sndbuf_limited) is not EAGAIN.
            memory_blocked = (limit > 0 and pending > 0 and
                              (pending >= limit or limit - pending < (pending >> 1)))
            if queued and saw_want_write and zero_window and memory_blocked:
                if full_since is None:
                    full_since = time.monotonic()
                elif time.monotonic() - full_since >= 0.1:
                    # Stop actions on actual WANT_WRITE; periodic native
                    # snapshots/states still retry the stable TLS write
                    # throughout this owned, zero-window blocked interval.
                    assert time.monotonic() - full_since < 0.5
                    assert state_size * 12 < 262144
                    return giver, destination, queued, limit
            else:
                full_since = None
        time.sleep(0.002)
    raise AssertionError(
        "real recipient never reached WANT_WRITE with an owned memory-nonwritable "
        f"kernel send buffer and zero window: want_write={saw_want_write} "
        f"send_queue={queued} zero_window={zero_window} tb={limit} w={pending}; "
        f"last TCP={getattr(recipient, 'tcp_evidence', 'unobserved')}; "
        f"last pressure={getattr(recipient, 'pressure_evidence', 'unobserved')}")


def resume_reads(peer):
    peer.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    if hasattr(socket, "TCP_WINDOW_CLAMP"):
        peer.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_WINDOW_CLAMP, 1 << 20)


def backpressure_case(services, identities, *, admin_identity=None):
    (session, character), (healthy_session, healthy_character) = identities
    with ExitStack() as stack:
        recipient = stack.enter_context(ObservedPeer(services, session, character, receive_buffer=1 << 16))
        healthy = stack.enter_context(ObservedPeer(services, healthy_session, healthy_character))
        healthy.until(lambda kind, body: kind == 6)
        before = healthy.until(lambda kind, body: kind == 4)["tick"]
        admin = None
        if admin_identity is not None:
            admin = stack.enter_context(ObservedPeer(services, *admin_identity))
            admin.until(lambda kind, body: kind == 6)
        readable_peers = [healthy] + ([admin] if admin is not None else [])
        giver, destination, queued, limit = pressure(services, recipient, readable_peers)
        assert queued > 0 and limit > 0
        # Valid readable traffic is sent while the original TLS write is still
        # blocked; only resuming reads allows the queued destination to execute.
        recipient.connection.sendall(frame(7, struct.pack(
            "!Iii", 2, destination[0], destination[1] + 20)))
        if admin is None:
            before_records = len(recipient.records)
            before_bytes = sum(recipient.frame_sizes)
            before_tcp = recipient.tcp_evidence
            resume_reads(recipient)
            try:
                current = recipient.until(lambda kind, body: kind == 6 and
                                          (body["character"]["state"]["x"], body["character"]["state"]["y"]) ==
                                          (destination[0], destination[1] + 20), timeout=8)
            except Exception as error:
                diagnostics = [
                    f"before resume TCP={before_tcp}",
                    f"decoded_frames={len(recipient.records)} delta_frames={len(recipient.records) - before_records} "
                    f"decoded_bytes={sum(recipient.frame_sizes)} delta_bytes={sum(recipient.frame_sizes) - before_bytes} "
                    f"last_tick={recipient.last_tick}",
                ]
                try:
                    state = next(body["character"]["state"] for kind, body in reversed(recipient.records) if kind == 6)
                    diagnostics.append(f"last_position=({state['x']}, {state['y']})")
                except Exception as sampling_error:
                    diagnostics.append(services.redact(f"position sampling failed: {sampling_error}"))
                try:
                    send_queue(services, recipient)
                    diagnostics.append(f"after resume TCP={recipient.tcp_evidence}")
                except Exception as sampling_error:
                    diagnostics.append(services.redact(f"TCP sampling failed: {sampling_error}"))
                try:
                    ss = shutil.which("ss")
                    if ss:
                        tcp = services.command([
                            ss, "-tinmH", "state", "established",
                            f"( sport = :{services.world_port} and dport = :{recipient.connection.getsockname()[1]} )",
                        ], timeout=2)
                        diagnostics.append(f"after resume ss={tcp[-4000:]!r}")
                except Exception as sampling_error:
                    diagnostics.append(services.redact(f"ss sampling failed: {sampling_error}"))
                raise AssertionError(services.redact(
                    f"{error}; backpressure resume diagnostics: " + "; ".join(diagnostics))) from error
            assert current["self"] == recipient.id
            snapshot = recipient.until(lambda kind, body: kind == 4 and any(
                player["id"] == recipient.id and
                (player["x"], player["y"]) == (destination[0], destination[1] + 20)
                for player in body["players"]))
            assert {player["id"] for player in snapshot["players"]} == {recipient.id, healthy.id}
            recipient.action("interact", target=giver["id"])
            recipient.until(lambda kind, body: kind == 6 and any(
                event.get("type") == "dialogue" for event in body["events"]))
        else:
            started = time.monotonic()
            admin.action("moderate", target=recipient.id, operation="kick")
            resume_reads(recipient)
            recipient.closed(started + 2.5)
            assert any(kind == 6 and any(
                event.get("type") == "kick" and event.get("by") == admin.id
                for event in body["events"]) for kind, body in recipient.records)
        continuation = healthy.until(lambda kind, body: kind == 4 and body["tick"] > before)
        assert healthy.id in {player["id"] for player in continuation["players"]}
        healthy.action("chat", channel="local", text="Healthy after blocked TLS write")
        healthy.until(lambda kind, body: kind == 6 and any(
            event.get("type") == "chat" and event.get("text") == "Healthy after blocked TLS write"
            for event in body["events"]))
    released(services, [character, healthy_character] + ([admin_identity[1]] if admin_identity else []))
    print(("backpressured graceful admin kick" if admin_identity else
           "backpressured pending-write/read ordering and peer isolation") + ": passed")


def stalled_native(services, identity_pair):
    """A healthy native client that pauses longer than its 5 s read deadline (scene load, first GPU upload)
    must service what the world sent meanwhile instead of declaring a timeout."""
    session, character = identity_pair
    child = native(services, session, character, ticks=160)
    try:
        services.until(lambda: accepted(child)[1], "stalled native first state")
        world_lines = len(services.world["lines"])
        os.kill(child["process"].pid, signal.SIGSTOP)
        try:
            time.sleep(7)
        finally:
            os.kill(child["process"].pid, signal.SIGCONT)
        assert finished(services, child, "stalled native") == 0, \
            "native client lost a live session after a 7 s stall: " + " | ".join(list(child["lines"])[-3:])
        closes = [line for line in list(services.world["lines"])[world_lines:] if " close=" in line]
        assert all(line.endswith("close=goodbye") for line in closes), closes
    finally:
        services.stop(child)
    released(services, [character])
    print("native client survives a 7 s main-thread stall: passed")


def graceful_native(services, native_identity, observer_identity, admin_identity):
    session, character = native_identity
    with ExitStack() as stack:
        observer = stack.enter_context(ObservedPeer(services, *observer_identity))
        admin = stack.enter_context(ObservedPeer(services, *admin_identity))
        observer.until(lambda kind, body: kind == 6)
        admin.until(lambda kind, body: kind == 6)
        proxy = stack.enter_context(WorldProxy(services, forward=True))
        child = native(services, session, character, port=proxy.port)
        try:
            services.until(lambda: proxy.established.is_set() and all(accepted(child)) or proxy.errors,
                           "actual native graceful-kick bootstrap", timeout=8)
            assert not proxy.errors and proxy.established.is_set()
            native_id = struct.unpack("!I", proxy.records[0][6:10])[0]
            started = time.monotonic()
            admin.action("moderate", target=native_id, operation="kick")
            admin.action("moderate", target=observer.id, operation="kick")
            observer.closed(started + 2.5)
            services.until(lambda: child["process"].poll() is not None and child["eof"] and
                           proxy.world_clean_eof or proxy.errors, "native actual graceful kick", timeout=2.5)
            assert child["process"].returncode == 0 and proxy.world_clean_eof and not proxy.errors
            assert time.monotonic() - started < 3
            assert_real_prefix(proxy, child, character)
            _, states = accepted(child)
            for frames in (states, [body for kind, body in observer.records if kind == 6]):
                assert any(any(event.get("type") == "kick" and event.get("by") == admin.id
                               for event in body["events"]) for body in frames)
        finally:
            services.stop(child)
    released(services, [character, observer_identity[1], admin_identity[1]])
    print("actual native no-ticks graceful admin kick and strict TLS observer: passed")


def graceful_stop(services, native_identity, observer_identity, finite_identity):
    with ExitStack() as stack:
        observer = stack.enter_context(ObservedPeer(services, *observer_identity, receive_buffer=1 << 16))
        observer.until(lambda kind, body: kind == 6)
        children = []
        try:
            for identity_value, ticks in ((native_identity, None), (finite_identity, 1000000)):
                session, character = identity_value
                proxy = stack.enter_context(WorldProxy(services, forward=True))
                child = native(services, session, character, port=proxy.port, ticks=ticks)
                children.append((proxy, child, character, ticks))
                services.until(lambda: proxy.established.is_set() and all(accepted(child)) or proxy.errors,
                               "actual native server-stop bootstrap", timeout=8)
                assert not proxy.errors and proxy.established.is_set()
            pressure(services, observer, [])
            record_count = len(observer.records)
            started = time.monotonic()
            os.kill(services.world["process"].pid, signal.SIGTERM)
            resume_reads(observer)
            observer.closed(started + 2.5)
            services.until(lambda: services.world["process"].poll() is not None,
                           "bounded native world shutdown", timeout=2.5)
            assert services.world["process"].returncode == 0
            assert time.monotonic() - started < 3
            # No terminal gameplay event is invented: complete queued records
            # and authenticated close_notify are the server-stop contract.
            assert len(observer.records) > record_count and observer.clean_eof, "server stop dropped queued complete frames"
            # Observe every native EOF before re-decoding historical records:
            # proof construction/comparison is not transport shutdown latency.
            try:
                services.until(lambda: any(proxy.errors for proxy, _, _, _ in children) or all(
                    child["process"].poll() is not None and child["eof"] and proxy.world_clean_eof
                    for proxy, child, _, _ in children), "native server-stop EOF",
                    timeout=min(2.5, max(0.0, started + 3 - time.monotonic())))
            except AssertionError as error:
                raise AssertionError(
                    f"native shutdown observation failed: elapsed={time.monotonic() - started:.6f}s "
                    f"child_pids={[child['process'].pid for _, child, _, _ in children]}") from error
            for proxy, child, _, ticks in children:
                assert not proxy.errors and proxy.world_clean_eof
                assert child["process"].returncode == (0 if ticks is None else 1)
            elapsed = time.monotonic() - started
            assert elapsed < 3, (
                f"native EOF exceeded bounded shutdown: observed_elapsed={elapsed:.6f}s "
                f"child_pids={[child['process'].pid for _, child, _, _ in children]} "
                f"exit_codes={[child['process'].returncode for _, child, _, _ in children]} "
                f"pipe_eof={[child['eof'] for _, child, _, _ in children]} "
                f"world_tls_eof={[proxy.world_clean_eof for proxy, _, _, _ in children]}")
            for proxy, child, character, _ in children:
                assert_real_prefix(proxy, child, character)
        finally:
            for _, child, _, _ in children:
                services.stop(child)
    released(services, [native_identity[1], observer_identity[1], finite_identity[1]])
    services.start_world()
    print("actual native no-ticks server stop, unmet finite ticks, strict TLS close_notify: passed")


def main():
    args = arguments(__doc__)
    with Services(args) as services:
        try:
            source_requires_assets(services)
            services.start_world()
            wrong_ca, dns_cert = certificates(services)
            framing_case(services, 0, "clean authenticated record EOF")
            framing_case(services, 1, "trusted DNS SAN and clean EOF", cert=dns_cert, host="localhost")
            index = 2
            for offset in (1, 2, 3):
                framing_case(services, index, f"truncated header at {offset}", lambda record, n=offset: record[:n])
                index += 1
            for offset in (0, 1, -1):
                framing_case(services, index, f"truncated body at {offset}",
                             lambda record, n=offset: record[:4 + n] if n >= 0 else record[:n])
                index += 1
            for length in (0, 1, 65536):
                framing_case(services, index, f"malformed body length {length}",
                             lambda record, n=length: struct.pack("!I", n) + record[4:])
                index += 1
            framing_case(services, index, "malformed wire version", lambda record: record[:4] + b"\xff" + record[5:])
            index += 1
            index = session_contract_cases(services, index)
            snapshot_requires_state(services, index)
            index += 1
            same_tick_state_causality(services, index)
            index += 1
            appearance_api_cases(services, index)
            index += 1
            rejected_certificate(services, index + 1, "unrelated trust root", ca=wrong_ca)
            rejected_certificate(services, index + 2, "trusted certificate with wrong host SAN", cert=dns_cert)
            session, character = identity(services, index + 3)
            unauthorized = "0" * 64 if session != "0" * 64 else "1" * 64
            services.secrets.add(unauthorized)
            child = native(services, unauthorized, character)
            try:
                assert finished(services, child, "unauthorized session") == 1, "native accepted nonexistent session"
                assert not child["records"], "unauthorized native session received world state"
            finally:
                services.stop(child)
            released(services, [character])
            print("unauthorized native session: passed")
            transport_identities = [identity(services, index + offset) for offset in (4, 5)]
            admin_index = index + 6
            admin_identity = identity(services, admin_index)
            services.stop(services.auth)
            services.start_auth(admin=f"native_eof_{admin_index}")
            assert services.api("GET", "/v1/admin/status", session=admin_identity[0], admin=True)["accounts"] > 0
            native_identity = identity(services, index + 7)
            finite_identity = identity(services, index + 8)
            abrupt_isolation(services, transport_identities)
            backpressure_case(services, transport_identities)
            backpressure_case(services, transport_identities, admin_identity=admin_identity)
            stalled_native(services, identity(services, index + 9))
            graceful_native(services, native_identity, transport_identities[0], admin_identity)
            graceful_stop(services, native_identity, transport_identities[0], finite_identity)
        except Exception:
            trace = services.redact(traceback.format_exc())
            services.pump(0)
            world = getattr(services, "world", None)
            world_log = services.redact("\n".join(world["lines"]))[-24000:] if world is not None else ""
            raise AssertionError(trace + "\nowned world (prioritized):\n" + world_log +
                                 "\nother service diagnostics:\n" +
                                 services.redact(services.diagnostics())[-12000:]) from None
    print("owned native TLS EOF, framing, trust, backpressure, isolation and graceful shutdown checks passed")


if __name__ == "__main__":
    main()
