#!/usr/bin/env python3
"""NONCANONICAL wave 30: the native client's shard-transfer state machine against two stub TLS shards.

Shard A admits the client, sends a state carrying {type:"transfer",host,port,ticket,reason,realm}, a goodbye
{"reason":"transfer"} and closes. The headless client must then hello shard B with the same session and character
plus the 64-hex ticket (v3 frame, 138-byte body) and accept B's world. Further cases: a refused ticket ends the
client with the destination's reason, a source shard that never closes is left after the 5 s grace, an empty
transfer host means the current host, and a malformed transfer event ends the session as an invalid message."""
import argparse
import json
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

VERSION = 3
SESSION = "ab" * 32
CHARACTER = 4242
TICKET = "0123456789abcdef" * 4


def frame(kind, payload=b"", version=VERSION):
    body = bytes((version, kind)) + payload
    return struct.pack("!I", len(body)) + body


def state(self_id, tick, events=()):
    character = {"zone": "test", "x": 0, "y": 0, "hp": 10, "max_hp": 10, "mana": 0, "max_mana": 0, "level": 1, "xp": 0,
                 "currency": 0, "inventory": [], "equipment": {}, "quests": {}, "buffs": []}
    body = {"self": self_id, "tick": tick, "zone": "test", "source_registry_id": None, "bounds": [-1000, -1000, 1000, 1000],
            "character": {"name": "Deneme", "class_id": 1, "appearance": {}, "equipment": [], "state": character},
            "xp_interval": 1500, "entities": [], "events": list(events), "catalogs": {}}
    return frame(6, json.dumps(body).encode())


def welcome(self_id, tick):
    return frame(2, struct.pack("!II", self_id, tick))


def snapshot(self_id, tick):
    return frame(4, struct.pack("!IBB", tick, 0, 1) + struct.pack("!Iii", self_id, 0, 0))


def goodbye(reason):
    return frame(5, json.dumps({"reason": reason}).encode())


class Shard(threading.Thread):
    """One stub shard: accepts one TLS client, records its hello, then runs `script(connection, hello)`."""

    def __init__(self, context, script):
        super().__init__(daemon=True)
        self.context, self.script = context, script
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(20)
        self.port = self.listener.getsockname()[1]
        self.hello = None
        self.error = None

    def read_frame(self, connection):
        header = self.exact(connection, 4)
        (length,) = struct.unpack("!I", header)
        return self.exact(connection, length)

    @staticmethod
    def exact(connection, count):
        data = b""
        while len(data) < count:
            chunk = connection.recv(count - len(data))
            if not chunk:
                raise EOFError("client closed")
            data += chunk
        return data

    def run(self):
        try:
            raw, _ = self.listener.accept()
            raw.settimeout(20)
            with self.context.wrap_socket(raw, server_side=True) as connection:
                self.hello = self.read_frame(connection)
                self.script(self, connection, self.hello)
        except Exception as error:  # reported by the main thread
            self.error = error
        finally:
            self.listener.close()

    @staticmethod
    def close(connection):
        try:
            connection.unwrap()
        except (OSError, ssl.SSLError):
            pass


def certificates(root):
    def run(*command):
        subprocess.run(command, check=True, capture_output=True)
    ca, key = root / "ca.pem", root / "server.key"
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", root / "ca.key", "-out", ca, "-days", "1",
        "-subj", "/CN=IKV transfer test CA", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", root / "server.csr", "-subj", "/CN=localhost")
    (root / "server.ext").write_text("subjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\n"
                                     "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
                                     "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
    run("openssl", "x509", "-req", "-in", root / "server.csr", "-CA", ca, "-CAkey", root / "ca.key", "-CAcreateserial",
        "-out", root / "server.pem", "-days", "1", "-extfile", root / "server.ext")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(str(root / "server.pem"), str(key))
    return ca, context


def hello_fields(hello):
    assert hello[0] == VERSION and hello[1] == 1, "hello is not a v3 hello frame"
    session = hello[2:66].decode()
    (character,) = struct.unpack("!Q", hello[66:74])
    ticket = hello[74:].decode() if len(hello) > 74 else None
    assert len(hello) in (74, 138), "hello body has an unexpected length"
    return session, character, ticket


def source(event, close=True):
    def script(shard, connection, hello):
        session, character, ticket = hello_fields(hello)
        assert (session, character, ticket) == (SESSION, CHARACTER, None), "first hello carries a ticket or wrong identity"
        connection.sendall(welcome(1, 1) + state(1, 1, [event]) + goodbye("transfer"))
        if close:
            Shard.close(connection)
        else:
            time.sleep(8)  # a source shard that never closes: the client must leave after the 5 s grace
    return script


def destination(refuse=None):
    def script(shard, connection, hello):
        session, character, ticket = hello_fields(hello)
        assert (session, character, ticket) == (SESSION, CHARACTER, TICKET), "destination hello lost the ticket or identity"
        if refuse:
            connection.sendall(goodbye(refuse))
            Shard.close(connection)
            return
        connection.sendall(welcome(7, 1) + state(7, 1) + snapshot(7, 2))
        body = shard.read_frame(connection)  # the client's own goodbye after --ticks 1
        assert body[:2] == bytes((VERSION, 5)), "client did not end with a goodbye"
        Shard.close(connection)
    return script


def run_case(client, ca, context, name, *, host="127.0.0.1", close=True, refuse=None, event_override=None, expect_ok=True):
    b = Shard(context, destination(refuse))
    event = {"type": "transfer", "host": host, "port": b.port, "ticket": TICKET, "reason": "party", "realm": "Galata"}
    if event_override:
        event.update(event_override)
    a = Shard(context, source(event, close))
    a.start(); b.start()
    started = time.monotonic()
    result = subprocess.run([client, "--headless", "--host", "127.0.0.1", "--port", str(a.port), "--ca", str(ca),
                             "--session-stdin", "--character", str(CHARACTER), "--ticks", "1"],
                            input=SESSION + "\n", capture_output=True, text=True, timeout=60)
    elapsed = time.monotonic() - started
    a.join(15)
    if event_override is None:
        b.join(15)
    if a.error:
        raise AssertionError(f"{name}: source shard: {a.error!r}")
    if expect_ok:
        if b.error:
            raise AssertionError(f"{name}: destination shard: {b.error!r}")
        assert result.returncode == 0, f"{name}: client failed: {result.stderr}"
        lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        transfers = [line for line in lines if line.get("type") == "transfer"]
        assert len(transfers) == 1 and transfers[0]["port"] == b.port and transfers[0]["reason"] == "party" and \
            transfers[0]["realm"] == "Galata", f"{name}: transfer not reported once: {result.stdout}"
        assert transfers[0]["farewell"] in ("transfer", ""), f"{name}: unexpected farewell"
        states = [line["state"] for line in lines if line.get("type") == "state"]
        assert [s["self"] for s in states] == [1, 7], f"{name}: states were not source then destination: {states}"
        assert any(line.get("self") == 7 and "players" in line for line in lines), f"{name}: no destination snapshot"
    else:
        assert result.returncode != 0, f"{name}: client accepted an invalid transfer"
    print(f"PASS {name} ({elapsed:.1f}s)")
    return result, elapsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ikv-transfer-") as directory:
        ca, context = certificates(Path(directory))
        run_case(args.client, ca, context, "party transfer reconnects with the ticket")
        run_case(args.client, ca, context, "empty transfer host keeps the current host", host="")
        _, elapsed = run_case(args.client, ca, context, "silent source shard is left after the grace", close=False)
        assert 4.5 <= elapsed < 20, f"grace handoff took {elapsed:.1f}s"
        result, _ = run_case(args.client, ca, context, "refused ticket ends with the destination's reason",
                             refuse="Geçiş bileti geçersiz.", expect_ok=False)
        assert "Geçiş bileti geçersiz." in result.stderr, result.stderr
        run_case(args.client, ca, context, "malformed transfer event is an invalid message",
                 event_override={"ticket": "XYZ"}, expect_ok=False)
    print("client transfer checks passed")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"client transfer check failed: {error}", file=sys.stderr)
        sys.exit(1)
