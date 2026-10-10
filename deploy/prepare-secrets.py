#!/usr/bin/env python3
"""Provision a NEW owned development-only CA and private Compose secrets.

No production trust is implied. Existing directories/material are never replaced.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=120)
    if result.returncode:
        # Child output may contain paths or private configuration; do not echo it.
        raise RuntimeError(f"{Path(command[0]).name} failed (exit {result.returncode})")


def write_private(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as stream:
        stream.write(value)


def export_client_ca(directory, image):
    info = directory.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) != 0o700):
        raise ValueError("export requires an existing current-user-owned mode0700 directory")
    destination = directory / "client-ca.pem"
    if destination.exists() or destination.is_symlink():
        raise ValueError("refusing existing client CA destination")
    source = directory / "tls-ca"
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 16384:
        raise ValueError("CA must be a bounded regular certificate file")
    try:
        fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as stream:
            certificate = stream.read(16385)
    except PermissionError:
        with tempfile.TemporaryFile() as output:
            result = subprocess.run(
                ["docker", "run", "--rm", "--network", "none", "--read-only",
                 "--cap-drop", "ALL", "--cap-add", "DAC_OVERRIDE",
                 "--security-opt", "no-new-privileges:true", "--user", "0:0",
                 "--mount", f"type=bind,src={directory.resolve()},dst=/secrets,readonly",
                 "--entrypoint", "/bin/cat", image, "/secrets/tls-ca"],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.DEVNULL,
                timeout=120)
            if result.returncode:
                raise RuntimeError("owned Docker CA export failed")
            output.seek(0)
            certificate = output.read(16385)
    if (len(certificate) > 16384 or not re.fullmatch(
            rb"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+"
            rb"-----END CERTIFICATE-----\r?\n?", certificate)):
        raise ValueError("CA export is not a single public PEM certificate")
    parsed = subprocess.run(["openssl", "x509", "-inform", "PEM", "-noout"],
                            input=certificate, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, timeout=10)
    if parsed.returncode:
        raise ValueError("CA certificate parsing failed")
    write_private(destination, certificate.decode("ascii"))


def provision(args):
    directory = Path(args.directory).absolute()
    if any(c in str(directory) for c in ",\n\r"):
        raise ValueError("directory must not contain commas or line breaks")
    if directory.exists() or directory.is_symlink():
        raise ValueError("refusing existing secrets directory; choose a NEW owned path")
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,253}", args.dsn_host):
        raise ValueError("invalid database hostname")
    dns = args.dns or ["localhost"]
    ips = args.ip or ["127.0.0.1"]
    for name in dns:
        if not re.fullmatch(r"[A-Za-z0-9.-]{1,253}", name) or name.startswith("-"):
            raise ValueError("invalid certificate DNS SAN")
    ips = [str(ipaddress.ip_address(value)) for value in ips]
    if not 1 <= args.days <= 30:
        raise ValueError("development certificate lifetime must be 1..30 days")
    directory.parent.mkdir(parents=True, exist_ok=True)
    directory.mkdir(mode=0o700)
    complete = False
    try:
        password = secrets.token_hex(32)
        write_private(directory / "db-password", secrets.token_hex(32) + "\n")
        write_private(directory / "app-db-password", password + "\n")
        write_private(directory / "dsn", f"host={args.dsn_host} port=5432 dbname=ikv user=ikv password={password}\n")
        del password
        ca_key = directory / "development-ca-key"
        ca = directory / "tls-ca"
        key = directory / "tls-key"
        csr = directory / "request.csr"
        extensions = directory / "certificate.cnf"
        write_private(extensions, "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=" + ",".join(["DNS:" + name for name in dns] + ["IP:" + value for value in ips]) + "\n")
        run(["openssl", "req", "-x509", "-newkey", "rsa:3072", "-noenc", "-sha256",
             "-days", str(args.days), "-subj", "/CN=IKV OWNED DEVELOPMENT CA - NOT PRODUCTION",
             "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign",
             "-keyout", str(ca_key), "-out", str(ca)])
        run(["openssl", "req", "-new", "-newkey", "rsa:3072", "-noenc", "-sha256",
             "-subj", "/CN=IKV OWNED DEVELOPMENT SERVICE", "-keyout", str(key), "-out", str(csr)])
        run(["openssl", "x509", "-req", "-in", str(csr), "-CA", str(ca), "-CAkey", str(ca_key),
             "-set_serial", "0x" + secrets.token_hex(16), "-days", str(args.days), "-sha256",
             "-extfile", str(extensions), "-out", str(directory / "tls-cert")])
        csr.unlink()
        extensions.unlink()
        for file in directory.iterdir():
            file.chmod(0o600)
        export_client_ca(directory, args.ownership_image)
        if args.ownership == "compose":
            service = ("dsn", "tls-cert", "tls-key", "tls-ca")
            if os.geteuid() == 0:
                for name in service:
                    os.chown(directory / name, 10001, 10001)
                    (directory / name).chmod(0o400)
                os.chown(directory / "app-db-password", 999, 999)
                (directory / "app-db-password").chmod(0o400)
                os.chown(directory / "db-password", 0, 0)
            else:
                # The explicitly owned Docker daemon supplies only file ownership.
                # No network, mounts outside the new directory, or persistent helper.
                run(["docker", "run", "--rm", "--network", "none", "--read-only",
                     "--cap-drop", "ALL", "--cap-add", "CHOWN", "--cap-add", "FOWNER",
                     "--cap-add", "DAC_OVERRIDE", "--security-opt", "no-new-privileges:true",
                     "--user", "0:0", "--mount", f"type=bind,src={directory},dst=/secrets",
                     args.ownership_image, "/bin/sh", "-eu", "-c",
                     "chown 10001:10001 /secrets/dsn /secrets/tls-cert /secrets/tls-key /secrets/tls-ca; "
                     "chmod 0400 /secrets/dsn /secrets/tls-cert /secrets/tls-key /secrets/tls-ca; "
                     "chown 999:999 /secrets/app-db-password; chmod 0400 /secrets/app-db-password; "
                     "chown 0:0 /secrets/db-password; chmod 0600 /secrets/db-password"])
            for name in service:
                info = (directory / name).stat()
                if (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) != (10001, 10001, 0o400):
                    raise RuntimeError("Compose service secret ownership mapping failed")
            app = (directory / "app-db-password").stat()
            admin = (directory / "db-password").stat()
            if (app.st_uid, app.st_gid, stat.S_IMODE(app.st_mode)) != (999, 999, 0o400) or (admin.st_uid, stat.S_IMODE(admin.st_mode)) != (0, 0o600):
                raise RuntimeError("Compose PostgreSQL secret ownership mapping failed")
        complete = True
        return directory
    finally:
        if not complete:
            shutil.rmtree(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, help="NEW private directory, or existing owned directory for CA export")
    parser.add_argument("--export-client-ca", action="store_true",
                        help="export only existing public tls-ca to a new current-user-owned client-ca.pem")
    parser.add_argument("--dns", action="append", help="owned DNS SAN (repeatable; default localhost)")
    parser.add_argument("--ip", action="append", help="owned IP SAN (repeatable; default127.0.0.1)")
    parser.add_argument("--days", type=int, default=7)
    parser.add_argument("--dsn-host", default="database")
    parser.add_argument("--ownership", choices=("compose", "current"), default="compose",
                        help="compose sets real image UIDs; current keeps private files for native disposable checks")
    parser.add_argument("--ownership-image", default="debian:trixie-slim",
                        help="owned Docker helper image used only if not root (may require permitted image pull)")
    args = parser.parse_args()
    old_umask = os.umask(0o077)
    try:
        if args.export_client_ca:
            directory = Path(args.directory).absolute()
            if any(c in str(directory) for c in ",\n\r"):
                raise ValueError("directory must not contain commas or line breaks")
            export_client_ca(directory, args.ownership_image)
            print(f"Exported public client CA to {directory / 'client-ca.pem'}")
            return 0
        directory = provision(args)
        print(f"Created OWNED DEVELOPMENT ONLY CA/SAN and private secrets in {directory}")
        print("No production trust or existing credentials were replaced.")
        if args.ownership == "current":
            print("Current-user ownership selected: not the Compose image UID mapping.")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        print("Secret provisioning failed; no secret values printed. Check directory, openssl and owned Docker access.", file=sys.stderr)
        return 1
    finally:
        os.umask(old_umask)


if __name__ == "__main__":
    sys.exit(main())
