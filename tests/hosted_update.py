#!/usr/bin/env python3
"""Hosted server automatic upgrade (docs/LAUNCHER_UPDATES.md "Hosted server"): a launcher-hosted supervisor of release N
notices a newer signed release N+1 installed into its install root, queues the world's announced restart (a connected
player receives the countdown), the world saves every character, and the supervisor hands over to N+1's supervisor,
which restarts PostgreSQL, ikv-auth and ikv-server; the character's saved position survives.

    python3 tests/hosted_update.py --old build-full/ikv-launcher --new build-next/ikv-launcher \
        --helper build-full/ikv-updater --client build-full/ikv-client --assets original/app/base

--new must be a launcher built with a higher IKV_VERSION (e.g. cmake -DIKV_VERSION=0.15.1); its ikv-auth/ikv-server are
the old build's (symlinked beside it). The N+1 "AppImage" is a signed stand-in that execs --new. Needs PostgreSQL server
tools, openssl and the genuine original assets (read only). Everything runs in a temporary directory with an ephemeral
Ed25519 release key and a private development CA; no network access beyond 127.0.0.1.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
import pwd
import secrets
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.request

PASSWORD = "hosted-update-" + secrets.token_hex(8)


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kwargs)


def version_of(launcher):
    return run([launcher, "--version"], env=dict(os.environ, QT_QPA_PLATFORM="offscreen", IKV_UPDATE_HANDOFF="1")).stdout.split()[-1]


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def pg_bin():
    for directory in [str(Path(p).parent) for p in [shutil.which("initdb")] if p] + \
            sorted((str(p) for p in Path("/usr/lib/postgresql").glob("*/bin")), reverse=True):
        if all((Path(directory) / tool).exists() for tool in ("initdb", "pg_ctl", "psql")):
            return Path(directory)
    raise SystemExit("PostgreSQL server tools (initdb, pg_ctl, psql) are required")


def provision(server, bin_dir):
    """The same layout provisionHost() creates in src/launcher_server.cpp (Linux)."""
    secrets_dir, socket_dir, pg = server / "secrets", server / "sock", server / "pg"
    for path in (server, secrets_dir, socket_dir, server / "backups"):
        path.mkdir(mode=0o700)
        path.chmod(0o700)
    run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-noenc", "-days", "2",
         "-subj", "/CN=IKV hosted update test CA", "-addext", "basicConstraints=critical,CA:TRUE",
         "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", secrets_dir / "development-ca-key",
         "-out", secrets_dir / "tls-ca"])
    run(["openssl", "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-noenc",
         "-subj", "/CN=IKV LAUNCHER LOCAL SERVICE", "-keyout", secrets_dir / "tls-key", "-out", secrets_dir / "request.csr"])
    (secrets_dir / "certificate.cnf").write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                                                 "extendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n"
                                                 "subjectAltName=DNS:localhost,IP:127.0.0.1\n")
    run(["openssl", "x509", "-req", "-in", secrets_dir / "request.csr", "-CA", secrets_dir / "tls-ca", "-CAkey",
         secrets_dir / "development-ca-key", "-set_serial", "0x" + secrets.token_hex(16), "-days", "2", "-sha256",
         "-extfile", secrets_dir / "certificate.cnf", "-out", secrets_dir / "tls-cert"])
    for stale in ("request.csr", "certificate.cnf"):
        (secrets_dir / stale).unlink()
    shutil.copyfile(secrets_dir / "tls-ca", secrets_dir / "client-ca.pem")
    user = pwd.getpwuid(os.geteuid()).pw_name
    run([bin_dir / "initdb", "-D", pg, "--no-locale", "--encoding=UTF8", "--auth-local=peer", "--auth-host=reject", "--username=" + user])
    (pg / "pg_hba.conf").write_text(f'local all "{user}" peer\nlocal ikv ikv scram-sha-256\n')
    with (pg / "postgresql.conf").open("a") as conf:
        conf.write(f"\nlisten_addresses = ''\nunix_socket_directories = '{socket_dir}'\nunix_socket_permissions = 0700\n"
                   "port = 5432\npassword_encryption = scram-sha-256\n")
    run([bin_dir / "pg_ctl", "start", "-D", pg, "-l", pg / "postgres.log", "-w", "-t", "60", "-s"])
    password = secrets.token_hex(32)
    run([bin_dir / "psql", "--no-psqlrc", "--quiet", "--set", "ON_ERROR_STOP=1", "--file", "-", "--dbname",
         f"host='{socket_dir}' port=5432 dbname=postgres user='{user}'"],
        input=f"CREATE ROLE ikv LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD '{password}';\n"
              "CREATE DATABASE ikv OWNER ikv;\n")
    run([bin_dir / "pg_ctl", "stop", "-D", pg, "-m", "fast", "-w", "-t", "30", "-s"])
    (secrets_dir / "dsn").write_text(f"host='{socket_dir}' port=5432 dbname=ikv user=ikv password={password} connect_timeout=5\n")
    for item in secrets_dir.iterdir():
        item.chmod(0o600)
    host = {"bind": "127.0.0.1", "lan_name": "", "keep_alive": True, "autostart": False, "name": "Güncelleme Testi", "bots": 0,
            "auth_port": free_port(), "admin_port": free_port(), "world_port": free_port(),
            "sans": ["DNS:localhost", "IP:127.0.0.1"], "host": "localhost", "update_delay_seconds": 5}
    (server / "host.json").write_text(json.dumps(host))
    (server / "host.json").chmod(0o600)


def state(server):
    try:
        return json.loads((server / "state.json").read_text())
    except (OSError, ValueError):
        return {}


def wait_for(predicate, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.25)
    raise AssertionError("timed out: " + what)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("old", "new", "helper", "client", "assets"):
        parser.add_argument("--" + name, required=True, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    old_version, new_version = version_of(args.old), version_of(args.new)
    assert old_version != new_version, "--new must report a different (higher) version"
    for tool in ("ikv-auth", "ikv-server"):
        beside = args.new.parent / tool
        if not beside.exists():
            beside.symlink_to(args.old.parent / tool)
    bin_dir = pg_bin()
    work = Path(tempfile.mkdtemp(prefix="ikv-hu-", dir="/tmp"))  # short: the PostgreSQL socket path limit
    server, install = work / "server", work / "install"
    supervisor_pid = None
    try:
        install.mkdir(mode=0o700)
        (install / "assets").symlink_to(args.assets)
        config = work / "config" / "IKVCommunity"
        config.mkdir(parents=True, mode=0o700)
        private, public = work / "release-private.pem", work / "release-public.pem"
        run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", private])
        run(["openssl", "pkey", "-in", private, "-pubout", "-out", public])
        (config / "IKVLauncher.conf").write_text(f"[updates]\ntrust={public}\n")
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=str(work / "config"))
        environment.pop("IKV_UPDATE_HANDOFF", None)
        provision(server, bin_dir)
        host = json.loads((server / "host.json").read_text())

        # Release N's supervisor creates the administrator (password on stdin only) and runs the server.
        started = subprocess.run([args.old, "--host-supervisor", server, "--asset-root", install / "assets",
                                  "--bootstrap-admin", "hostadmin"], input=PASSWORD + "\n", text=True, env=environment,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        assert started.returncode == 0, started.stdout
        running = wait_for(lambda: state(server) if state(server).get("phase") == "running" else None, 900, "release N running")
        supervisor_pid = running["pid"]
        assert running["version"] == old_version and running.get("upgrades") is True, running

        tls = ssl.create_default_context(cafile=str(server / "secrets/client-ca.pem"))
        def api(method, path, body=None, session=None):
            request = urllib.request.Request(f"https://localhost:{host['auth_port']}{path}", method=method,
                                             data=None if body is None else json.dumps(body).encode(),
                                             headers={"Content-Type": "application/json", **({"Authorization": "Bearer " + session} if session else {})})
            with urllib.request.urlopen(request, context=tls, timeout=30) as response:
                return json.loads(response.read())
        api("POST", "/v1/register", {"username": "gezgin_test", "password": PASSWORD})
        session = api("POST", "/v1/login", {"username": "gezgin_test", "password": PASSWORD})["session"]
        character = api("POST", "/v1/characters", {"name": "Gezgin", "class_id": 1, "appearance": {}}, session=session)["id"]
        before = api("GET", "/v1/characters", session=session)["characters"][0]["state"]

        # A connected player walking east until the world closes for the upgrade.
        client = subprocess.Popen([args.client, "--headless", "--host", "localhost", "--port", str(host["world_port"]),
                                   "--ca", server / "secrets/client-ca.pem", "--session-stdin", "--character", str(character),
                                   "--dx", "1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        client.stdin.write(session + "\n")
        client.stdin.close()
        lines = []
        reader = threading.Thread(target=lambda: lines.extend(client.stdout), daemon=True)
        reader.start()
        wait_for(lambda: any('"players"' in line for line in lines), 60, "player admitted")

        # Release N+1 arrives in the install root (what the launcher's automatic update installs): a signed stand-in
        # AppImage that runs the --new launcher.
        payload = work / "payload"
        (payload / "bin").mkdir(parents=True)
        image = payload / "bin/ikv-linux.AppImage"
        image.write_text(f'#!/bin/sh\nexec "{args.new}" "$@"\n')
        image.chmod(0o755)
        package = work / f"ikv-linux-x86_64-{new_version}.tar.gz"
        with package.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed, \
                tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for path in (payload / "bin", image):
                info = archive.gettarinfo(str(path), arcname=path.relative_to(payload).as_posix())
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                if info.isfile():
                    with path.open("rb") as source:
                        archive.addfile(info, source)
                else:
                    archive.addfile(info)
        manifest, signature = work / "manifest.json", work / "manifest.sig"
        run([sys.executable, Path(__file__).resolve().parents[1] / "tools/sign_manifest.py", "--root", payload, "--package", package,
             "--version", new_version, "--channel", "experimental", "--sequence", "1", "--output", manifest,
             "--signature", signature, "--key-file", private])
        run([args.helper, "install", "--manifest", manifest, "--signature", signature, "--package", package,
             "--trust", public, "--root", install])
        installed_at = time.monotonic()

        # The supervisor (polling every 30 s) announces, the world saves and exits, and N+1's supervisor takes over.
        def announced():
            return [line for line in lines if f"v{new_version}" in line and "announce" in line]
        notice = wait_for(announced, 90, "announce event at the connected player")
        assert "saniye içinde yeniden başlatılacak" in notice[0], notice[0]
        assert client.wait(timeout=60) is not None
        reader.join(timeout=5)
        positions = [player for line in lines if line.startswith('{"players"') or '"self"' in line
                     for snapshot in [json.loads(line)] if "self" in snapshot
                     for player in snapshot["players"] if player["id"] == snapshot["self"]]
        assert positions and positions[-1]["x"] != positions[0]["x"], "the player did not move before the upgrade"
        upgraded = wait_for(lambda: state(server) if state(server).get("phase") == "running"
                            and state(server).get("version") == new_version else None, 900, "release N+1 running")
        supervisor_pid = upgraded["pid"]
        log = (server / "server.log").read_text(errors="replace")
        assert "yeni sürüm gözetmeni başlatılıyor" in log and "PostgreSQL durduruldu" in log, log[-4000:]

        # The character's saved state survived the upgrade (the world saved it before exiting 75).
        session = api("POST", "/v1/login", {"username": "gezgin_test", "password": PASSWORD})["session"]
        after = api("GET", "/v1/characters", session=session)["characters"][0]["state"]
        assert (after.get("x"), after.get("y")) == (positions[-1]["x"], positions[-1]["y"]), \
            ("saved position differs from the last position before the upgrade", positions[-1], after)
        print(json.dumps({"old": old_version, "new": new_version, "announce": json.loads(notice[0]).get("state", {}).get("events", notice[0])
                          if notice[0].startswith("{") else notice[0], "seconds_after_install": round(time.monotonic() - installed_at, 1),
                          "last_position": positions[-1], "state_before": {k: before.get(k) for k in ("x", "y", "zone")},
                          "state_after": {k: after.get(k) for k in ("x", "y", "zone")}}, ensure_ascii=False, default=str))
        print("hosted server automatic upgrade check passed")
    finally:
        if supervisor_pid:
            try:
                os.kill(int(supervisor_pid), signal.SIGTERM)
                wait_for(lambda: state(server).get("phase") in ("stopped", "failed"), 120, "supervisor stop")
            except (ProcessLookupError, AssertionError):
                pass
        if (server / "pg/postmaster.pid").exists():
            subprocess.run([bin_dir / "pg_ctl", "stop", "-D", server / "pg", "-m", "immediate", "-w", "-s"], check=False)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
