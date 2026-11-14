#!/usr/bin/env python3
"""Windows self-update under Wine: the real cross-built ikv-launcher.exe detects a newer signed Windows package on an
owned HTTPS release origin, verifies and installs it (ikv-updater.exe, releases/<version> + current.json) and restarts
into the installed release's bin/ikv-launcher.exe; a tampered manifest is refused; startup hands off to the installed
release.

    python3 tests/windows_update_wine.py --stage build-windows/stage

The installed release is a signed stand-in (a tiny MinGW program at bin/ikv-launcher.exe that records its arguments and
IKV_UPDATE_HANDOFF), so no second Windows build is needed. Runs on an owned Xvfb display in a temporary WINEPREFIX with an
ephemeral Ed25519 key and CA.
"""
import argparse
import gzip
import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

STANDIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
int wmain(int argc, wchar_t** argv) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\'); wcscpy(slash + 1, L"handoff.txt");
    FILE* out = _wfopen(path, L"w");
    const char* handoff = getenv("IKV_UPDATE_HANDOFF");
    fprintf(out, "standin 9.0.0 handoff=%s args=", handoff ? handoff : "unset");
    for (int i = 1; i < argc; ++i) fprintf(out, "%s%ls", i > 1 ? " " : "", argv[i]);
    fprintf(out, "\n"); fclose(out); return 0;
}
"""


def run(command, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run([str(c) for c in command], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs)


def windows_path(path):
    return "Z:" + str(path).replace("/", "\\")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", required=True, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    launcher = args.stage / "bin/ikv-launcher.exe"
    assert launcher.is_file() and (args.stage / "bin/ikv-updater.exe").is_file(), "stage needs ikv-launcher.exe and ikv-updater.exe"
    work = Path(tempfile.mkdtemp(prefix="ikv-wine-update-"))
    xvfb = server = None
    try:
        # Ephemeral release key and HTTPS CA.
        private, public = work / "private.pem", work / "public.pem"
        run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", private])
        run(["openssl", "pkey", "-in", private, "-pubout", "-out", public])
        ca_key, ca, key, cert = work / "ca-key.pem", work / "ca.pem", work / "key.pem", work / "cert.pem"
        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=IKV wine update CA",
             "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
             "-keyout", ca_key, "-out", ca])
        run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost", "-keyout", key, "-out", work / "csr"])
        (work / "ext").write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
                                  "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n"
                                  "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
        run(["openssl", "x509", "-req", "-in", work / "csr", "-CA", ca, "-CAkey", ca_key, "-CAcreateserial", "-days", "1",
             "-sha256", "-extfile", work / "ext", "-out", cert])

        # Signed Windows package 9.0.0: bin/ikv-launcher.exe stand-in plus a data file.
        payload = work / "payload"
        (payload / "bin").mkdir(parents=True)
        (work / "standin.c").write_text(STANDIN)
        run(["x86_64-w64-mingw32-gcc", "-municode", "-O2", "-s", "-o", payload / "bin/ikv-launcher.exe", work / "standin.c"])
        (payload / "README.txt").write_text("IKV 9.0.0 stand-in\n")
        package = work / "ikv-windows-x86_64-9.0.0.tar.gz"
        with package.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed, \
                tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for path in sorted(payload.rglob("*")):
                info = archive.gettarinfo(str(path), arcname=path.relative_to(payload).as_posix())
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                if info.isfile():
                    with path.open("rb") as source:
                        archive.addfile(info, source)
                else:
                    archive.addfile(info)
        manifest, signature = work / "manifest-windows.json", work / "manifest-windows.sig"
        run([sys.executable, Path(__file__).resolve().parents[1] / "tools/sign_manifest.py", "--root", payload, "--package", package,
             "--version", "9.0.0", "--channel", "stable", "--sequence", "1", "--platform", "windows",
             "--output", manifest, "--signature", signature, "--key-file", private])
        assert json.loads(manifest.read_bytes())["platform"] == "windows"

        responses, requests = {}, []
        class Origin(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                body = responses.get(self.path)
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        origin = f"https://localhost:{server.server_port}"
        threading.Thread(target=server.serve_forever, daemon=True).start()

        def publish(tamper=False):
            responses.clear()
            assets = []
            for path in (manifest, signature, package):
                data = path.read_bytes()
                responses["/" + path.name] = data + (b" " if tamper and path == manifest else b"")
                assets.append({"name": path.name, "browser_download_url": origin + "/" + path.name})
            responses["/releases"] = json.dumps([{"tag_name": "v9.0.0", "draft": False, "prerelease": False, "assets": assets}]).encode()

        # Owned display and Wine prefix.
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
                                stdout=subprocess.PIPE, text=True)
        display = ":" + xvfb.stdout.readline().strip()
        env = dict(os.environ, WINEPREFIX=str(work / "prefix"), WINEDEBUG="-all", DISPLAY=display, WINEDLLOVERRIDES="mscoree,mshtml=")
        for name in list(env):
            if name.lower().endswith("_proxy"):
                del env[name]
        run(["wineboot", "-i"], env=env, timeout=300)

        def update(root, succeeds):
            result = run(["wine", launcher, "--update-once", "--install-root", windows_path(root), "--repo", "owned/fixture",
                          "--trust", windows_path(public), "--update-ca", windows_path(ca), "--release-api", origin + "/releases",
                          "--channel", "stable", "--network-timeout-ms", "20000"], env=env, timeout=180, check=False)
            assert (result.returncode == 0) == succeeds, (result.returncode, result.stdout, result.stderr[-4000:])
            return result

        root = work / "install"
        publish()
        result = update(root, True)
        marker = root / "releases/9.0.0/bin/handoff.txt"
        deadline = time.monotonic() + 60
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.5)
        evidence = {"stdout": result.stdout.strip().splitlines()[-2:], "requests": requests[:]}
        assert marker.exists(), ("installed launcher was not started", evidence, result.stderr[-4000:])
        handoff = marker.read_text().strip()
        assert handoff.startswith("standin 9.0.0 handoff=1 args=--update-once"), handoff
        assert json.loads((root / "current.json").read_text()) == {"current": "9.0.0", "previous": ""}
        assert json.loads((root / ".update-highwater/stable.json").read_text())["sequence"] == 1
        assert (root / "releases/9.0.0/README.txt").read_text() == "IKV 9.0.0 stand-in\n"
        evidence["update_restart"] = handoff

        # Startup handoff: the plain launcher (installation/root persisted by --update-once) runs the installed release.
        marker.unlink()
        run(["wine", launcher, "--version"], env=env, timeout=120, check=False)
        deadline = time.monotonic() + 60
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.5)
        assert marker.exists(), "startup handoff did not start the installed release"
        evidence["startup_handoff"] = marker.read_text().strip()
        assert evidence["startup_handoff"] == "standin 9.0.0 handoff=1 args=--version", evidence

        # Tampered manifest: refused, nothing installed.
        tampered = work / "install-tampered"
        publish(tamper=True)
        start = len(requests)
        update(tampered, False)
        assert "/" + package.name not in requests[start:], requests[start:]
        assert not (tampered / "current.json").exists() and not (tampered / "releases/9.0.0").exists()
        evidence["tampered"] = "refused before the package download; no current.json"
        print(json.dumps(evidence, ensure_ascii=False, indent=1))
        print("windows self-update under Wine passed")
    finally:
        if server:
            server.shutdown()
        subprocess.run(["wineserver", "-k"], env=dict(os.environ, WINEPREFIX=str(work / "prefix")), check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if xvfb:
            xvfb.terminate()
            xvfb.wait(timeout=10)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
