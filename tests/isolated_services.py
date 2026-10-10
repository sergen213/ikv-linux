"""Owned native TLS services and a disposable, Unix-socket-only PostgreSQL cluster."""

import argparse
from collections import deque
from contextlib import AbstractContextManager
import http.client
import json
import linecache
import os
from pathlib import Path
import pwd
import re
import selectors
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time


def arguments(description):
    parser = argparse.ArgumentParser(description=description)
    for name in ("auth", "server", "client", "source"):
        parser.add_argument("--" + name, required=True)
    return parser.parse_args()


def frame(kind, payload=b"", version=2):
    body = bytes((version, kind)) + payload
    return struct.pack("!I", len(body)) + body


def hello(session, character):
    return frame(1, session.encode("ascii") + struct.pack("!Q", character))


def exact(connection, size):
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise EOFError("peer closed an incomplete frame")
        result.extend(chunk)
    return bytes(result)


class WorldPeer(AbstractContextManager):
    def __init__(self, services, session, character, *, receive_buffer=None, strict_tls=False, defer_hello=False):
        self.services = services
        self.connection = services.connect(services.world_port, receive_buffer=receive_buffer, strict_tls=strict_tls)
        self.buffer = bytearray()
        self.id = None
        self.last_tick = -1
        try:
            if not defer_hello:
                self.connection.sendall(hello(session, character))
                self.until(lambda kind, body: kind == 2)
        except BaseException:
            self.connection.close()
            raise

    def receive(self):
        while len(self.buffer) < 4:
            self.fill()
        length = struct.unpack("!I", self.buffer[:4])[0]
        assert 2 <= length <= 65535, "invalid server frame length"
        while len(self.buffer) < length + 4:
            self.fill()
        body = bytes(self.buffer[4:length + 4])
        del self.buffer[:length + 4]
        assert body[0] == 2, "unexpected wire version"
        kind, payload = body[1], body[2:]
        if kind in (6, 8):
            payload = json.loads(payload)
            assert isinstance(payload, dict), "world state is not an object"
        elif kind == 2:
            assert len(payload) == 8
            self.id, tick = struct.unpack("!II", payload)
            payload = {"self": self.id, "tick": tick}
        elif kind == 4:
            tick, count = struct.unpack("!IH", payload[:6])
            assert len(payload) == 6 + 12 * count
            players = [dict(zip(("id", "x", "y"), struct.unpack("!Iii", payload[6 + 12*i:18 + 12*i]))) for i in range(count)]
            ids = [player["id"] for player in players]
            assert 1 <= count <= 32 and ids == sorted(set(ids)) and self.id in ids
            assert all(player["id"] > 0 and -1536000 <= player["x"] <= 1536000 and -1536000 <= player["y"] <= 1536000 for player in players)
            assert tick > self.last_tick, "world snapshot tick regressed"
            self.last_tick = tick
            payload = {"tick": tick, "players": players}
        return kind, payload

    def fill(self):
        chunk = self.connection.recv(65536)
        if not chunk:
            raise EOFError("owned world closed before expected record")
        self.buffer.extend(chunk)

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        last_records = deque(maxlen=4)
        last_events = []
        while time.monotonic() < deadline:
            self.services.pump(0)
            try:
                kind, body = self.receive()
            except socket.timeout:
                continue
            if predicate(kind, body):
                return body
            summary = {"kind": kind}
            if isinstance(body, dict):
                summary.update({key: body[key] for key in
                                ("self", "tick", "zone", "bounds", "source_registry_id", "xp_interval") if key in body})
                if kind == 6:
                    current = body["character"]["state"]
                    summary["state"] = {key: current.get(key) for key in
                                        ("zone", "x", "y", "hp", "mana", "level", "xp", "equipment")}
                    summary["entities"] = [{key: entity.get(key) for key in ("id", "x", "y", "hp")}
                                           for entity in body["entities"][:8]]
                    summary["events"] = [{key: event.get(key) for key in ("type", "code", "action", "target")}
                                         for event in body["events"][:8]]
                    if summary["events"]:
                        last_events = summary["events"]
                elif kind == 4:
                    summary["players"] = body["players"][:8]
            last_records.append(self.services.redact(json.dumps(summary, ensure_ascii=True))[:1200])
        caller = sys._getframe(1)
        stage = f"{Path(caller.f_code.co_filename).name}:{caller.f_lineno} ({caller.f_code.co_name})"
        if caller.f_code.co_name in ("state", "event") and caller.f_back is not None:
            parent = caller.f_back
            stage += f" from {Path(parent.f_code.co_filename).name}:{parent.f_lineno} ({parent.f_code.co_name})"
            condition_file, condition_line = parent.f_code.co_filename, parent.f_lineno
        else:
            condition_file, condition_line = predicate.__code__.co_filename, predicate.__code__.co_firstlineno
        condition = " ".join(linecache.getline(condition_file, condition_line + offset).strip() for offset in range(8))
        raise AssertionError(self.services.redact(
            f"deadline awaiting authoritative world record: peer={self.id}; stage={stage}; "
            f"condition_excerpt={condition[:512]}; last_events={json.dumps(last_events)[:1200]}; last_records={list(last_records)}"))

    def action(self, action, **fields):
        self.connection.sendall(frame(8, json.dumps({"action": action, **fields}, ensure_ascii=False, separators=(",", ":")).encode()))

    def __exit__(self, *error):
        try:
            self.connection.sendall(frame(5))
        except OSError:
            pass
        self.connection.close()


class Services(AbstractContextManager):
    def __init__(self, args):
        self.args = args
        self.temporary = tempfile.TemporaryDirectory(prefix="ikv-integration-", dir="/tmp")
        self.root = Path(self.temporary.name)
        self.selector = selectors.DefaultSelector()
        self.children = []
        self.secrets = set()
        self.pg_started = False
        self.pg_prefix = []
        self.auth_port = self.world_port = None

    def redact(self, text):
        for value in sorted(self.secrets, key=len, reverse=True):
            if value:
                text = text.replace(value, "[redacted]")
        return re.sub(r"\b[0-9a-fA-F]{64}\b", "[redacted-token]", text)

    def command(self, argv, *, postgres=False, input=None, timeout=30):
        process = subprocess.Popen((self.pg_prefix if postgres else []) + list(map(str, argv)), stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        try:
            output, _ = process.communicate(input=input, timeout=timeout)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise
        assert process.returncode == 0, self.redact(output[-12000:])
        return output

    def binary(self, name):
        found = shutil.which(name)
        if not found:
            bindir = self.command(["pg_config", "--bindir"]).strip()
            candidate = Path(bindir) / name
            if candidate.is_file():
                found = str(candidate)
        assert found, "required executable not installed: " + name
        return found

    def secret_file(self, name, value):
        path = self.root / name
        with open(path, "w", opener=lambda p, flags: os.open(p, flags, 0o600)) as output:
            output.write(value)
        os.chmod(path, 0o600)
        return path

    def __enter__(self):
        try:
            self.bootstrap()
            return self
        except BaseException as error:
            self.__exit__(type(error), error, error.__traceback__)
            raise

    def bootstrap(self):
        os.chmod(self.root, 0o700)
        self.pgdata = self.root / "postgres"
        self.pgsocket = self.root / "socket"
        self.pgdata.mkdir(mode=0o700)
        self.pgsocket.mkdir(mode=0o700)
        if os.geteuid() == 0:
            try:
                user = pwd.getpwnam("postgres")
            except KeyError:
                user = pwd.getpwnam("nobody")
            for path in (self.root, self.pgdata, self.pgsocket):
                os.chown(path, user.pw_uid, user.pw_gid)
            self.pg_prefix = ["runuser", "-u", user.pw_name, "--"]
        self.initdb, self.pgctl = self.binary("initdb"), self.binary("pg_ctl")
        self.psql, self.pgdump = self.binary("psql"), self.binary("pg_dump")
        self.command([self.initdb, "-D", self.pgdata, "--no-locale", "--encoding=UTF8", "--auth-local=trust", "--auth-host=reject", "--username=ikv_test"], postgres=True)
        self.pg_started = True
        self.command([self.pgctl, "-D", self.pgdata, "-l", self.pgdata / "postgres.log", "-o", "-c listen_addresses='' -c unix_socket_permissions=0700 -p 5432 -k " + str(self.pgsocket), "-w", "-t", "15", "start"], postgres=True)
        self.dsn = "host=" + str(self.pgsocket) + " hostaddr='' port=5432 options='' connect_timeout=5 user=ikv_test dbname=postgres"
        self.sql("CREATE DATABASE ikv_test")
        self.dsn = self.dsn.replace("dbname=postgres", "dbname=ikv_test")
        assert self.sql("SHOW listen_addresses") == "", "disposable PostgreSQL exposed a TCP listener"
        assert self.sql("SHOW unix_socket_permissions") == "0700"
        self.dsn_file = self.secret_file("dsn", self.dsn + "\n")
        self.ca, self.cert, self.key = self.root / "ca.pem", self.root / "server.pem", self.root / "server.key"
        openssl = self.binary("openssl")
        self.command([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", self.root / "ca.key", "-out", self.ca, "-days", "1", "-subj", "/CN=IKV isolated integration CA", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign"])
        self.command([openssl, "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", self.key, "-out", self.root / "server.csr", "-subj", "/CN=localhost"])
        extensions = self.secret_file("server.ext", "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
        self.command([openssl, "x509", "-req", "-in", self.root / "server.csr", "-CA", self.ca, "-CAkey", self.root / "ca.key", "-CAcreateserial", "-out", self.cert, "-days", "1", "-extfile", extensions])
        for path in self.root.iterdir():
            if path.is_file():
                os.chmod(path, 0o600)
        self.context = ssl.create_default_context(cafile=str(self.ca))
        self.start_auth()

    def sql(self, statement, *, dsn=None):
        return self.command([self.psql, "--no-psqlrc", "--tuples-only", "--no-align", "--set", "ON_ERROR_STOP=1", "--dbname", dsn or self.dsn, "--command", statement]).strip()

    def start(self, executable, *args, stdin=None, env=None):
        environment = dict(os.environ)
        environment["PATH"] = str(Path(self.pgdump).parent) + os.pathsep + environment.get("PATH", "")
        if env:
            environment.update(env)
        process = subprocess.Popen([str(executable), *map(str, args)], stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True, env=environment)
        child = {"process": process, "buffer": b"", "lines": deque(maxlen=120), "records": deque(maxlen=400), "eof": False}
        self.children.append(child)
        os.set_blocking(process.stdout.fileno(), False)
        self.selector.register(process.stdout, selectors.EVENT_READ, child)
        if stdin is not None:
            process.stdin.write(stdin.encode())
            process.stdin.close()
        return child

    def pump(self, timeout=0.02):
        for key, _ in self.selector.select(timeout):
            child = key.data
            chunk = os.read(key.fileobj.fileno(), 65536)
            if not chunk:
                self.selector.unregister(key.fileobj)
                child["eof"] = True
            child["buffer"] += chunk
            while b"\n" in child["buffer"]:
                raw, child["buffer"] = child["buffer"].split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace")
                child["lines"].append(self.redact(text))
                if text.startswith("{"):
                    child["records"].append(json.loads(text))
            assert len(child["buffer"]) <= 1024 * 1024, "oversized child log line"
            if child["eof"] and child["buffer"]:
                text = child["buffer"].decode("utf-8", errors="replace")
                child["buffer"] = b""
                child["lines"].append(self.redact(text))
                if text.startswith("{"):
                    child["records"].append(json.loads(text))

    def until(self, predicate, description, timeout=15):
        deadline = time.monotonic() + timeout
        while not predicate():
            assert time.monotonic() < deadline, "deadline awaiting " + description + "\n" + self.diagnostics()
            self.pump(0.03)

    def ready(self, child, service):
        def port():
            for line in child["lines"]:
                match = re.search(r"\b" + service + r"_port=(\d+)", line)
                if match:
                    return int(match.group(1))
            assert child["process"].poll() is None, "service exited before ready\n" + self.diagnostics()
        self.until(lambda: port() is not None, service + " readiness")
        return port()

    def stop(self, child):
        process = child["process"]
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.pump(0)

    def start_auth(self, admin=None):
        args = ["--bind", "127.0.0.1", "--port", "0", "--admin-bind", "127.0.0.1", "--admin-port", "0", "--cert", self.cert, "--key", self.key, "--dsn-file", self.dsn_file, "--migrations", Path(self.args.source) / "database/migrations", "--web-root", Path(self.args.source) / "web", "--backup-dir", self.root / "backups"]
        if admin:
            args.extend(("--admin-bootstrap", admin))
        self.auth = self.start(self.args.auth, *args)
        self.auth_port = self.ready(self.auth, "auth")
        self.admin_port = self.ready(self.auth, "admin")

    def independent_gameplay(self):
        config = json.loads((Path(self.args.source) / "config/gameplay.json").read_text())
        # Generated original content (tools/generate_content.cpp) is replaced by the compact fixture content.
        fixture = json.loads((Path(self.args.source) / "tests/independent_content.json").read_text())
        config.update({key: fixture[key] for key in ("zones", "defaults", "items", "quests", "npcs", "activities")})
        config["recipes"] = {}  # generated recipes reference generated items absent from the fixture
        config["bots"]["spawn"] = []  # mechanics/admission assertions count leases and players; bot tests spawn explicitly
        # Mechanics tests assert this compact independent layout, not the source-scaled playable one.
        config.update(movement_step=100, cell_size=1000, aoi=2000, interaction_range=150, combat_range=200, trade_range=200)
        for skill in config["skills"].values():
            if "range" in skill:
                skill["range"] = 200 if skill["source"][0] == 0 else 700
            if "radius" in skill:
                skill["radius"] = 300
        return config

    def start_world(self, gameplay=None, *, asset_root=None):
        if gameplay is None:
            gameplay = self.root / "independent-gameplay.json"
            gameplay.write_text(json.dumps(self.independent_gameplay()))
        args = ["--bind", "127.0.0.1", "--port", "0", "--cert", self.cert, "--key", self.key,
                "--dsn-file", self.dsn_file, "--gameplay", gameplay]
        if asset_root is not None:
            args.extend(("--asset-root", asset_root))
        self.world = self.start(self.args.server, *args)
        self.world_port = self.ready(self.world, "world")

    def connect(self, port, *, context=None, hostname="localhost", receive_buffer=None, strict_tls=False):
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            raw.settimeout(2)
            if receive_buffer is not None:
                raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
                if hasattr(socket, "TCP_WINDOW_CLAMP"):
                    raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_WINDOW_CLAMP, receive_buffer)
            raw.connect(("127.0.0.1", port))
            connection = (context or self.context).wrap_socket(raw, server_hostname=hostname, suppress_ragged_eofs=not strict_tls)
        except BaseException:
            raw.close()
            raise
        connection.settimeout(0.25)
        return connection

    def accepted(self, child, connections):
        assert any(child is owned for owned in self.children), "acceptance evidence requires an owned service"
        proc = Path("/proc") / str(child["process"].pid)

        def endpoint(address):
            host, port = address
            return f"{struct.unpack('=I', socket.inet_aton(host))[0]:08X}:{port:04X}"

        local = endpoint(("127.0.0.1", self.auth_port))
        pending = {endpoint(connection.getsockname()) for connection in connections}
        inodes = set()
        try:
            for fd in (proc / "fd").iterdir():
                try:
                    target = os.readlink(fd)
                except FileNotFoundError:
                    continue
                if target.startswith("socket:[") and target.endswith("]"):
                    inodes.add(target[8:-1])
            with (proc / "net/tcp").open() as rows:
                if rows.readline().split()[:4] != ["sl", "local_address", "rem_address", "st"]:
                    raise RuntimeError("unsupported Linux /proc TCP header for acceptance evidence")
                for row in rows:
                    fields = row.split()
                    if len(fields) < 10:
                        raise RuntimeError("unsupported Linux /proc TCP format for acceptance evidence")
                    if fields[1] == local and fields[2] in pending and fields[3] == "01" and fields[9] in inodes:
                        pending.remove(fields[2])
                        if not pending:
                            return True
        except OSError as error:
            raise RuntimeError("socket acceptance evidence requires readable Linux " + str(proc / "fd") + " and " + str(proc / "net/tcp")) from error
        return not pending

    def api(self, method, path, body=None, *, session=None, status=200, admin=False, source_ip=None):
        connection = http.client.HTTPSConnection("localhost", self.admin_port if admin else self.auth_port, context=self.context, timeout=5, source_address=(source_ip, 0) if source_ip is not None else None)
        headers = {"Content-Type": "application/json"}
        if session:
            headers["Authorization"] = "Bearer " + session
        try:
            connection.request(method, path, body=None if body is None else json.dumps(body, ensure_ascii=False).encode(), headers=headers)
            response = connection.getresponse()
            payload = response.read(1024 * 1024 + 1)
            assert len(payload) <= 1024 * 1024, "oversized API response"
            result = json.loads(payload) if payload else None
            assert response.status == status, self.redact(f"{method} {path}: expected {status}, got {response.status}: {result}")
            if isinstance(result, dict):
                for field in ("session", "token"):
                    if field in result:
                        self.secrets.add(result[field])
            return result
        finally:
            connection.close()

    def diagnostics(self):
        return "\n\n".join(Path(child["process"].args[0]).name + ":\n" + "\n".join(child["lines"]) for child in self.children)

    def __exit__(self, *error):
        failures = []
        for child in reversed(self.children):
            try:
                self.stop(child)
            except BaseException as failure:
                failures.append(self.redact(str(failure)))
                if child["process"].poll() is None:
                    child["process"].kill()
                    child["process"].wait(timeout=5)
            finally:
                child["process"].stdout.close()
        self.selector.close()
        try:
            if self.pg_started:
                self.command([self.pgctl, "-D", self.pgdata, "-m", "immediate", "-w", "-t", "10", "stop"], postgres=True)
        except BaseException as failure:
            failures.append(self.redact(str(failure)))
        finally:
            self.pg_started = False
            self.temporary.cleanup()
        if failures and not any(error):
            raise AssertionError("fixture cleanup failed: " + "; ".join(failures))


def world_admission_invariants(services, admin_session):
    """Fill the real world, replace departing actors, and fault a lease release."""
    password = "Owned-admission-Password!49"
    services.secrets.add(password)
    identities, spares = [], []
    peers, ownership, closed, closing = [], {}, set(), set()
    states, snapshots, seen_ids = {}, {}, set()
    world = services.world
    process = world["process"]
    paused = faulting = False
    fault_character = None

    def drain():
        services.pump(0)
        for peer in peers:
            if peer in closed:
                continue
            previous_timeout = peer.connection.gettimeout()
            peer.connection.setblocking(False)
            try:
                for _ in range(128):
                    try:
                        kind, body = peer.receive()
                    except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError):
                        break
                    except EOFError:
                        assert faulting or peer in closing, "healthy full-capacity actor disconnected"
                        assert faulting or not peer.buffer, "admission close truncated a world frame"
                        closed.add(peer)
                        break
                    except (ConnectionResetError, ssl.SSLEOFError):
                        assert faulting or (peer in closing and peer.id is None), "joined actor lost orderly TLS transport"
                        closed.add(peer)
                        break
                    if kind == 2:
                        assert peer.id > 0 and peer.id not in seen_ids, "duplicate actor admission identity"
                        seen_ids.add(peer.id)
                    elif kind == 6:
                        identity = ownership[peer]
                        assert body["self"] == peer.id
                        assert all(body["character"][field] == identity[field] for field in ("name", "class_id", "appearance"))
                        states[peer] = body["character"]["state"]
                    elif kind == 4:
                        snapshots[peer] = body
            finally:
                peer.connection.settimeout(previous_timeout)

    def await_drained(predicate, description, timeout=8):
        def ready():
            drain()
            return predicate()
        services.until(ready, description, timeout=timeout)

    def add_peer(identity, *, deferred=False):
        drain()
        peer = WorldPeer(services, identity["session"], identity["id"], strict_tls=True, defer_hello=deferred)
        peers.append(peer)
        ownership[peer] = identity
        if not deferred:
            assert peer.id > 0 and peer.id not in seen_ids
            seen_ids.add(peer.id)
        return peer

    def lease_count():
        return services.sql("SELECT count(*) FROM characters WHERE lease_instance IS NOT NULL")

    def released(identity):
        return services.sql(f"SELECT lease_instance IS NULL AND lease_expires_at IS NULL FROM characters WHERE id={identity['id']}") == "t"

    def replace(departing, identity):
        nonlocal paused
        pending = add_peer(identity, deferred=True)
        closing.update((departing, pending))
        os.killpg(process.pid, signal.SIGSTOP)
        paused = True
        services.until(lambda: "\nState:\tT" in (Path("/proc") / str(process.pid) / "status").read_text(),
                       "owned world stopped before queueing both protocol frames", timeout=2)
        try:
            departing.connection.sendall(frame(5))
            pending.connection.sendall(hello(identity["session"], identity["id"]))
        finally:
            os.killpg(process.pid, signal.SIGCONT)
            paused = False
        await_drained(lambda: pending.id is not None or pending in closed or process.poll() is not None,
                      "same-poll replacement admission or capacity closure")
        if process.poll() is not None:
            assert faulting, "normal actor replacement stopped the world"
            return pending
        if pending.id is None:
            if faulting and not released(identity):
                await_drained(lambda: process.poll() is not None, "failed admission lease rollback fail-stop")
                return pending
            assert pending in closed and released(identity), "capacity rejection orphaned a character lease"
            pending.connection.close()
        await_drained(lambda: departing in closed and released(ownership[departing]),
                      "departing actor detachment and lease release")
        if pending.id is None:
            pending = add_peer(identity)
        closing.discard(pending)
        await_drained(lambda: pending in states, "replacement authoritative character state")
        assert lease_count() == "32", "replacement did not restore exactly 32 owned leases"
        return pending

    try:
        services.until(lambda: services.api("GET", "/v1/status")["world"].get("player_count") == 0,
                       "empty world before admission regression")
        assert lease_count() == "0", "admission fixture requires no existing actor leases"
        # Dedicated source IP keeps these 36 credential requests below the public
        # 60/IP/minute budget without inheriting preceding security-test quotas.
        for account in range(4):
            username = f"admission_{account}"
            created = services.api("POST", "/v1/register", {"username": username, "password": password}, source_ip="127.0.0.5")
            first_session = services.api("POST", "/v1/login", {"username": username, "password": password}, source_ip="127.0.0.5")["session"]
            account_characters = []
            for slot in range(8):
                session = first_session if slot == 0 else services.api("POST", "/v1/login", {"username": username, "password": password}, source_ip="127.0.0.5")["session"]
                character = services.api("POST", "/v1/characters", {"name": f"Admission{account}_{slot}", "class_id": 1, "appearance": {}}, session=session)
                assert character["account_id"] == created["account_id"]
                identity = {**character, "session": session}
                identities.append(identity)
                account_characters.append(character["id"])
            listed = services.api("GET", "/v1/characters", session=first_session)["characters"]
            assert {item["id"] for item in listed} == set(account_characters)
        assert len({identity["session"] for identity in identities}) == 32
        assert len({identity["id"] for identity in identities}) == 32
        for slot in range(2):
            character = services.api("POST", "/v1/characters", {"name": f"AdmissionSpare{slot}", "class_id": 1, "appearance": {}}, session=admin_session)
            spares.append({**character, "session": admin_session})
        initial = [add_peer(identity) for identity in identities]
        actor_ids = {peer.id for peer in initial}
        assert len(actor_ids) == 32
        await_drained(lambda: all(peer in states and peer in snapshots and
                                  {item["id"] for item in snapshots[peer]["players"]} == actor_ids for peer in initial),
                      "all 32 authenticated actors observing the same full world")
        assert lease_count() == "32"
        await_drained(lambda: services.api("GET", "/v1/status")["world"].get("player_count") == 32,
                      "authoritative 32-player world heartbeat")
        for index, peer in enumerate(initial):
            peer.connection.sendall(frame(7, struct.pack("!Iii", 1, 100 + index, 0)))
        await_drained(lambda: all((states[peer]["x"], states[peer]["y"]) == (100 + index, 0)
                                  for index, peer in enumerate(initial)),
                      "32 actor movement roundtrips")
        character_ids = ",".join(str(identity["id"]) for identity in identities)
        positions = ",".join(f"({identity['id']},{100 + index})" for index, identity in enumerate(identities))
        await_drained(lambda: services.sql(f"SELECT count(*) FROM characters c JOIN (VALUES {positions}) expected(id,x) ON c.id=expected.id WHERE (c.state->>'x')::integer=expected.x AND (c.state->>'y')::integer=0 AND c.lease_instance IS NOT NULL") == "32",
                      "periodic persistence of all 32 actor roundtrips")
        persisted = json.loads(services.sql(f"SELECT json_agg(json_build_object('id',id,'account_id',account_id,'x',state->'x','y',state->'y','lease',lease_instance) ORDER BY id) FROM characters WHERE id IN ({character_ids})"))
        by_id = {row["id"]: row for row in persisted}
        for index, identity in enumerate(identities):
            row = by_id[identity["id"]]
            assert row["account_id"] == identity["account_id"] and row["lease"]
            assert (row["x"], row["y"]) == (100 + index, 0)

        overflow = add_peer(spares[0], deferred=True)
        closing.add(overflow)
        overflow.connection.sendall(hello(spares[0]["session"], spares[0]["id"]))
        await_drained(lambda: overflow in closed, "33rd actor capacity closure")
        assert overflow.id is None and released(spares[0]) and lease_count() == "32"
        overflow.connection.close()
        replacement = replace(initial[0], spares[0])
        remaining = initial[1:] + [replacement]
        replacement_ids = {peer.id for peer in remaining}
        await_drained(lambda: all(peer in snapshots and
                                  {item["id"] for item in snapshots[peer]["players"]} == replacement_ids for peer in remaining),
                      "32 peers observing the admitted replacement without a ghost actor")
        assert services.sql(f"SELECT lease_instance IS NOT NULL FROM characters WHERE id={spares[0]['id']}") == "t"

        fault_character = spares[1]["id"]
        services.sql(f"""
            CREATE FUNCTION admission_release_fault() RETURNS trigger LANGUAGE plpgsql AS $$
            BEGIN
                IF OLD.id = {fault_character} AND OLD.lease_instance IS NOT NULL AND NEW.lease_instance IS NULL THEN
                    RAISE EXCEPTION 'owned admission lease-release fault';
                END IF;
                RETURN NEW;
            END $$;
            CREATE TRIGGER admission_release_fault BEFORE UPDATE ON characters
            FOR EACH ROW EXECUTE FUNCTION admission_release_fault();
        """)
        faulting = True
        doomed = replace(initial[1], spares[1])
        if process.poll() is None:
            assert doomed.id is not None, "replacement never became a tracked actor"
            closing.add(doomed)
            doomed.connection.sendall(frame(5))
        await_drained(lambda: process.poll() is not None, "lease-release authority fail-stop", timeout=8)
        assert process.returncode == 1, "lease-release failure did not exit the world with code 1"
        assert services.sql(f"SELECT lease_instance IS NOT NULL AND lease_expires_at IS NOT NULL FROM characters WHERE id={fault_character}") == "t"
        expiries = services.sql("SELECT id,lease_instance,lease_expires_at FROM characters ORDER BY id")
        deadline = time.monotonic() + 1.25
        while time.monotonic() < deadline:
            services.pump(0.03)
        assert services.sql("SELECT id,lease_instance,lease_expires_at FROM characters ORDER BY id") == expiries, "exited world continued renewing character leases"
    finally:
        try:
            if paused:
                try:
                    os.killpg(process.pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            for peer in peers:
                try:
                    peer.connection.close()
                except OSError:
                    pass
            services.stop(world)
        finally:
            try:
                services.sql("DROP TRIGGER IF EXISTS admission_release_fault ON characters; DROP FUNCTION IF EXISTS admission_release_fault();")
                owned_ids = ",".join(str(identity["id"]) for identity in identities + spares)
                if owned_ids:
                    services.sql(f"UPDATE characters SET lease_instance=NULL,lease_expires_at=NULL WHERE id IN ({owned_ids})")
            finally:
                services.start_world()
