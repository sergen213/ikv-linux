#!/usr/bin/env python3
"""Real local OpenSSL provisioning, ownership/current-user mode and refusal checks."""
import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ikv-secret-check-") as root:
        target = Path(root) / "secrets"
        command = ["python3", args.prepare, "--directory", str(target), "--ownership", "current"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        assert result.returncode == 0, "real development provisioning failed"
        app = (target / "app-db-password").read_text().strip()
        admin = (target / "db-password").read_text().strip()
        assert len(app) == len(admin) == 64 and app != admin
        assert all(c in "0123456789abcdef" for c in app + admin)
        assert app not in result.stdout + result.stderr and admin not in result.stdout + result.stderr
        assert "password=" + app in (target / "dsn").read_text()
        for item in target.iterdir():
            assert stat.S_IMODE(item.stat().st_mode) == 0o600
            assert item.stat().st_uid == os.geteuid()
        assert stat.S_IMODE(target.stat().st_mode) == 0o700
        client_ca = target / "client-ca.pem"
        assert client_ca.read_bytes() == (target / "tls-ca").read_bytes()
        assert b"PRIVATE KEY" not in client_ca.read_bytes()
        verify = subprocess.run(["openssl", "verify", "-CAfile", str(client_ca),
                                 "-verify_hostname", "localhost", str(target / "tls-cert")],
                                capture_output=True, timeout=10)
        assert verify.returncode == 0, "generated service certificate does not verify DNS SAN"
        verify_ip = subprocess.run(["openssl", "verify", "-CAfile", str(target / "tls-ca"),
                                    "-verify_ip", "127.0.0.1", str(target / "tls-cert")],
                                   capture_output=True, timeout=10)
        assert verify_ip.returncode == 0, "generated service certificate does not verify IP SAN"
        cert = subprocess.run(["openssl", "x509", "-in", str(target / "tls-cert"), "-pubkey", "-noout"],
                              capture_output=True, timeout=10, check=True).stdout
        key = subprocess.run(["openssl", "pkey", "-in", str(target / "tls-key"), "-pubout"],
                             capture_output=True, timeout=10, check=True).stdout
        assert cert == key, "service certificate and private key mismatch"
        before = {p.name: p.read_bytes() for p in target.iterdir()}
        refuse = subprocess.run(command, capture_output=True, timeout=10)
        assert refuse.returncode != 0 and before == {p.name: p.read_bytes() for p in target.iterdir()}
        export = ["python3", args.prepare, "--directory", str(target), "--export-client-ca"]
        refuse_export = subprocess.run(export, capture_output=True, timeout=10)
        assert refuse_export.returncode != 0
        assert before == {p.name: p.read_bytes() for p in target.iterdir()}
        client_ca.unlink()
        preserved = {p.name: (p.read_bytes(), p.stat()) for p in target.iterdir()}
        exported = subprocess.run(export, capture_output=True, timeout=10)
        assert exported.returncode == 0
        assert client_ca.read_bytes() == before["tls-ca"]
        assert client_ca.stat().st_uid == os.geteuid()
        assert stat.S_IMODE(client_ca.stat().st_mode) == 0o600
        assert client_ca.read_bytes() not in exported.stdout + exported.stderr
        for name, (content, info) in preserved.items():
            item = target / name
            assert item.read_bytes() == content
            assert (item.stat().st_uid, item.stat().st_mode) == (info.st_uid, info.st_mode)
        client_ca.unlink()
        client_ca.symlink_to(target / "development-ca-key")
        refused_link = subprocess.run(export, capture_output=True, timeout=10)
        assert refused_link.returncode != 0 and client_ca.is_symlink()
        client_ca.unlink()
        target.chmod(0o755)
        refused_mode = subprocess.run(export, capture_output=True, timeout=10)
        assert refused_mode.returncode != 0 and not client_ca.exists()
        target.chmod(0o700)
        alias = Path(root) / "alias"
        alias.symlink_to(target, target_is_directory=True)
        refused_directory = subprocess.run(
            ["python3", args.prepare, "--directory", str(alias), "--export-client-ca"],
            capture_output=True, timeout=10)
        assert refused_directory.returncode != 0 and not client_ca.exists()
        (target / "tls-ca").write_bytes((target / "development-ca-key").read_bytes())
        refused_private = subprocess.run(export, capture_output=True, timeout=10)
        assert refused_private.returncode != 0 and not client_ca.exists()
        invalid = Path(root) / "invalid"
        fail = subprocess.run(["python3", args.prepare, "--directory", str(invalid), "--ownership", "current",
                               "--dns", "localhost\nmalicious=value"], capture_output=True, timeout=10)
        assert fail.returncode != 0 and not invalid.exists()
    print("owned development secret provisioning and preservation checks passed")


if __name__ == "__main__":
    main()
