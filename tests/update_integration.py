#!/usr/bin/env python3
"""Exercise signed updates with a real helper, tar archives and disposable Ed25519 keys."""

import argparse
import gzip
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading


def run(command, succeeds=True, env=None, timeout=60):
    result = subprocess.run(
        [str(part) for part in command], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout, env=env,
    )
    diagnostic = (command, result.returncode, result.stdout, result.stderr)
    assert result.returncode == 0 if succeeds else result.returncode > 0, diagnostic
    return result


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def make_archive(package, payload, extra=None, change=None):
    with tarfile.open(package, "w:gz", format=tarfile.USTAR_FORMAT) as archive:
        if extra is not None:
            member, content = extra
            archive.addfile(member, io.BytesIO(content) if member.isfile() else None)
        for path in sorted(payload.rglob("*")):
            member = archive.gettarinfo(str(path), arcname=path.relative_to(payload).as_posix())
            member.uid = member.gid = 0
            member.uname = member.gname = ""
            member.mtime = 0
            if change is not None:
                change(member)
            if member.isfile():
                with path.open("rb") as source:
                    archive.addfile(member, source)
            else:
                archive.addfile(member)


def sign_fixture(manifest, signature, private_key, document):
    manifest.write_bytes(json.dumps(document, sort_keys=True, separators=(",", ":")).encode("ascii"))
    run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", private_key,
         "-in", manifest, "-out", signature])
    assert signature.stat().st_size == 64


def tree_snapshot(root):
    snapshot = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in sorted(directories + files):
            path = Path(directory) / name
            mode = path.lstat().st_mode
            relative = path.relative_to(root).as_posix()
            if stat.S_ISLNK(mode):
                snapshot[relative] = ("link", os.readlink(path))
            elif stat.S_ISREG(mode):
                snapshot[relative] = ("file", stat.S_IMODE(mode), path.read_bytes())
            else:
                snapshot[relative] = ("directory", stat.S_IMODE(mode))
    return snapshot


def launcher_tls_check(launcher, helper, workspace, public_key, releases):
    """Exercise signed updates and bounded HTTP bodies against an owned TLS repository."""
    import time

    tls = workspace / "tls"
    tls.mkdir()
    ca_key, ca_cert = tls / "ca-key.pem", tls / "ca.pem"
    server_key, server_cert = tls / "server-key.pem", tls / "server.pem"
    request, extensions = tls / "server.csr", tls / "server.ext"
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-subj", "/CN=IKV disposable update CA", "-keyout", ca_key, "-out", ca_cert])
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-subj", "/CN=localhost", "-keyout", server_key, "-out", request])
    extensions.write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\n"
                          "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                          "extendedKeyUsage=serverAuth\n", encoding="ascii")
    run(["openssl", "x509", "-req", "-in", request, "-CA", ca_cert, "-CAkey", ca_key,
         "-CAcreateserial", "-days", "1", "-sha256", "-extfile", extensions, "-out", server_cert])
    unrelated_ca = tls / "unrelated-ca.pem"
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-subj", "/CN=Untrusted disposable CA", "-keyout", tls / "unrelated-key.pem",
         "-out", unrelated_ca])
    for key in (ca_key, server_key, tls / "unrelated-key.pem"):
        key.chmod(0o600)
    responses, requests, faults = {}, [], {}
    all_faults = []

    def make_fault(status, mode, location=None, length=None, size=0):
        fault = dict(status=status, mode=mode, location=location, length=length, size=size,
                     headers=threading.Event(), ended=threading.Event(),
                     cancel=threading.Event(), closed=threading.Event())
        all_faults.append(fault)
        return fault

    class Repository(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            # Writes must not leave a handler stuck after the launcher aborts a reply.
            self.connection.settimeout(1)

        def do_GET(self):
            requests.append(self.path)
            fault = faults.get(self.path)
            self.close_connection = True
            try:
                if fault is not None:
                    self.send_response(fault["status"])
                    self.send_header("Connection", "close")
                    if fault["location"] is not None:
                        self.send_header("Location", fault["location"])
                    if fault["length"] is not None:
                        self.send_header("Content-Length", str(fault["length"]))
                    if fault["mode"] == "chunked":
                        self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    self.wfile.flush()
                    fault["headers"].set()
                    if fault["mode"] == "stream":
                        # Small, continuously available data defeats an inactivity-only timer.
                        # Do not exceed any body cap: this must fail on the wall deadline.
                        deadline = time.monotonic() + 6
                        while not fault["cancel"].wait(0.025):
                            if time.monotonic() >= deadline:
                                fault["ended"].set()
                                break
                            self.wfile.write(b" " * 8)
                            self.wfile.flush()
                    else:
                        if fault["mode"] == "chunked":
                            remaining = fault["size"]
                            while remaining and not fault["cancel"].is_set():
                                amount = min(4096, remaining)
                                self.wfile.write(("%x\r\n" % amount).encode("ascii"))
                                self.wfile.write(b"x" * amount + b"\r\n")
                                self.wfile.flush()
                                remaining -= amount
                        # Deliberately omit a body (declared oversize) or the terminal chunk.
                        # A correct launcher must reject before the fixture completes a body.
                        if not fault["cancel"].wait(6):
                            fault["ended"].set()
                            if fault["mode"] == "chunked":
                                self.wfile.write(b"0\r\n\r\n")
                                self.wfile.flush()
                    return
                if self.path == "/redirect/manifest.sig":
                    body = b"redirect"
                    self.send_response(302)
                    self.send_header("Location", origin + "/manifest.sig")
                    self.send_header("Content-Type", "text/plain")
                else:
                    body = responses.get(self.path)
                    if body is None:
                        self.send_error(404)
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json" if self.path.endswith(".json")
                                     or self.path == "/releases" else "application/octet-stream")
                self.send_header("Connection", "close")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except OSError:
                # An aborted HTTPS reply closes the owned socket; this is expected.
                pass
            finally:
                if fault is not None:
                    fault["closed"].set()

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Repository)
    # Join handlers on close rather than abandoning daemon threads between test cases.
    server.daemon_threads = False
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(server_cert, server_key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    origin = "https://localhost:" + str(server.server_port)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05},
                              name="owned-update-https", daemon=True)
    thread.start()
    root = workspace / "launcher-installation"
    (root / "settings").mkdir(parents=True)
    (root / "assets").mkdir()
    (root / "settings/user.json").write_bytes(b'{"volume":21}\n')
    (root / "assets/preserved.bin").write_bytes(b"launcher user assets")
    preserved = {name: tree_snapshot(root / name) for name in ("settings", "assets")}
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    environment["XDG_RUNTIME_DIR"] = str(tls / "qt-runtime")
    Path(environment["XDG_RUNTIME_DIR"]).mkdir(mode=0o700)
    for name in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"):
        environment[name] = str(tls / name.lower())
        Path(environment[name]).mkdir(mode=0o700)
    # Do not allow a developer's proxy configuration to intercept this owned repository.
    for name in list(environment):
        if name.lower() in ("http_proxy", "https_proxy", "all_proxy"):
            del environment[name]
    environment["NO_PROXY"] = environment["no_proxy"] = "localhost,127.0.0.1"
    # Startup handoff would exec the installed fixture "AppImage" instead of updating; it is exercised
    # explicitly by handoff() below. Restart after Güncelle sets the same guard itself.
    unguarded = dict(environment)
    environment["IKV_UPDATE_HANDOFF"] = "1"

    def publish(release):
        _, package, manifest, signature, document = release
        responses.clear()
        assets = []
        for name, path in (("manifest.json", manifest), ("manifest.sig", signature),
                           (package.name, package)):
            responses["/" + name] = path.read_bytes()
            download_path = "/redirect/manifest.sig" if name == "manifest.sig" else "/" + name
            assets.append({"name": name, "browser_download_url": origin + download_path})
        # Drafts and non-vX.Y.Z tags are never offered, even when they would sort first.
        responses["/releases"] = json.dumps([
            {"tag_name": "nightly", "draft": False, "prerelease": False, "assets": assets},
            {"tag_name": "v99.0.0", "draft": True, "prerelease": False, "assets": assets},
            {"tag_name": "v" + document["version"], "draft": False, "prerelease": False, "assets": assets},
        ]).encode("ascii")

    def launch(ca=ca_cert, succeeds=True, timeout_ms=2000):
        result = run([launcher, "--update-once", "--install-root", root,
                      "--repo", "owned-fixture/releases", "--trust", public_key,
                      "--update-ca", ca, "--release-api", origin + "/releases",
                      "--network-timeout-ms", str(timeout_ms),
                      "--channel", "stable"], succeeds=succeeds, env=environment, timeout=15)
        for name, expected in preserved.items():
            assert tree_snapshot(root / name) == expected, "launcher modified " + name
        return result

    def restarted(result, version):
        # Güncelle installed the release and the launcher exec'd its AppImage in the same process.
        assert "v" + version + " kuruldu" in result.stdout, result.stdout
        assert ("fixture-appimage " + version + " handoff=1 args=--update-once") in result.stdout, result.stdout

    def handoff():
        # Startup handoff: the installed, verified newer release replaces this launcher process.
        return run([launcher, "--version"], env=unguarded, timeout=15)

    def launch_fault(path, fault, succeeds=False):
        faults[path] = fault
        try:
            result = launch(succeeds=succeeds, timeout_ms=750)
            assert fault["headers"].is_set(), (path, "fixture was not requested")
            assert not fault["ended"].is_set(), (path, "launcher waited for body completion")
            return result
        finally:
            fault["cancel"].set()
            faults.pop(path, None)
            if fault["headers"].is_set():
                assert fault["closed"].wait(2), (path, "fixture handler did not stop")

    def assert_active_unchanged():
        assert (os.readlink(root / "current"),
                tree_snapshot((root / "current").resolve())) == active
        assert not (root / "previous").exists(), "failed launcher update changed previous"

    try:
        publish(releases[0])
        restarted(launch(), "1.0.0")
        assert os.readlink(root / "current") == "releases/1.0.0"
        assert json.loads(run([helper, "verify", "--version", "1.0.0", "--trust",
                               public_key, "--root", root]).stdout) == releases[0][4]
        assert handoff().stdout == "fixture-appimage 1.0.0 handoff=1 args=--version\n"
        assert set(requests) >= {"/releases", "/manifest.json", "/redirect/manifest.sig",
                                 "/manifest.sig", "/" + releases[0][1].name}, requests
        active = (os.readlink(root / "current"), tree_snapshot((root / "current").resolve()))
        publish(releases[1])
        requested_before = list(requests)
        launch(ca=unrelated_ca, succeeds=False)
        assert requests == requested_before, "untrusted TLS connection reached HTTP handler"
        assert_active_unchanged()

        result = launch_fault("/releases", make_fault(200, "stream"))
        diagnostic = result.stdout + result.stderr
        assert "HTTPS request wall deadline exceeded" in diagnostic, diagnostic
        assert_active_unchanged()

        for status in (302, 503):
            start = len(requests)
            fault = make_fault(status, "hold", location=origin + "/oversize-target"
                               if status == 302 else None, length=65537)
            result = launch_fault("/releases", fault)
            diagnostic = result.stdout + result.stderr
            assert "HTTPS response exceeds configured bound" in diagnostic, diagnostic
            assert "/oversize-target" not in requests[start:], requests[start:]
            assert_active_unchanged()

        for path, status, size in (("/manifest.sig", 200, 65), ("/releases", 503, 65537)):
            result = launch_fault(path, make_fault(status, "chunked", size=size))
            diagnostic = result.stdout + result.stderr
            assert "HTTPS response exceeds configured bound" in diagnostic, diagnostic
            assert_active_unchanged()

        start = len(requests)
        fault = make_fault(302, "stream", location="https://127.0.0.1:"
                           + str(server.server_port) + "/forbidden-target")
        result = launch_fault("/redirect/manifest.sig", fault)
        diagnostic = result.stdout + result.stderr
        assert "HTTPS origin refused" in diagnostic, diagnostic
        assert "/forbidden-target" not in requests[start:], requests[start:]
        assert_active_unchanged()

        for name in ("manifest.json", "manifest.sig", releases[1][1].name):
            original = responses["/" + name]
            if name == "manifest.json":
                responses["/" + name] = original + b" "
            else:
                changed = bytearray(original)
                changed[len(changed) // 2] ^= 1
                responses["/" + name] = bytes(changed)
            launch(succeeds=False)
            assert_active_unchanged()
            responses["/" + name] = original

        # Following headers must not wait for this valid redirect's never-ending body.
        start = len(requests)
        fault = make_fault(302, "stream", location=origin + "/manifest.sig")
        restarted(launch_fault("/redirect/manifest.sig", fault, succeeds=True), "2.0.0")
        assert "/manifest.sig" in requests[start:], requests[start:]
        assert os.readlink(root / "current") == "releases/2.0.0"
        assert os.readlink(root / "previous") == "releases/1.0.0"
        assert json.loads(run([helper, "verify", "--version", "2.0.0", "--trust",
                               public_key, "--root", root]).stdout) == releases[1][4]
        highwater = root / ".update-highwater/stable.json"
        accepted_highwater = highwater.read_bytes()
        publish(releases[0])
        launch(succeeds=False)
        assert os.readlink(root / "current") == "releases/2.0.0", "HTTPS replay downgraded release"
        assert os.readlink(root / "previous") == "releases/1.0.0"
        assert highwater.read_bytes() == accepted_highwater
        assert json.loads(run([helper, "rollback", "--trust", public_key,
                               "--root", root]).stdout) == releases[0][4]
        assert os.readlink(root / "current") == "releases/1.0.0"
        assert os.readlink(root / "previous") == "releases/2.0.0"
        assert handoff().stdout.startswith("fixture-appimage 1.0.0 "), "handoff ignored rollback"
        launch(succeeds=False)
        assert os.readlink(root / "current") == "releases/1.0.0"
        assert os.readlink(root / "previous") == "releases/2.0.0"
        assert highwater.read_bytes() == accepted_highwater, "HTTPS replay reset rollback freshness"
        publish(releases[1])
        restarted(launch(), "2.0.0")
        assert os.readlink(root / "current") == "releases/2.0.0"
        assert os.readlink(root / "previous") == "releases/1.0.0"
        assert highwater.read_bytes() == accepted_highwater
        for name, expected in preserved.items():
            assert tree_snapshot(root / name) == expected, "rollback modified " + name
        # A modified installed release is never executed: this build reports it and stays in charge.
        altered = root / "releases/2.0.0/data.txt"
        original = altered.read_bytes()
        altered.write_bytes(b"tampered")
        result = handoff()
        assert result.stdout.startswith("IKVLauncher ") and "fixture-appimage" not in result.stdout, result.stdout
        assert "Kurulu güncelleme kullanılamadı" in result.stderr, result.stderr
        altered.write_bytes(original)
        assert handoff().stdout.startswith("fixture-appimage 2.0.0 ")
    finally:
        for fault in all_faults:
            fault["cancel"].set()
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
        assert not thread.is_alive(), "owned HTTPS server did not stop"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True)
    parser.add_argument("--signer", default=str(Path(__file__).resolve().parents[1] / "tools/sign_manifest.py"))
    parser.add_argument("--launcher", help="Also exercise the launcher's real QtNetwork HTTPS update path")
    args = parser.parse_args()
    helper = Path(args.helper).resolve()
    signer = Path(args.signer).resolve()
    with tempfile.TemporaryDirectory(prefix="ikv-update-check-") as temporary:
        workspace = Path(temporary)
        private_key = workspace / "private.pem"
        public_key = workspace / "public.pem"
        run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", private_key])
        private_key.chmod(0o600)
        run(["openssl", "pkey", "-in", private_key, "-pubout", "-out", public_key])
        root = workspace / "installation"
        root.mkdir()
        (root / "settings").mkdir()
        (root / "assets").mkdir()
        (root / "settings/user.json").write_bytes(b'{"volume":37}\n')
        (root / "assets/preserved.bin").write_bytes(b"user assets remain outside releases\x00")
        preserved = {name: tree_snapshot(root / name) for name in ("settings", "assets")}

        def check_preserved():
            for name, expected in preserved.items():
                assert tree_snapshot(root / name) == expected, name

        def invoke(operation, *arguments, succeeds=True):
            result = run([helper, operation, *arguments, "--trust", public_key, "--root", root], succeeds)
            check_preserved()
            return json.loads(result.stdout) if succeeds else None

        def signing_command(payload, package, version, manifest, signature, sequence=2):
            return [sys.executable, signer, "--root", payload, "--package", package,
                    "--version", version, "--channel", "stable", "--sequence", str(sequence),
                    "--output", manifest, "--signature", signature, "--key-file", private_key]

        def make_release(version, sequence, appimage=False):
            directory = workspace / version
            directory.mkdir()
            payload = directory / "payload"
            (payload / "bin").mkdir(parents=True)
            (payload / "bin/ikv").write_bytes(b"#!/bin/sh\necho " + version.encode("ascii") + b"\n")
            (payload / "bin/ikv").chmod(0o755)
            if appimage:
                # Stand-in for the release AppImage that the launcher execs on restart/handoff.
                (payload / "bin/ikv-linux.AppImage").write_bytes(
                    b'#!/bin/sh\necho "fixture-appimage ' + version.encode("ascii")
                    + b' handoff=${IKV_UPDATE_HANDOFF:-unset} args=$*"\n')
                (payload / "bin/ikv-linux.AppImage").chmod(0o755)
            (payload / "data.txt").write_bytes(b"release " + version.encode("ascii") + b"\n")
            (payload / "data.txt").chmod(0o644)
            package = directory / (version + ".tar.gz")
            manifest = directory / "manifest.json"
            signature = directory / "manifest.sig"
            make_archive(package, payload)
            command = signing_command(payload, package, version, manifest, signature, sequence)
            run(command)
            first_manifest, first_signature = manifest.read_bytes(), signature.read_bytes()
            run(command)
            assert manifest.read_bytes() == first_manifest, "non-deterministic manifest"
            assert signature.read_bytes() == first_signature, "non-deterministic Ed25519 signature"
            assert len(first_signature) == 64
            document = json.loads(first_manifest)
            assert document["schema"] == 2 and document["sequence"] == sequence
            assert document["package_sha256"] == sha256(package.read_bytes())
            assert [entry["path"] for entry in document["files"]] == (
                ["bin/ikv", "bin/ikv-linux.AppImage", "data.txt"] if appimage else ["bin/ikv", "data.txt"])
            return payload, package, manifest, signature, document

        def install(release, succeeds=True):
            _, package, manifest, signature, document = release
            result = invoke("install", "--manifest", manifest, "--signature", signature,
                            "--package", package, succeeds=succeeds)
            if succeeds:
                assert result == document
                assert os.readlink(root / "current") == "releases/" + document["version"]
                target = root / "releases" / document["version"]
                assert (target / ".ikv-update/manifest.json").read_bytes() == manifest.read_bytes()
                assert (target / ".ikv-update/signature.bin").read_bytes() == signature.read_bytes()
                for entry in document["files"]:
                    path = target / entry["path"]
                    assert path.stat().st_size == entry["size"]
                    assert sha256(path.read_bytes()) == entry["sha256"]
                    assert bool(path.stat().st_mode & 0o111) == entry["executable"]
            return result

        def active_snapshot():
            return (os.readlink(root / "current"),
                    os.readlink(root / "previous") if (root / "previous").is_symlink() else None,
                    tree_snapshot((root / "current").resolve()))

        first = make_release("v1", 1)
        install(first)
        assert invoke("verify", "--version", "v1") == first[4]
        highwater = root / ".update-highwater/stable.json"
        first_highwater = {"sequence": 1, "manifest_sha256": sha256(first[2].read_bytes())}
        assert json.loads(highwater.read_bytes()) == first_highwater
        # Freshness state cannot escape confinement through symlinks.
        highwater_directory = highwater.parent
        saved_directory = root / ".saved-highwater"
        highwater_directory.rename(saved_directory)
        outside = workspace / "outside-highwater"
        outside.mkdir()
        highwater_directory.symlink_to(outside, target_is_directory=True)
        try:
            install(first, succeeds=False)
            assert not list(outside.iterdir()), "highwater escaped install root"
        finally:
            highwater_directory.unlink()
            saved_directory.rename(highwater_directory)
        saved_highwater = highwater.read_bytes()
        highwater.unlink()
        outside_file = outside / "stable.json"
        outside_file.write_bytes(saved_highwater)
        highwater.symlink_to(outside_file)
        try:
            install(first, succeeds=False)
            assert outside_file.read_bytes() == saved_highwater
        finally:
            highwater.unlink()
            highwater.write_bytes(saved_highwater)
            highwater.chmod(0o600)
        second = make_release("v2", 2)
        before = active_snapshot()
        fifo = workspace / "untrusted-input-fifo"
        os.mkfifo(fifo)
        for flag in ("--manifest", "--signature", "--package", "--trust"):
            command = [helper, "install", "--manifest", second[2], "--signature", second[3],
                       "--package", second[1], "--trust", public_key, "--root", root]
            command[command.index(flag) + 1] = fifo
            run(command, succeeds=False, timeout=5)
            assert active_snapshot() == before, flag + " FIFO changed active release"
            check_preserved()
        fifo.unlink()
        unsafe_parent = workspace / "unsafe-parent"
        unsafe_parent.mkdir()
        unsafe_parent.chmod(0o777)
        safe_parent = workspace / "safe-parent"
        safe_parent.mkdir(mode=0o700)
        symlink_parent = workspace / "symlink-parent"
        symlink_parent.symlink_to(safe_parent, target_is_directory=True)
        try:
            for rejected_root in (unsafe_parent / "install", symlink_parent / "install"):
                run([helper, "install", "--manifest", second[2], "--signature", second[3],
                     "--package", second[1], "--trust", public_key, "--root", rejected_root],
                    succeeds=False)
                assert not (rejected_root / "current").exists()
                assert not (rejected_root / "current").is_symlink()
                assert active_snapshot() == before, "unsafe root attempt changed active release"
                check_preserved()
        finally:
            unsafe_parent.chmod(0o700)
            symlink_parent.unlink()
        for index, label in ((2, "manifest"), (3, "signature"), (1, "package")):
            path = second[index]
            original = path.read_bytes()
            if label == "manifest":
                path.write_bytes(original + b" ")  # Still JSON, but not the signed bytes.
            else:
                changed = bytearray(original)
                changed[len(changed) // 2] ^= 1
                path.write_bytes(changed)
            install(second, succeeds=False)
            assert active_snapshot() == before, label + " tamper changed active release"
            path.write_bytes(original)

        # Sequence is required inside the authenticated schema, not inferred from version.
        invalid_manifest, invalid_signature = workspace / "invalid.json", workspace / "invalid.sig"
        invalid_documents = []
        missing = dict(second[4])
        missing.pop("sequence")
        invalid_documents.append(missing)
        invalid_documents.append(dict(missing, schema=1))
        invalid_documents.append(dict(second[4], schema=1))
        for value in (0, -1, 1.5, "2", True, 18446744073709551616):
            invalid_documents.append(dict(second[4], sequence=value))
        for document in invalid_documents:
            sign_fixture(invalid_manifest, invalid_signature, private_key, document)
            install((second[0], second[1], invalid_manifest, invalid_signature, document), succeeds=False)
            assert active_snapshot() == before, "invalid signed sequence/schema changed active release"
            assert json.loads(highwater.read_bytes()) == first_highwater
        # A sequence added without re-signing cannot confer freshness.
        invalid_manifest.write_bytes(json.dumps(dict(second[4], sequence=99)).encode("ascii"))
        invalid_signature.write_bytes(second[3].read_bytes())
        install((second[0], second[1], invalid_manifest, invalid_signature, second[4]), succeeds=False)
        assert active_snapshot() == before
        for value in ("0", "-1", "1.5", "18446744073709551616", "+2", "two"):
            command = signing_command(second[0], second[1], "invalid-sequence",
                                      invalid_manifest, invalid_signature, value)
            run(command, succeeds=False)
        command = signing_command(second[0], second[1], "missing-sequence",
                                  invalid_manifest, invalid_signature)
        position = command.index("--sequence")
        del command[position:position + 2]
        run(command, succeeds=False)
        # A tiny signed gzip must not make the tar parser allocate unbounded metadata.
        bomb_directory = workspace / "bad-metadata-bomb"
        bomb_directory.mkdir()
        bomb_package = bomb_directory / "metadata-bomb.tar.gz"
        longname = tarfile.TarInfo("././@LongLink")
        longname.type = tarfile.GNUTYPE_LONGNAME
        longname.size = 65 * 1024 * 1024
        chunk = b"A" * (1024 * 1024)
        with gzip.open(bomb_package, "wb", compresslevel=9) as compressed:
            compressed.write(longname.tobuf(format=tarfile.GNU_FORMAT))
            for _ in range(64):
                compressed.write(chunk)
            compressed.write(chunk[:-1] + b"\x00")
            with gzip.open(second[1], "rb") as benign_tar:
                while data := benign_tar.read(1024 * 1024):
                    compressed.write(data)
        bomb_manifest = bomb_directory / "manifest.json"
        bomb_signature = bomb_directory / "manifest.sig"
        bomb_document = dict(second[4], version="bad-metadata-bomb", package=bomb_package.name,
                             package_size=bomb_package.stat().st_size,
                             package_sha256=sha256(bomb_package.read_bytes()))
        sign_fixture(bomb_manifest, bomb_signature, private_key, bomb_document)
        install((second[0], bomb_package, bomb_manifest, bomb_signature, bomb_document), succeeds=False)
        assert active_snapshot() == before, "signed metadata bomb changed active release"
        assert json.loads(highwater.read_bytes()) == first_highwater, "failed extraction raised highwater"

        for kind in ("traversal", "symlink", "hardlink", "special"):
            directory = workspace / ("bad-" + kind)
            directory.mkdir()
            package = directory / (kind + ".tar.gz")
            member = tarfile.TarInfo("../../escaped-update-file" if kind == "traversal" else "evil-link")
            member.mode = 0o644
            member.size = 4 if kind == "traversal" else 0
            if kind == "symlink":
                member.type = tarfile.SYMTYPE
                member.linkname = "../../escaped-update-file"
            elif kind == "hardlink":
                member.type = tarfile.LNKTYPE
                member.linkname = "bin/ikv"
            elif kind == "special":
                member.type = tarfile.FIFOTYPE
            make_archive(package, second[0], (member, b"evil"))
            manifest, signature = directory / "manifest.json", directory / "manifest.sig"
            document = dict(second[4], version="bad-" + kind, package=package.name,
                            package_size=package.stat().st_size, package_sha256=sha256(package.read_bytes()))
            # Deliberately bypass the signing tool to exercise the helper trust boundary.
            sign_fixture(manifest, signature, private_key, document)
            install((second[0], package, manifest, signature, document), succeeds=False)
            assert active_snapshot() == before, kind + " archive changed active release"
            assert not (root / "escaped-update-file").exists()
            assert not (workspace / "escaped-update-file").exists()
            # The supported publisher must refuse the same malicious archives.
            run(signing_command(second[0], package, "bad-" + kind,
                                directory / "publisher.json", directory / "publisher.sig"), succeeds=False)
            assert not (directory / "publisher.json").exists()
            assert not (directory / "publisher.sig").exists()

        # Publisher rejects mismatched file bytes, permissions, and unsafe payloads.
        mismatch = workspace / "mismatch.tar.gz"
        original_payload = (second[0] / "data.txt").read_bytes()
        (second[0] / "data.txt").write_bytes(b"different bytes")
        make_archive(mismatch, second[0])
        (second[0] / "data.txt").write_bytes(original_payload)
        rejected_manifest, rejected_signature = workspace / "rejected.json", workspace / "rejected.sig"
        run(signing_command(second[0], mismatch, "mismatch", rejected_manifest, rejected_signature), succeeds=False)

        def change_mode(member):
            if member.name == "bin/ikv":
                member.mode = 0o644

        make_archive(mismatch, second[0], change=change_mode)
        run(signing_command(second[0], mismatch, "mismatch", rejected_manifest, rejected_signature), succeeds=False)
        unsafe = second[0] / "unsafe-link"
        unsafe.symlink_to("data.txt")
        run(signing_command(second[0], second[1], "mismatch", rejected_manifest, rejected_signature), succeeds=False)
        unsafe.unlink()
        unsafe = second[0] / "unsafe-fifo"
        os.mkfifo(unsafe)
        run(signing_command(second[0], second[1], "mismatch", rejected_manifest, rejected_signature), succeeds=False)
        unsafe.unlink()
        unsafe = second[0] / "empty-directory"
        unsafe.mkdir()
        run(signing_command(second[0], second[1], "mismatch", rejected_manifest, rejected_signature), succeeds=False)
        unsafe.rmdir()
        unsafe = second[0] / ("a" * 129)
        unsafe.write_bytes(b"component too long")
        run(signing_command(second[0], second[1], "mismatch", rejected_manifest, rejected_signature), succeeds=False)
        unsafe.unlink()
        assert not rejected_manifest.exists() and not rejected_signature.exists()

        install(second)
        assert os.readlink(root / "previous") == "releases/v1"
        assert invoke("verify", "--version", "v2") == second[4]
        # Malformed durable state fails closed rather than resetting freshness.
        saved_highwater = highwater.read_bytes()
        highwater.write_bytes(b'{"sequence":0,"manifest_sha256":"invalid"}')
        try:
            install(second, succeeds=False)
        finally:
            highwater.write_bytes(saved_highwater)
        second_highwater = {"sequence": 2, "manifest_sha256": sha256(second[2].read_bytes())}
        assert json.loads(highwater.read_bytes()) == second_highwater
        before_replay = active_snapshot()
        install(first, succeeds=False)
        assert active_snapshot() == before_replay, "historical signed release replay downgraded current"
        assert json.loads(highwater.read_bytes()) == second_highwater
        before_reinstall = active_snapshot()
        install(second)
        assert active_snapshot() == before_reinstall, "idempotent install changed release pointers/content"
        replacement = make_release("replacement", 2)
        replacement_document = dict(replacement[4], version="v2")
        sign_fixture(replacement[2], replacement[3], private_key, replacement_document)
        install((*replacement[:4], replacement_document), succeeds=False)
        assert active_snapshot() == before_reinstall, "conflicting signed version overwrote retained release"
        # Equal sequence with a different version is rejected even with a valid signature.
        sign_fixture(replacement[2], replacement[3], private_key, replacement[4])
        install(replacement, succeeds=False)
        assert not (root / "releases/replacement").exists()
        assert active_snapshot() == before_reinstall
        assert json.loads(highwater.read_bytes()) == second_highwater
        journal = root / ".update-transaction"
        journal.write_text(json.dumps({"version": "v3", "current": "v2", "previous": "v1"}),
                           encoding="ascii")
        journal.chmod(0o600)
        (root / "previous").unlink()
        (root / "previous").symlink_to("releases/v2")
        assert not (root / "releases/v3").exists()
        assert invoke("verify", "--version", "v2") == second[4]
        assert not journal.exists(), "recovery did not clear transaction journal"
        assert active_snapshot() == before_reinstall, "recovery did not restore both saved pointers"
        assert json.loads(highwater.read_bytes()) == second_highwater, "journal recovery lowered highwater"
        assert invoke("rollback") == first[4]
        assert os.readlink(root / "current") == "releases/v1"
        assert os.readlink(root / "previous") == "releases/v2"
        assert invoke("verify", "--version", "v1") == first[4]
        assert json.loads(highwater.read_bytes()) == second_highwater, "explicit rollback lowered highwater"
        after_rollback = active_snapshot()
        install(first, succeeds=False)
        assert active_snapshot() == after_rollback, "automatic replay bypassed freshness after rollback"
        install(second)
        assert json.loads(highwater.read_bytes()) == second_highwater
        assert invoke("rollback") == first[4]
        # Model a crash after v3's durable floor but before cutover journal removal.
        # Recovery restores old pointers; it must not restore their lower sequence.
        third = make_release("v3", 3)
        recovery_root = workspace / "recovery-installation"

        def recover_invoke(operation, release=None, succeeds=True):
            arguments = []
            if release is not None:
                arguments = ["--manifest", release[2], "--signature", release[3],
                             "--package", release[1]]
            return run([helper, operation, *arguments, "--trust", public_key,
                        "--root", recovery_root], succeeds=succeeds)

        recover_invoke("install", first)
        recover_invoke("install", second)
        recover_invoke("install", third)
        recovery_highwater = recovery_root / ".update-highwater/stable.json"
        third_highwater = recovery_highwater.read_bytes()
        assert json.loads(third_highwater) == {
            "sequence": 3, "manifest_sha256": sha256(third[2].read_bytes())}
        recovery_journal = recovery_root / ".update-transaction"
        recovery_journal.write_text(json.dumps({"version": "v3", "current": "../escape", "previous": "v1"}),
                                    encoding="ascii")
        recovery_journal.chmod(0o600)
        run([helper, "verify", "--version", "v2", "--trust", public_key,
             "--root", recovery_root], succeeds=False)
        assert recovery_highwater.read_bytes() == third_highwater, "journal failure changed highwater"
        assert os.readlink(recovery_root / "current") == "releases/v3"
        recovery_journal.write_text(json.dumps({"version": "v3", "current": "v2", "previous": "v1"}),
                                    encoding="ascii")
        recovery_journal.chmod(0o600)
        run([helper, "verify", "--version", "v2", "--trust", public_key,
             "--root", recovery_root])
        assert not recovery_journal.exists()
        assert os.readlink(recovery_root / "current") == "releases/v2"
        assert os.readlink(recovery_root / "previous") == "releases/v1"
        assert recovery_highwater.read_bytes() == third_highwater
        recover_invoke("install", second, succeeds=False)
        recover_invoke("install", third)
        assert os.readlink(recovery_root / "current") == "releases/v3"
        assert recovery_highwater.read_bytes() == third_highwater
        # Sequences are channel-local and never an ordering of arbitrary version strings.
        experimental_document = dict(first[4], version="z-experimental", channel="experimental")
        sign_fixture(invalid_manifest, invalid_signature, private_key, experimental_document)
        experimental = (first[0], first[1], invalid_manifest, invalid_signature, experimental_document)
        recover_invoke("install", experimental)
        assert os.readlink(recovery_root / "current") == "releases/z-experimental"
        assert json.loads((recovery_root / ".update-highwater/experimental.json").read_bytes()) == {
            "sequence": 1, "manifest_sha256": sha256(invalid_manifest.read_bytes())}
        assert recovery_highwater.read_bytes() == third_highwater
        recover_invoke("install", first, succeeds=False)
        final = make_release("a-final", 18446744073709551615)
        recover_invoke("install", final)
        assert os.readlink(recovery_root / "current") == "releases/a-final"
        assert json.loads(recovery_highwater.read_bytes()) == {
            "sequence": 18446744073709551615, "manifest_sha256": sha256(final[2].read_bytes())}
        recover_invoke("install", third, succeeds=False)
        recover_invoke("install", final)
        before = active_snapshot()
        (root / "releases/v2/data.txt").write_bytes(b"altered retained release")
        invoke("verify", "--version", "v2", succeeds=False)
        invoke("rollback", succeeds=False)
        assert active_snapshot() == before, "rollback accepted altered retained release"
        retained_signature = root / "releases/v1/.ikv-update/signature.bin"
        saved_signature = retained_signature.read_bytes()
        retained_signature.write_bytes(bytes(64))
        invoke("verify", "--version", "v1", succeeds=False)
        retained_signature.write_bytes(saved_signature)
        check_preserved()
        if args.launcher:
            # The launcher offers vX.Y.Z tags only and restarts into the installed AppImage.
            launcher_tls_check(Path(args.launcher).resolve(), helper, workspace, public_key,
                               (make_release("1.0.0", 1, appimage=True), make_release("2.0.0", 2, appimage=True)))
    print("signed updater integration checks passed")


if __name__ == "__main__":
    main()
