#!/usr/bin/env python3
"""A newer server refuses this client as outdated: the client shows the 'Yeni sürüm hazır' notice naming the server's
version and exits 75 (the launcher then installs and restarts into the new release).

An owned TLS world endpoint (ephemeral CA) reads the client's hello and answers exactly what a protocol-3 server sends a
legacy client: a version-2 goodbye {"reason", "code":"outdated_client", "version", "protocol":3}, then closes.
"""
import argparse
import json
import socket
import ssl
import subprocess
import tempfile
import threading
from pathlib import Path


def run(command):
    subprocess.run([str(part) for part in command], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ikv-client-outdated-") as name:
        work = Path(name)
        ca_key, ca, key, cert = work / "ca-key.pem", work / "ca.pem", work / "key.pem", work / "cert.pem"
        run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "1",
             "-subj", "/CN=IKV disposable world CA", "-keyout", ca_key, "-out", ca])
        run(["openssl", "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
             "-subj", "/CN=localhost", "-keyout", key, "-out", work / "csr.pem"])
        (work / "ext").write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n"
                                  "extendedKeyUsage=serverAuth\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n")
        run(["openssl", "x509", "-req", "-in", work / "csr.pem", "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
             "-days", "1", "-sha256", "-extfile", work / "ext", "-out", cert])
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert, key)
        listener = socket.create_server(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        body = json.dumps({"reason": "Sunucu sürümü v9.9.9; istemciniz eski. Yeni sürüm hazır; oyun güncelleniyor.",
                           "code": "outdated_client", "version": "9.9.9", "protocol": 3}).encode()
        goodbye = (len(body) + 2).to_bytes(4, "big") + bytes([2, 5]) + body  # frame: length, version 2, Type::goodbye
        seen = {}

        def serve():
            connection, _ = listener.accept()
            with context.wrap_socket(connection, server_side=True) as tls:
                def exactly(count):
                    data = b""
                    while len(data) < count:
                        chunk = tls.recv(count - len(data))
                        assert chunk, "client closed before its hello"
                        data += chunk
                    return data
                hello = exactly(int.from_bytes(exactly(4), "big"))
                seen["hello"] = (hello[0], hello[1], len(hello))
                tls.sendall(goodbye)
                try:
                    tls.unwrap()
                except (ssl.SSLError, OSError):
                    pass  # the refused client may close first

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        result = subprocess.run([args.client, "--headless", "--host", "localhost", "--port", str(port), "--ca", str(ca),
                                 "--session-stdin", "--character", "1", "--ticks", "1"],
                                input="ab" * 32 + "\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        thread.join(timeout=10)
        assert seen.get("hello", (0, 1))[1] == 1, seen  # Type::hello
        assert result.returncode == 75, (result.returncode, result.stdout, result.stderr)
        assert "Yeni sürüm hazır (v9.9.9); oyun güncelleniyor." in result.stderr, result.stderr
    print("client outdated-refusal check passed")


if __name__ == "__main__":
    main()
