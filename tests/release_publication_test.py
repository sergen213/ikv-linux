#!/usr/bin/env python3
"""Real gh/OpenSSL/publisher subprocesses against owned HTTPS + bare-Git fixtures.

Requires python3, openssl, gh and git. Exercises fixture transport, NOT external
GitHub publication or GitHub environment enforcement. No fake gh executable,
production adapter, source-text checks, or external requests are used.
Draft lookup honors GraphQL field projection and authenticated REST release IDs;
the REST tag route exposes published releases only.
"""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import signal
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from urllib.parse import parse_qs, unquote, urlsplit
import unittest

PUBLISHER = Path(__file__).resolve().parents[1] / "tools" / "publish_release.py"
MAX_HTTP_BODY = 16 * 1024 * 1024
MAX_HTTP_LINE = 8192
MAX_HTTP_FRAMING = 65536
MAX_HTTP_TRAILERS = 16384
HTTP_BODY_TIMEOUT = 5
HTTP_TOKEN = rb"[!#$%&'*+\-.^_`|~0-9A-Za-z]+"
HTTP_QUOTED = rb'"(?:[\t\x20-\x21\x23-\x5b\x5d-\x7e\x80-\xff]|\\[\t\x20-\x7e\x80-\xff])*"'
CHUNK_LINE = re.compile(rb"([0-9A-Fa-f]+)(?:[ \t]*;[ \t]*" + HTTP_TOKEN +
                        rb"(?:[ \t]*=[ \t]*(?:" + HTTP_TOKEN + rb"|" + HTTP_QUOTED + rb"))?)*")
TRAILER_LINE = re.compile(HTTP_TOKEN + rb":[\t\x20-\x7e\x80-\xff]*")


def command(arguments, data=None, env=None, check=True):
    arguments = list(map(str, arguments))
    with subprocess.Popen(arguments, stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, start_new_session=True) as process:
        try:
            stdout, stderr = process.communicate(data, timeout=120)
        except subprocess.TimeoutExpired:
            # Kill the owned group, including a publisher's active gh child, then reap it.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate(timeout=5)
            raise
        result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
        if check:
            result.check_returncode()
        return result


class OwnedGitHub:
    """HTTPS fixture with durable asset bytes and native Git atomic branch CAS."""
    def __init__(self, root):
        self.root = root
        self.git_dir = root / "repository.git"
        self.assets_dir = root / "assets"
        self.assets_dir.mkdir()
        command(["git", "init", "--bare", self.git_dir])
        self.git_env = dict(os.environ, GIT_AUTHOR_NAME="Owned fixture", GIT_AUTHOR_EMAIL="fixture@localhost",
                            GIT_COMMITTER_NAME="Owned fixture", GIT_COMMITTER_EMAIL="fixture@localhost")
        self.environment = {"name": "native-release-production", "protection_rules": [
            {"type": "required_reviewers", "reviewers": [{"type": "User", "reviewer": {"id": 1, "login": "fixture-reviewer"}}]}]}
        self.seed = {channel: {"schema": 1, "channel": channel, "sequence": 0} for channel in ("stable", "experimental")}
        self.target = self.commit_ledger(self.seed)
        self.git("update-ref", "refs/heads/release-ledger", self.target)
        self.git("update-ref", "refs/heads/main", self.target)
        self.git("symbolic-ref", "HEAD", "refs/heads/main")
        self.other_target = self.commit_ledger(self.seed, self.target)
        self.release = None
        self.requests = []
        self.writes = []
        self.lock = threading.RLock()
        self.cas_race = False
        self.fail_upload = None
        self.corrupt_download = False
        self.advance_after_download = False
        self.ledger_read_before_download = False
        self.tag_conflict_on_create = False
        self.advance_tip_after_upload = False
        self.before_final_verifications = set()
        self.verified_at_publish = set()
        certificate, key = root / "localhost.pem", root / "localhost.key"
        command(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                 "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", key, "-out", certificate])
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.dispatch("GET")

            def do_POST(self):
                self.dispatch("POST")

            def do_PATCH(self):
                self.dispatch("PATCH")

            def do_PUT(self):
                self.dispatch("PUT")

            def read_body(self):
                deadline = time.monotonic() + HTTP_BODY_TIMEOUT
                framing = 0

                def exact(size, body=False):
                    data = bytearray()
                    while len(data) < size:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise TimeoutError("HTTP body deadline exceeded")
                        self.connection.settimeout(remaining)
                        # read1 makes at most one socket read, so continuous traffic
                        # cannot reset an inactivity timeout indefinitely inside read().
                        part = self.rfile.read1(min(size - len(data), 65536))
                        if not part:
                            raise ValueError("premature HTTP body EOF")
                        data.extend(part)
                        if body:
                            self.body_length += len(part)
                    return bytes(data)

                def line():
                    nonlocal framing
                    data = bytearray()
                    while len(data) < MAX_HTTP_LINE:
                        data.extend(exact(1))
                        framing += 1
                        if framing > MAX_HTTP_FRAMING:
                            raise ValueError("HTTP framing exceeds limit")
                        if data[-1] == 10:
                            if not data.endswith(b"\r\n"):
                                raise ValueError("HTTP line lacks CRLF")
                            return bytes(data[:-2])
                    raise ValueError("HTTP line exceeds limit")

                lengths = self.headers.get_all("Content-Length", [])
                encodings = self.headers.get_all("Transfer-Encoding", [])
                if (lengths and encodings) or len(lengths) > 1 or len(encodings) > 1:
                    raise ValueError("ambiguous HTTP body framing")
                if not encodings:
                    value = lengths[0].strip() if lengths else "0"
                    if len(value) > 20 or re.fullmatch(r"[0-9]+", value) is None:
                        raise ValueError("invalid HTTP Content-Length")
                    size = int(value)
                    if size > MAX_HTTP_BODY:
                        raise ValueError("HTTP body exceeds limit")
                    return exact(size, body=True)
                if encodings[0].strip().lower() != "chunked" or self.request_version != "HTTP/1.1":
                    raise ValueError("unsupported HTTP Transfer-Encoding")
                data = bytearray()
                while True:
                    match = CHUNK_LINE.fullmatch(line())
                    if match is None:
                        raise ValueError("invalid HTTP chunk size or extension")
                    size = int(match[1], 16)
                    if size > MAX_HTTP_BODY - len(data):
                        raise ValueError("HTTP body exceeds limit")
                    if size == 0:
                        trailers = 0
                        while True:
                            trailer = line()
                            trailers += len(trailer) + 2
                            if trailers > MAX_HTTP_TRAILERS:
                                raise ValueError("HTTP trailers exceed limit")
                            if not trailer:
                                return bytes(data)
                            if (TRAILER_LINE.fullmatch(trailer) is None or
                                    trailer.split(b":", 1)[0].lower() in (b"content-length", b"transfer-encoding")):
                                raise ValueError("invalid HTTP trailer")
                    data.extend(exact(size, body=True))
                    if exact(2) != b"\r\n":
                        raise ValueError("HTTP chunk lacks CRLF")
                    framing += 2
                    if framing > MAX_HTTP_FRAMING:
                        raise ValueError("HTTP framing exceeds limit")

            def failure(self, label, method, path):
                # Only framing metadata; never exception text, Authorization or body bytes.
                return {"message": (f"owned fixture {label}: method={method} path={path} "
                                    f"CL={self.headers.get_all('Content-Length', [])!r} "
                                    f"TE={self.headers.get_all('Transfer-Encoding', [])!r} "
                                    f"bodylen={self.body_length}")}

            def dispatch(self, method):
                self.body_length = 0
                parsed = urlsplit(self.path)
                path = unquote(parsed.path)
                if path.startswith("/api/v3/"):
                    path = path[7:]
                try:
                    data = self.read_body()
                except (ValueError, TimeoutError, OSError):
                    status, value = 400, self.failure("framing failure", method, path)
                else:
                    try:
                        with fixture.lock:
                            fixture.requests.append((method, path))
                            status, value = fixture.handle(method, path, parse_qs(parsed.query), data, self.headers)
                    except Exception:
                        status, value = 500, self.failure("failure", method, path)
                self.connection.settimeout(5)
                self.close_connection = True
                raw = value if isinstance(value, bytes) else json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/octet-stream" if isinstance(value, bytes) else "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)

        class HTTPSFixtureServer(ThreadingHTTPServer):
            daemon_threads = True

            def get_request(self):
                connection, address = super().get_request()
                # Bound TLS accept as well as subsequent request reads/writes.
                connection.settimeout(5)
                try:
                    return context.wrap_socket(connection, server_side=True), address
                except Exception:
                    connection.close()
                    raise

        self.server = HTTPSFixtureServer(("127.0.0.1", 0), Handler)
        self.host = "localhost:" + str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client_env = dict(os.environ, GH_TOKEN="owned-fixture-only-token", GH_ENTERPRISE_TOKEN="owned-fixture-only-token",
                               GH_HOST=self.host, SSL_CERT_FILE=str(certificate), GH_PROMPT_DISABLED="1",
                               GH_CONFIG_DIR=str(root / "gh-config"), NO_PROXY="localhost,127.0.0.1")
        for key in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY", "https_proxy", "http_proxy", "all_proxy"):
            self.client_env.pop(key, None)

    def close(self):
        shutdown = threading.Thread(target=self.server.shutdown, daemon=True)
        shutdown.start()
        shutdown.join(timeout=5)
        exceeded_deadline = shutdown.is_alive()
        self.server.server_close()
        self.thread.join(timeout=5)
        shutdown.join(timeout=1)
        if exceeded_deadline or shutdown.is_alive() or self.thread.is_alive():
            raise RuntimeError("owned HTTPS server exceeded its shutdown deadline")

    def git(self, *arguments, data=None, check=True):
        return command(["git", "--git-dir", self.git_dir, *arguments], data=data, env=self.git_env, check=check)

    def commit_ledger(self, ledgers, parent=None):
        members = []
        for channel, ledger in sorted(ledgers.items()):
            blob = self.git("hash-object", "-w", "--stdin", data=json.dumps(ledger, sort_keys=True).encode()).stdout.strip()
            members.append(b"100644 blob " + blob + b"\t" + channel.encode() + b".json\n")
        subtree = self.git("mktree", data=b"".join(members)).stdout.strip()
        tree = self.git("mktree", data=b"040000 tree " + subtree + b"\t.ikv-release-ledger\n").stdout.strip().decode()
        args = ["commit-tree", tree]
        if parent:
            args += ["-p", parent]
        return self.git(*args, data=b"Owned authenticated issuance fixture\n").stdout.strip().decode()

    def ledgers(self):
        return {channel: json.loads(self.git("show", "release-ledger:.ikv-release-ledger/" + channel + ".json").stdout)
                for channel in ("stable", "experimental")}

    def replace_ledger(self, channel, ledger):
        ledgers = self.ledgers()
        ledgers[channel] = ledger
        old = self.git("rev-parse", "refs/heads/release-ledger").stdout.strip().decode()
        new = self.commit_ledger(ledgers, old)
        self.git("update-ref", "refs/heads/release-ledger", new, old)

    def persist_release(self):
        (self.root / "release.json").write_text(json.dumps(self.release, sort_keys=True))

    def handle(self, method, path, query, raw, headers):
        if headers.get("Authorization") not in ("token owned-fixture-only-token", "Bearer owned-fixture-only-token"):
            return 401, {"message": "fixture authentication required"}
        prefix = "/repos/owner/repo"
        if path in ("/graphql", "/api/graphql") and method == "POST":
            body = json.loads(raw)
            if "mutation" in body.get("query", ""):
                return 422, {"message": "fixture does not allow GraphQL mutations"}
            variables = body.get("variables", {})
            # gh's draft lookup selects scalar fields, not a release connection.
            # Return exactly that selection: githubv4 rejects unsolicited fields.
            gql = body.get("query", "")
            repository = re.search(r"(?:(\w+)\s*:\s*)?\brepository\s*\([^)]*\)\s*\{", gql)
            selection = re.search(r"(?:(\w+)\s*:\s*)?\brelease\s*\(\s*tagName\s*:\s*\$(\w+)\s*\)\s*\{([^{}]*)\}", gql)
            if repository is None or selection is None:
                return 422, {"message": "unsupported owned fixture GraphQL selection"}
            fields = re.findall(r"(?:(\w+)\s*:\s*)?(\w+)", selection[3])
            if not fields or any(field not in ("databaseId", "isDraft") for _, field in fields):
                return 422, {"message": "unsupported owned fixture GraphQL release field"}
            pending_tag = variables.get(selection[2])
            release = None
            if self.release is not None and self.release["tag_name"] == pending_tag:
                values = {"databaseId": self.release["id"], "isDraft": self.release["draft"]}
                release = {alias or field: values[field] for alias, field in fields}
            return 200, {"data": {repository[1] or "repository": {selection[1] or "release": release}}}
        if method == "GET" and path == prefix:
            return 200, {"id": 1, "name": "repo", "full_name": "owner/repo", "owner": {"login": "owner"},
                         "default_branch": "main", "permissions": {"push": True}, "private": True}
        if method == "GET" and path == prefix + "/environments/native-release-production":
            return 200, self.environment
        if method == "GET" and path.startswith(prefix + "/git/ref/"):
            ref = "refs/" + path.split("/git/ref/", 1)[1]
            result = self.git("rev-parse", "--verify", ref, check=False)
            if result.returncode:
                return 404, {"message": "Not Found"}
            sha = result.stdout.strip().decode()
            kind = self.git("cat-file", "-t", sha).stdout.strip().decode()
            return 200, {"ref": ref, "object": {"type": kind, "sha": sha}}
        if path.startswith(prefix + "/contents/"):
            name = path.split("/contents/", 1)[1]
            branch = query.get("ref", ["release-ledger"])[0]
            if method == "GET":
                content = self.git("show", branch + ":" + name, check=False)
                if content.returncode:
                    return 404, {"message": "Not Found"}
                sha = self.git("rev-parse", branch + ":" + name).stdout.strip().decode()
                if self.advance_after_download:
                    self.ledger_read_before_download = True
                return 200, {"type": "file", "encoding": "base64", "sha": sha,
                             "content": base64.b64encode(content.stdout).decode(), "path": name}
            if method == "PUT":
                self.writes.append((method, path))
                payload = json.loads(raw)
                if payload["branch"] != "release-ledger":
                    return 422, {"message": "unknown fixture branch"}
                old = self.git("rev-parse", "refs/heads/release-ledger", check=False)
                if old.returncode:
                    return 404, {"message": "branch must already exist"}
                old = old.stdout.strip().decode()
                current_blob = self.git("rev-parse", "release-ledger:" + name, check=False)
                if current_blob.returncode or current_blob.stdout.strip().decode() != payload["sha"]:
                    return 409, {"message": "Contents SHA mismatch"}
                ledgers = self.ledgers()
                channel = Path(name).stem
                ledgers[channel] = json.loads(base64.b64decode(payload["content"]))
                new = self.commit_ledger(ledgers, old)
                if self.cas_race:
                    # Real competing native branch update; update-ref expected-old must lose.
                    competing = self.commit_ledger(self.ledgers(), old)
                    self.git("update-ref", "refs/heads/release-ledger", competing, old)
                result = self.git("update-ref", "refs/heads/release-ledger", new, old, check=False)
                if result.returncode:
                    return 409, {"message": "native atomic branch CAS lost"}
                sha = self.git("rev-parse", "release-ledger:" + name).stdout.strip().decode()
                return 200, {"content": {"sha": sha}, "commit": {"sha": new}}
        if path == prefix + "/releases" and method == "POST":
            self.writes.append((method, path))
            if self.release is not None:
                return 422, {"message": "release already exists"}
            payload = json.loads(raw)
            api_url = "https://" + self.host + "/api/v3" + prefix + "/releases/7"
            created = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            self.release = dict(payload, id=7, node_id="RE_fixture_7", assets=[],
                                url=api_url, assets_url=api_url + "/assets",
                                html_url="https://" + self.host + "/owner/repo/releases/tag/" + payload["tag_name"],
                                upload_url="https://" + self.host + "/uploads/repos/owner/repo/releases/7/assets{?name,label}",
                                tarball_url=None, zipball_url=None, immutable=False,
                                created_at=created, updated_at=created, published_at=None,
                                author={"id": 1, "node_id": "U_fixture", "login": "fixture"})
            self.release.setdefault("draft", False)
            self.release.setdefault("prerelease", False)
            self.release.setdefault("name", None)
            self.release.setdefault("body", "")
            self.release.setdefault("target_commitish", self.git("rev-parse", "HEAD").stdout.strip().decode())
            if self.tag_conflict_on_create:
                self.git("update-ref", "refs/tags/" + payload["tag_name"], self.other_target, "0" * 40)
            self.persist_release()
            return 201, self.release
        # GitHub's tag route is published-only; authenticated drafts remain visible by ID.
        if (path.startswith(prefix + "/releases/tags/") or path == prefix + "/releases/7") and method == "GET":
            if self.release is None or ("/tags/" in path and (self.release["draft"] or path.rsplit("/", 1)[1] != self.release["tag_name"])):
                return 404, {"message": "Not Found"}
            return 200, self.release
        if path == "/uploads/repos/owner/repo/releases/7/assets" and method == "POST":
            self.writes.append((method, path))
            name = query["name"][0]
            if name == self.fail_upload:
                return 500, {"message": "owned interrupted upload"}
            if any(asset["name"] == name for asset in self.release["assets"]):
                return 422, {"message": "asset already exists"}
            destination = self.assets_dir / name
            if destination.parent != self.assets_dir:
                return 422, {"message": "unsafe fixture asset"}
            destination.write_bytes(raw)
            asset_id = 100 + len(self.release["assets"])
            created = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            asset = {"id": asset_id, "node_id": "RA_fixture_" + str(asset_id),
                     "name": name, "label": query.get("label", [None])[0], "size": len(raw), "state": "uploaded",
                     "content_type": headers.get("Content-Type", "application/octet-stream"),
                     "digest": "sha256:" + hashlib.sha256(raw).hexdigest(), "download_count": 0,
                     "created_at": created, "updated_at": created, "uploader": self.release["author"],
                     "browser_download_url": "https://" + self.host + "/owner/repo/releases/download/" + self.release["tag_name"] + "/" + name,
                     "url": "https://" + self.host + "/api/v3" + prefix + "/releases/assets/" + str(asset_id)}
            self.release["assets"].append(asset)
            if self.advance_tip_after_upload:
                self.advance_tip_after_upload = False
                self.git("update-ref", "refs/heads/main", self.other_target, self.target)
            self.persist_release()
            return 201, asset
        if path.startswith(prefix + "/releases/assets/") and method == "GET":
            asset_id = int(path.rsplit("/", 1)[1])
            asset = next(a for a in self.release["assets"] if a["id"] == asset_id)
            if len(self.release["assets"]) == 3:
                self.before_final_verifications.add(asset["name"])
            data = (self.assets_dir / asset["name"]).read_bytes()
            if self.advance_after_download and self.ledger_read_before_download:
                # A real competing issuer advances native Git CAS after this retry
                # checked its floor, even when every asset was already uploaded.
                self.advance_after_download = False
                ledger = self.ledgers()["stable"]
                ledger["sequence"] += 1
                self.replace_ledger("stable", ledger)
            return 200, data + (b"tampered" if self.corrupt_download else b"")
        if path == prefix + "/releases/7" and method == "PATCH":
            self.writes.append((method, path))
            payload = json.loads(raw)
            if payload.get("draft") is False:
                self.verified_at_publish = set(self.before_final_verifications)
                tag = "refs/tags/" + self.release["tag_name"]
                existing = self.git("rev-parse", "--verify", tag, check=False)
                if existing.returncode:
                    self.git("update-ref", tag, self.release["target_commitish"], "0" * 40)
            self.release.update(payload)
            self.release["updated_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            if not self.release["draft"] and self.release["published_at"] is None:
                self.release["published_at"] = self.release["updated_at"]
                self.release["tarball_url"] = "https://" + self.host + "/api/v3" + prefix + "/tarball/" + self.release["tag_name"]
                self.release["zipball_url"] = "https://" + self.host + "/api/v3" + prefix + "/zipball/" + self.release["tag_name"]
            self.persist_release()
            return 200, self.release
        return 404, {"message": "unknown owned fixture endpoint: " + method + " " + path}


class PublicationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ikv-publication-test-")
        self.root = Path(self.tmp.name)
        self.fixture = OwnedGitHub(self.root)
        self.key, self.trust = self.root / "test-private.pem", self.root / "trust.pem"
        command(["openssl", "genpkey", "-algorithm", "Ed25519", "-out", self.key])
        command(["openssl", "pkey", "-in", self.key, "-pubout", "-out", self.trust])
        self.package = self.root / "native.tar.gz"
        self.payload = b"owned fixture executable\n"
        self.make_package()
        self.manifest_path, self.signature = self.root / "manifest.json", self.root / "manifest.sig"
        self.manifest = {"schema": 2, "sequence": 1, "channel": "stable", "version": "1.2.3",
                         "package": self.package.name, "package_size": self.package.stat().st_size,
                         "package_sha256": hashlib.sha256(self.package.read_bytes()).hexdigest(),
                         "files": [{"path": "bin/client", "size": len(self.payload),
                                    "sha256": hashlib.sha256(self.payload).hexdigest(), "executable": True}]}
        self.sign()
        self.target = self.fixture.target

    def tearDown(self):
        self.fixture.close()
        self.tmp.cleanup()

    def make_package(self, name="bin/client", kind=None, mode=0o755):
        with tarfile.open(self.package, "w:gz") as archive:
            entry = tarfile.TarInfo(name)
            entry.mode, entry.size = mode, len(self.payload)
            if kind:
                entry.type, entry.linkname, entry.size = kind, "bin/client", 0
                archive.addfile(entry)
            else:
                archive.addfile(entry, io.BytesIO(self.payload))

    def sign(self, suffix=b""):
        self.manifest_path.write_bytes(json.dumps(self.manifest, separators=(",", ":")).encode() + suffix)
        command(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", self.key,
                 "-in", self.manifest_path, "-out", self.signature])

    def run_publisher(self, publish=True):
        args = [sys.executable, PUBLISHER, "--repo", "owner/repo", "--host", self.fixture.host,
                "--manifest", self.manifest_path, "--signature", self.signature, "--package", self.package,
                "--trust", self.trust, "--ledger-ref", "release-ledger", "--target", self.target]
        if publish:
            args += ["--publish"]
        return command(args, env=self.fixture.client_env, check=False)

    def succeed(self, publish=True):
        result = self.run_publisher(publish)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result

    def reject(self, no_writes=True, no_network=False):
        writes, requests = len(self.fixture.writes), len(self.fixture.requests)
        result = self.run_publisher()
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        if no_writes:
            self.assertEqual(len(self.fixture.writes), writes, result.stderr.decode())
        if no_network:
            self.assertEqual(len(self.fixture.requests), requests, result.stderr.decode())
        return result

    def test_real_gh_draft_upload_download_verify_edit(self):
        self.succeed()
        release = json.loads((self.root / "release.json").read_text())
        self.assertFalse(release["draft"])
        self.assertFalse(release["prerelease"])
        self.assertEqual(self.fixture.ledgers()["stable"]["sequence"], 1)
        self.assertEqual(self.fixture.writes[0][0], "PUT")
        self.assertEqual(self.fixture.writes[-1][0], "PATCH")
        self.assertEqual(self.fixture.verified_at_publish, {"manifest.json", "manifest.sig", "native.tar.gz"})
        for source in (self.manifest_path, self.signature, self.package):
            self.assertEqual((self.fixture.assets_dir / source.name).read_bytes(), source.read_bytes())
        actual = self.fixture.git("rev-parse", "refs/tags/native-stable-1.2.3").stdout.strip().decode()
        self.assertEqual(actual, self.target)

    def test_target_must_be_current_native_default_branch_tip_before_writes(self):
        self.target = self.fixture.other_target
        ledger_head = self.fixture.git("rev-parse", "refs/heads/release-ledger").stdout
        ledgers = self.fixture.ledgers()
        result = self.reject()
        self.assertIn("current default-branch HEAD", result.stderr.decode())
        self.target = self.fixture.target
        self.fixture.git("update-ref", "refs/heads/main", self.fixture.other_target, self.target)
        self.reject()
        self.fixture.git("update-ref", "-d", "refs/heads/main")
        self.reject()
        self.assertIsNone(self.fixture.release)
        self.assertEqual(self.fixture.git("rev-parse", "refs/heads/release-ledger").stdout, ledger_head)
        self.assertEqual(self.fixture.ledgers(), ledgers)
        self.assertEqual(list(self.fixture.assets_dir.iterdir()), [])
        self.assertEqual(self.fixture.writes, [])

    def test_default_tip_change_during_upload_prevents_remaining_writes(self):
        self.fixture.advance_tip_after_upload = True
        result = self.reject(no_writes=False)
        self.assertIn("rerun the approved workflow", result.stderr.decode())
        self.assertTrue(self.fixture.release["draft"])
        self.assertEqual(len(self.fixture.release["assets"]), 1)
        self.assertFalse(any(method == "PATCH" for method, _ in self.fixture.writes))

    def test_validation_only_has_no_http_requests(self):
        self.succeed(publish=False)
        self.assertEqual(self.fixture.requests, [])
        self.assertEqual(self.fixture.writes, [])

    def test_tampered_inputs_never_reach_network(self):
        for path in (self.manifest_path, self.signature, self.package):
            original = path.read_bytes()
            path.write_bytes(original + b"x")
            self.reject(no_network=True)
            path.write_bytes(original)
        signature = self.signature.read_bytes()
        self.signature.write_bytes(bytes([signature[0] ^ 1]) + signature[1:])
        self.reject(no_network=True)

    def test_wrong_trusted_ed25519_key_and_invalid_git_version(self):
        for version in ("1..2", "1.", "1.lock"):
            self.manifest["version"] = version
            self.sign()
            self.reject(no_network=True)
        self.manifest["version"] = "1.2.3"
        self.sign()
        wrong_key = self.root / "wrong-test-private.pem"
        command(["openssl", "genpkey", "-algorithm", "Ed25519", "-out", wrong_key])
        command(["openssl", "pkey", "-in", wrong_key, "-pubout", "-out", self.trust])
        self.reject(no_network=True)

    def test_strict_schema_sequence_bounds_and_duplicate_keys(self):
        for field, value in (("schema", True), ("schema", 2.0), ("sequence", True), ("sequence", 0),
                             ("sequence", -1), ("sequence", 2**64)):
            original = self.manifest[field]
            self.manifest[field] = value
            self.sign()
            self.reject(no_network=True)
            self.manifest[field] = original
        self.sign()
        self.manifest_path.write_bytes(self.manifest_path.read_bytes().replace(b'"schema":2', b'"schema":2,"schema":2'))
        command(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", self.key,
                 "-in", self.manifest_path, "-out", self.signature])
        self.reject(no_network=True)

    def test_signed_unsafe_archive_and_mode_mismatch(self):
        for name, kind, mode in (("../client", None, 0o755), ("bin/client", tarfile.SYMTYPE, 0o755),
                                 ("bin/client", None, 0o644), ("unsigned", None, 0o755)):
            self.make_package(name, kind, mode)
            self.manifest["package_size"] = self.package.stat().st_size
            self.manifest["package_sha256"] = hashlib.sha256(self.package.read_bytes()).hexdigest()
            self.sign()
            self.reject(no_network=True)

    def test_required_reviewers_missing_fails_before_any_write(self):
        for rules in ([], [{"type": "required_reviewers", "reviewers": []}], [{"type": "wait_timer", "wait_timer": 1}]):
            self.fixture.environment["protection_rules"] = rules
            self.reject()

    def test_missing_invalid_ledger_and_missing_branch_fail_closed(self):
        for ledger in ({}, {"schema": 1, "channel": "experimental", "sequence": 0},
                       {"schema": 1, "channel": "stable", "sequence": True},
                       {"schema": 1, "channel": "stable", "sequence": 1}):
            self.fixture.replace_ledger("stable", ledger)
            self.reject()
        old = self.fixture.git("rev-parse", "refs/heads/release-ledger").stdout.strip().decode()
        new = self.fixture.commit_ledger({"experimental": self.fixture.seed["experimental"]}, old)
        self.fixture.git("update-ref", "refs/heads/release-ledger", new, old)
        self.reject()
        self.fixture.git("update-ref", "-d", "refs/heads/release-ledger")
        self.reject()

    def test_native_atomic_cas_conflict_blocks_release_creation(self):
        self.fixture.cas_race = True
        self.reject(no_writes=False)
        self.assertEqual([method for method, _ in self.fixture.writes], ["PUT"])
        self.assertIsNone(self.fixture.release)
        self.assertEqual(self.fixture.ledgers()["stable"]["sequence"], 0)

    def test_exact_retry_no_writes_changed_signed_bytes_or_target_rejected(self):
        self.succeed()
        count = len(self.fixture.writes)
        self.succeed()
        self.assertEqual(len(self.fixture.writes), count)
        self.sign(b"\n")
        self.reject()
        self.sign()
        self.target = self.fixture.other_target
        self.reject()

    def test_lower_sequence_and_superseded_reservation_rejected(self):
        self.succeed()
        ledger = self.fixture.ledgers()["stable"]
        ledger["sequence"] = 2
        self.fixture.replace_ledger("stable", ledger)
        self.reject()

    def test_partial_upload_recovers_only_missing_assets(self):
        self.fixture.fail_upload = "manifest.sig"
        self.reject(no_writes=False)
        self.assertTrue(self.fixture.release["draft"])
        self.assertEqual([a["name"] for a in self.fixture.release["assets"]], ["manifest.json"])
        self.fixture.fail_upload = None
        self.succeed()
        self.assertFalse(self.fixture.release["draft"])
        self.assertEqual(len(self.fixture.release["assets"]), 3)

    def test_asset_and_immutable_release_conflict_never_clobbered(self):
        self.succeed()
        path = self.fixture.assets_dir / "manifest.sig"
        path.write_bytes(b"x" * 64)
        self.reject()
        path.write_bytes(self.signature.read_bytes())
        self.fixture.release["body"] = "unowned release"
        self.fixture.persist_release()
        self.reject()

    def test_actual_conflicting_git_tag_rejected(self):
        self.fixture.git("update-ref", "refs/tags/native-stable-1.2.3", self.fixture.other_target, "0" * 40)
        self.reject()
        self.assertIsNone(self.fixture.release)

    def test_tag_changed_during_draft_creation_blocks_asset_uploads(self):
        self.fixture.tag_conflict_on_create = True
        self.reject(no_writes=False)
        self.assertTrue(self.fixture.release["draft"])
        self.assertEqual(self.fixture.release["assets"], [])
        self.assertFalse(any(method == "PATCH" for method, _ in self.fixture.writes))

    def test_server_download_tamper_and_superseded_floor_prevent_publish(self):
        self.fixture.corrupt_download = True
        self.reject(no_writes=False)
        self.assertTrue(self.fixture.release["draft"])
        self.assertFalse(any(method == "PATCH" for method, _ in self.fixture.writes))
        self.fixture.corrupt_download = False
        assets = {path.name: path.read_bytes() for path in self.fixture.assets_dir.iterdir()}
        ledger_head = self.fixture.git("rev-parse", "refs/heads/release-ledger").stdout
        self.assertEqual(len(assets), 3)
        self.fixture.advance_after_download = True
        result = self.reject()
        self.assertIn("reservation was superseded", result.stderr.decode())
        self.assertFalse(self.fixture.advance_after_download)
        self.assertNotEqual(self.fixture.git("rev-parse", "refs/heads/release-ledger").stdout, ledger_head)
        self.assertEqual(self.fixture.ledgers()["stable"]["sequence"], 2)
        self.assertEqual({path.name: path.read_bytes() for path in self.fixture.assets_dir.iterdir()}, assets)
        self.assertTrue(self.fixture.release["draft"])
        self.assertFalse(any(method == "PATCH" for method, _ in self.fixture.writes))

    def test_experimental_is_prerelease_and_independent_ledger(self):
        self.manifest["channel"] = "experimental"
        self.sign()
        self.succeed()
        self.assertTrue(self.fixture.release["prerelease"])
        self.assertEqual(self.fixture.release["tag_name"], "native-experimental-1.2.3")
        self.assertEqual(self.fixture.ledgers()["stable"]["sequence"], 0)
        self.assertEqual(self.fixture.ledgers()["experimental"]["sequence"], 1)

    def test_non_ed25519_trust_is_rejected_locally(self):
        command(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", self.key])
        command(["openssl", "pkey", "-in", self.key, "-pubout", "-out", self.trust])
        self.reject(no_network=True)

    def test_publication_requires_gh_token_even_with_cached_credentials(self):
        self.fixture.client_env.pop("GH_TOKEN")
        self.reject(no_network=True)


if __name__ == "__main__":
    unittest.main()
