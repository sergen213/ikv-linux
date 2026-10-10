#!/usr/bin/env python3
"""Validate signed native artifacts locally; --publish explicitly enables protected GitHub writes."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import quote

from sign_manifest import (ED25519_SPKI_PREFIX, MAX_FILE, MAX_FILES, MAX_PACKAGE,
                           MAX_TOTAL, hash_stream, openssl, permissions,
                           regular_file, safe_path)

UINT64_MAX = 2**64 - 1
ENVIRONMENT = "native-release-production"


class PublicationError(ValueError):
    pass


def strict_json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise PublicationError("duplicate JSON key: " + key)
            result[key] = value
        return result
    def invalid_constant(value):
        raise PublicationError("invalid JSON number: " + value)
    return json.loads(data, object_pairs_hook=unique, parse_constant=invalid_constant)


def integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def sha256(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def snapshot(source, destination, maximum):
    size = 0
    with regular_file(source) as incoming, destination.open("xb") as outgoing:
        while chunk := incoming.read(1024 * 1024):
            size += len(chunk)
            if size > maximum:
                raise PublicationError("input exceeds size limit: " + str(source))
            outgoing.write(chunk)
    return destination


def validate_manifest(raw, package_name):
    manifest = strict_json(raw)
    fields = {"schema", "sequence", "version", "channel", "package", "package_size", "package_sha256", "files"}
    if not isinstance(manifest, dict) or set(manifest) != fields:
        raise PublicationError("manifest must contain exactly schema-2 fields")
    if type(manifest["schema"]) is not int or manifest["schema"] != 2:
        raise PublicationError("schema must be integer 2")
    if not integer(manifest["sequence"], 1, UINT64_MAX):
        raise PublicationError("sequence must be a positive uint64 integer")
    if manifest["channel"] not in ("stable", "experimental"):
        raise PublicationError("invalid release channel")
    if not isinstance(manifest["version"], str) or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}", manifest["version"]) is None:
        raise PublicationError("unsafe release version")
    if ".." in manifest["version"] or manifest["version"].endswith((".", ".lock")):
        raise PublicationError("version cannot form a valid Git release tag")
    if manifest["package"] != package_name or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,159}", package_name) is None or package_name in ("manifest.json", "manifest.sig"):
        raise PublicationError("signed package basename must match the supplied safe distinct filename")
    if not integer(manifest["package_size"], 1, MAX_PACKAGE) or not sha256(manifest["package_sha256"]):
        raise PublicationError("invalid signed package size or digest")
    entries = manifest["files"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= MAX_FILES:
        raise PublicationError("invalid signed file count")
    files, total = {}, 0
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256", "executable"}:
            raise PublicationError("invalid signed file fields")
        name = entry["path"]
        if not isinstance(name, str):
            raise PublicationError("signed path must be a string")
        safe_path(name)
        if name in files or not integer(entry["size"], 0, MAX_FILE) or not sha256(entry["sha256"]) or type(entry["executable"]) is not bool:
            raise PublicationError("duplicate path or invalid signed file metadata")
        files[name] = entry
        total += entry["size"]
    parents = {parent.as_posix() for name in files for parent in Path(name).parents if parent != Path(".")}
    if total > MAX_TOTAL or files.keys() & parents:
        raise PublicationError("signed file layout conflicts or exceeds total limit")
    return manifest, files, parents


def validate_package(package, manifest, files, parents):
    with regular_file(package) as source:
        size, digest = hash_stream(source, MAX_PACKAGE)
        if (size, digest) != (manifest["package_size"], manifest["package_sha256"]):
            raise PublicationError("package differs from signed size/SHA-256")
        source.seek(0)
        magic = source.read(2)
        source.seek(0)
        seen, found = set(), set()
        with tarfile.open(fileobj=source, mode="r:gz" if magic == b"\x1f\x8b" else "r:") as archive:
            for member in archive:
                name = member.name[:-1] if member.isdir() and member.name.endswith("/") else member.name
                safe_path(name)
                if name in seen:
                    raise PublicationError("duplicate archive member")
                seen.add(name)
                mode = permissions(member.mode)
                if member.isdir():
                    if name not in parents or member.size != 0:
                        raise PublicationError("unsigned/invalid archive directory")
                elif member.isreg() and member.sparse is None and name in files:
                    expected = files[name]
                    if member.size != expected["size"] or bool(mode & 0o111) != expected["executable"]:
                        raise PublicationError("archive size/executable mode differs from signed file")
                    content = archive.extractfile(member)
                    if content is None:
                        raise PublicationError("unreadable archive member")
                    with content:
                        actual = hash_stream(content, MAX_FILE)
                    if actual != (expected["size"], expected["sha256"]):
                        raise PublicationError("archive content differs from signed file")
                    found.add(name)
                else:
                    raise PublicationError("unsigned, linked, sparse or special archive member")
        if found != files.keys():
            raise PublicationError("archive is missing signed files")


class GitHub:
    def __init__(self, repo, host="github.com"):
        self.repo, self.host = repo, host
        self.prefix = "/repos/" + repo

    def run(self, arguments, input_data=None, output=None):
        environment = os.environ.copy()
        token = environment.get("GH_TOKEN")
        if not token:
            raise PublicationError("GH_TOKEN is required for publication")
        environment.update(GH_HOST=self.host, GH_PROMPT_DISABLED="1")
        if self.host != "github.com":
            environment["GH_ENTERPRISE_TOKEN"] = token
        environment.pop("GH_DEBUG", None)
        return subprocess.run(["gh", *arguments], input=input_data,
                              stdout=output if output is not None else subprocess.PIPE,
                              stderr=subprocess.PIPE, env=environment, timeout=600)

    def api(self, endpoint, method="GET", payload=None, missing_ok=False):
        arguments = ["api", "--method", method, "--include",
                     "-H", "Accept: application/vnd.github+json", endpoint]
        if payload is not None:
            arguments += ["--input", "-"]
        result = self.run(arguments, json.dumps(payload).encode() if payload is not None else None)
        # --include preserves the HTTP status, so authentication/transport errors never look like absence.
        response = result.stdout.replace(b"\r\n", b"\n")
        match = re.match(rb"HTTP/\S+ (\d{3})[^\n]*\n", response)
        status = int(match[1]) if match else None
        if missing_ok and status == 404:
            return None
        if result.returncode != 0 or status is None or not 200 <= status < 300:
            raise PublicationError("GitHub API failed (HTTP " + str(status) + "): " + result.stderr.decode(errors="replace").strip())
        _, separator, body = response.partition(b"\n\n")
        if not separator:
            raise PublicationError("GitHub API returned no response body")
        return strict_json(body)

    def command(self, arguments):
        result = self.run([*arguments, "--repo", self.repo])
        if result.returncode:
            raise PublicationError("gh release " + arguments[1] + " failed for " + self.repo + ":" + arguments[2] +
                                   ": " + result.stderr.decode(errors="replace").strip())

    def download(self, asset_id, destination):
        with destination.open("wb") as output:
            result = self.run(["api", "-H", "Accept: application/octet-stream",
                               self.prefix + "/releases/assets/" + str(asset_id)], output=output)
        if result.returncode:
            raise PublicationError("GitHub asset download failed: " + result.stderr.decode(errors="replace").strip())


def fetch_release(gh, tag):
    release = gh.api(gh.prefix + "/releases/tags/" + quote(tag, safe=""), missing_ok=True)
    if release is not None:
        return release
    # REST's tag route finds published releases; GraphQL also finds a draft's pending tag.
    owner, name = gh.repo.split("/")
    response = gh.api("graphql", "POST", {
        "query": "query($owner:String!,$name:String!,$tag:String!){repository(owner:$owner,name:$name){release(tagName:$tag){databaseId isDraft}}}",
        "variables": {"owner": owner, "name": name, "tag": tag}})
    if not isinstance(response, dict) or response.get("errors") or not isinstance(response.get("data"), dict):
        raise PublicationError("GitHub draft lookup failed")
    repository = response["data"].get("repository")
    if not isinstance(repository, dict) or "release" not in repository:
        raise PublicationError("GitHub draft lookup returned no repository")
    draft = repository["release"]
    if draft is None:
        return None
    if not isinstance(draft, dict) or draft.get("isDraft") is not True or not integer(draft.get("databaseId"), 1, 2**63 - 1):
        raise PublicationError("GitHub pending release metadata is invalid")
    return gh.api(gh.prefix + "/releases/" + str(draft["databaseId"]))


def release_identity(manifest, assets, target):
    return {"schema": 1, "channel": manifest["channel"], "sequence": manifest["sequence"],
            "tag": "native-" + manifest["channel"] + "-" + manifest["version"], "target": target,
            "manifest_sha256": assets["manifest.json"][1], "signature_sha256": assets["manifest.sig"][1],
            "package": manifest["package"], "package_size": manifest["package_size"],
            "package_sha256": manifest["package_sha256"]}


def read_ledger(gh, path, branch):
    response = gh.api(gh.prefix + "/contents/" + path + "?ref=" + quote(branch, safe=""))
    if not isinstance(response, dict) or response.get("type") != "file" or response.get("encoding") != "base64" or not isinstance(response.get("sha"), str) or not response["sha"]:
        raise PublicationError("ledger must be an existing Contents API file with SHA")
    try:
        ledger = strict_json(base64.b64decode("".join(response["content"].split()), validate=True))
    except (KeyError, TypeError, ValueError) as error:
        raise PublicationError("invalid ledger encoding") from error
    seed_fields = {"schema", "channel", "sequence"}
    issued_fields = seed_fields | {"tag", "target", "manifest_sha256", "signature_sha256", "package", "package_size", "package_sha256"}
    if not isinstance(ledger, dict) or type(ledger.get("schema")) is not int or ledger["schema"] != 1 or ledger.get("channel") not in ("stable", "experimental") or not integer(ledger.get("sequence"), 0, UINT64_MAX):
        raise PublicationError("invalid durable issuance ledger")
    if ledger["sequence"] == 0:
        valid = set(ledger) == seed_fields
    else:
        valid = (set(ledger) == issued_fields and all(sha256(ledger.get(key)) for key in ("manifest_sha256", "signature_sha256", "package_sha256"))
                 and isinstance(ledger.get("target"), str) and re.fullmatch(r"[0-9a-f]{40}", ledger["target"]) is not None
                 and isinstance(ledger.get("tag"), str) and re.fullmatch(r"native-" + ledger["channel"] + r"-[A-Za-z0-9][A-Za-z0-9._-]{0,95}", ledger["tag"]) is not None
                 and isinstance(ledger.get("package"), str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,159}", ledger["package"]) is not None
                 and integer(ledger.get("package_size"), 1, MAX_PACKAGE))
    if not valid:
        raise PublicationError("invalid ledger fields; operator initialization is required")
    return ledger, response["sha"]


def reserve(gh, identity, branch):
    gh.api(gh.prefix + "/git/ref/heads/" + quote(branch, safe=""))
    path = ".ikv-release-ledger/" + identity["channel"] + ".json"
    ledger, sha = read_ledger(gh, path, branch)
    if ledger["channel"] != identity["channel"] or ledger["sequence"] > identity["sequence"]:
        raise PublicationError("channel mismatch or release sequence below durable high-water mark")
    if ledger["sequence"] == identity["sequence"]:
        if ledger != identity:
            raise PublicationError("equal-sequence retry must match exact manifest/signature/package/target metadata")
    else:
        gh.api(gh.prefix + "/contents/" + path, "PUT", {
            "branch": branch, "sha": sha,
            "message": "Reserve authenticated " + identity["tag"] + " sequence " + str(identity["sequence"]),
            "content": base64.b64encode(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).decode()})
    return path


def assert_reservation(gh, path, branch, identity):
    if read_ledger(gh, path, branch)[0] != identity:
        raise PublicationError("issuance reservation was superseded; refusing stale release writes")


def check_release(release, identity, body):
    if (not isinstance(release, dict) or not integer(release.get("id"), 1, 2**63 - 1)
            or release.get("tag_name") != identity["tag"] or release.get("target_commitish") != identity["target"]
            or release.get("name") != identity["tag"] or release.get("body") != body
            or type(release.get("draft")) is not bool or release.get("prerelease") is not (identity["channel"] == "experimental")
            or not isinstance(release.get("assets"), list)):
        raise PublicationError("existing release immutable metadata differs; refusing recovery")


def check_tag(gh, identity, required=False):
    reference = gh.api(gh.prefix + "/git/ref/tags/" + quote(identity["tag"], safe=""), missing_ok=True)
    if reference is None:
        if required:
            raise PublicationError("published tag is missing")
        return
    obj = reference.get("object", {})
    for _ in range(8):
        if obj.get("type") != "tag":
            break
        tag_sha = obj.get("sha")
        if not isinstance(tag_sha, str) or re.fullmatch(r"[0-9a-f]{40}", tag_sha) is None:
            raise PublicationError("invalid annotated tag SHA")
        obj = gh.api(gh.prefix + "/git/tags/" + tag_sha).get("object", {})
    if obj.get("type") != "commit" or obj.get("sha") != identity["target"]:
        raise PublicationError("existing tag does not point to the explicit target SHA")


def verify_assets(gh, release, assets, directory, complete=False):
    found = set()
    for asset in release["assets"]:
        if not isinstance(asset, dict):
            raise PublicationError("invalid release asset metadata")
        name = asset.get("name")
        if name not in assets or name in found or not integer(asset.get("id"), 1, 2**63 - 1) or asset.get("state") != "uploaded" or type(asset.get("size")) is not int or asset["size"] != assets[name][0]:
            raise PublicationError("unexpected, duplicate or conflicting release asset")
        downloaded = directory / ("download-" + str(asset["id"]))
        gh.download(asset["id"], downloaded)
        with regular_file(downloaded) as source:
            actual = hash_stream(source, assets[name][0])
        downloaded.unlink()
        if actual != assets[name]:
            raise PublicationError("existing/server release asset bytes conflict: " + name)
        found.add(name)
    if complete and found != assets.keys():
        raise PublicationError("server release is missing verified assets")
    return found


def check_default_target(gh, target):
    message = "--target must match the repository's current default-branch HEAD; review the fresh default-branch tip and rerun the approved workflow"
    try:
        repository = gh.api(gh.prefix)
        branch = repository.get("default_branch") if isinstance(repository, dict) else None
        if not isinstance(branch, str) or not branch or len(branch) > 255:
            raise PublicationError(message)
        reference = gh.api(gh.prefix + "/git/ref/heads/" + quote(branch, safe=""))
        obj = reference.get("object") if isinstance(reference, dict) else None
        if not isinstance(obj, dict) or obj.get("type") != "commit" or obj.get("sha") != target:
            raise PublicationError(message)
    except PublicationError as error:
        if str(error) == message:
            raise
        raise PublicationError(message + ": " + str(error)) from error


def publish(gh, manifest, assets, directory, target, branch):
    environment = gh.api(gh.prefix + "/environments/" + ENVIRONMENT)
    rules = environment.get("protection_rules") if isinstance(environment, dict) else None
    if not isinstance(rules, list) or not any(
            isinstance(rule, dict) and rule.get("type") == "required_reviewers"
            and isinstance(rule.get("reviewers"), list) and rule["reviewers"]
            and all(isinstance(reviewer, dict) and reviewer.get("type") in ("User", "Team")
                    and isinstance(reviewer.get("reviewer"), dict)
                    and integer(reviewer["reviewer"].get("id"), 1, 2**63 - 1)
                    for reviewer in rule["reviewers"])
            for rule in rules):
        raise PublicationError("production environment must have configured required reviewers before any write")
    identity = release_identity(manifest, assets, target)
    body = "IKV authenticated native release\n" + json.dumps(identity, sort_keys=True, separators=(",", ":"))
    release = fetch_release(gh, identity["tag"])
    check_tag(gh, identity, required=release is not None and release.get("draft") is False)
    if release is not None:
        check_release(release, identity, body)
        verify_assets(gh, release, assets, directory, complete=not release["draft"])
    check_default_target(gh, target)
    ledger_path = reserve(gh, identity, branch)
    assert_reservation(gh, ledger_path, branch, identity)
    if release is None:
        arguments = ["release", "create", identity["tag"], "--draft", "--target", target,
                     "--title", identity["tag"], "--notes", body]
        if manifest["channel"] == "experimental":
            arguments.append("--prerelease")
        check_default_target(gh, target)
        gh.command(arguments)
        release = fetch_release(gh, identity["tag"])
        check_release(release, identity, body)
        if not release["draft"]:
            raise PublicationError("new release was not created as a draft")
        check_tag(gh, identity)
    present = verify_assets(gh, release, assets, directory)
    for name in assets:
        if name not in present:
            assert_reservation(gh, ledger_path, branch, identity)
            check_default_target(gh, target)
            gh.command(["release", "upload", identity["tag"], str(directory / name)])
    release = fetch_release(gh, identity["tag"])
    check_release(release, identity, body)
    verify_assets(gh, release, assets, directory, complete=True)
    assert_reservation(gh, ledger_path, branch, identity)
    check_tag(gh, identity, required=not release["draft"])
    if release["draft"]:
        check_default_target(gh, target)
        gh.command(["release", "edit", identity["tag"], "--draft=false",
                    "--prerelease=" + ("true" if manifest["channel"] == "experimental" else "false"),
                    "--target", target])
        release = fetch_release(gh, identity["tag"])
        check_release(release, identity, body)
        if release["draft"]:
            raise PublicationError("GitHub did not publish the verified draft")
        check_tag(gh, identity, required=True)
    return identity["tag"]


def execute(args):
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}", args.repo) is None or args.repo.split("/")[1] in (".", ".."):
        raise PublicationError("--repo must be an explicit OWNER/REPO")
    if not isinstance(args.host, str) or re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?(?::[0-9]{1,5})?", args.host) is None:
        raise PublicationError("--host must be an HTTPS hostname with optional port, not a URL")
    if ":" in args.host and not 1 <= int(args.host.rsplit(":", 1)[1]) <= 65535:
        raise PublicationError("invalid HTTPS host port")
    if re.fullmatch(r"[0-9a-fA-F]{40}", args.target) is None:
        raise PublicationError("--target must be an explicit full commit SHA")
    branch = args.ledger_ref
    if (not branch or len(branch) > 255 or branch.startswith(("-", "/", ".")) or branch.endswith(("/", "."))
            or re.search(r"[^A-Za-z0-9._/-]|\.\.|//|@\{", branch)
            or any(component.startswith(".") or component.endswith(".lock") for component in branch.split("/"))):
        raise PublicationError("--ledger-ref must name an explicit existing safe branch")
    with tempfile.TemporaryDirectory(prefix="ikv-publish-") as temporary:
        directory = Path(temporary)
        manifest_path = snapshot(args.manifest, directory / "manifest.json", 1024 * 1024)
        signature_path = snapshot(args.signature, directory / "manifest.sig", 64)
        trust = snapshot(args.trust, directory / "trust.pem", 16384)
        manifest, files, parents = validate_manifest(manifest_path.read_bytes(), args.package.name)
        package = snapshot(args.package, directory / manifest["package"], MAX_PACKAGE)
        public_der = openssl(["pkey", "-pubin", "-in", trust, "-outform", "DER"])
        if len(public_der) != 44 or not public_der.startswith(ED25519_SPKI_PREFIX):
            raise PublicationError("trusted public PEM must be canonical Ed25519 SPKI")
        if signature_path.stat().st_size != 64:
            raise PublicationError("signature must be exactly 64 raw Ed25519 bytes")
        openssl(["pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", trust,
                 "-in", manifest_path, "-sigfile", signature_path])
        validate_package(package, manifest, files, parents)
        assets = {}
        for path in (manifest_path, signature_path, package):
            with regular_file(path) as source:
                assets[path.name] = hash_stream(source, MAX_PACKAGE)
        if not args.publish:
            print("Validated exact signed artifacts locally; publication disabled (no network requests).")
            return
        tag = publish(GitHub(args.repo, args.host), manifest, assets, directory, args.target.lower(), branch)
        print("Published verified GitHub release " + args.repo + ":" + tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--signature", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--trust", type=Path, required=True)
    parser.add_argument("--ledger-ref", required=True)
    parser.add_argument("--target", required=True, help="Full current default-branch HEAD SHA; review the fresh tip before rerunning a rejected publication")
    parser.add_argument("--host", default="github.com", help="Explicit GitHub/Enterprise HTTPS host (default github.com)")
    parser.add_argument("--publish", action="store_true", help="Explicit authorization for external ledger/release writes")
    try:
        execute(parser.parse_args())
    except (OSError, ValueError, tarfile.TarError, subprocess.TimeoutExpired) as error:
        print("publish_release: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
