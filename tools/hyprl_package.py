#!/usr/bin/env python3
"""HYPRLODR mod package tooling.

Format (schema 1): a ZIP container with `manifest.json` at the archive root.

```text
manifest.json                 id, name, version, author, api, platform, entry, ...
native/<platform>/<id>.dll    the mod binary (or .so on linux-x86_64)
assets/...                    optional payload, opaque to the loader for now
config/...                    optional defaults shipped with the mod
README.md, LICENSE            optional documentation
```

Validation is defensive and never executes mod code: metadata is checked
structurally, and the manifest is cross-checked against the ABI descriptor only
by loading the library through `hyprlodr-host --describe`, which calls the
descriptor query and nothing else.

Usage:
    hyprl_package.py pack <dir> <out.zip>
    hyprl_package.py validate <package.zip|dir>
    hyprl_package.py install <package.zip> --mods-dir DIR [--host PATH] [--force]
    hyprl_package.py list --mods-dir DIR
    hyprl_package.py remove <mod-id> --mods-dir DIR
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path

SCHEMA = 1

# Limits are deliberately conservative; they are what stops a hostile package
# from filling the disk or exhausting memory before validation finishes.
MAX_ENTRIES = 4096
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_ENTRY_BYTES = 64 * 1024 * 1024
MAX_COMPRESSION_RATIO = 200

ALLOWED_ROOT_FILES = {"manifest.json", "README.md", "LICENSE"}
ALLOWED_PREFIXES = ("native/", "assets/", "config/")
PLATFORM_SUFFIX = {"windows-x86_64": ".dll", "linux-x86_64": ".so"}
REQUIRED_FIELDS = ("schema", "id", "name", "version", "author", "api", "platform", "entry")

ID_RE = re.compile(r"^[a-z0-9._-]{1,127}$")
VERSION_RE = re.compile(r"^\d+(\.\d+){0,2}$")
DEP_RE = re.compile(r"^[a-z0-9._-]{1,127}(@(>=|<=|>|<|=|\^|~)\d+(\.\d+){0,2})?$")
INDEX_NAME = "hyprlodr-mods.json"


class PackageError(Exception):
    """Raised for any invalid, unsafe or inconsistent package."""


def _check_entry_name(name: str) -> str:
    if not name or name.endswith("/"):
        raise PackageError(f"directory entries are not allowed: {name!r}")
    if "\\" in name:
        raise PackageError(f"backslash separators are not allowed: {name!r}")
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        raise PackageError(f"absolute paths are not allowed: {name!r}")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise PackageError(f"unsafe path component in {name!r}")
    if name != "/".join(parts):
        raise PackageError(f"non-normalised path: {name!r}")
    return name


def _is_allowed_path(name: str) -> bool:
    if name in ALLOWED_ROOT_FILES:
        return True
    return any(name.startswith(prefix) for prefix in ALLOWED_PREFIXES)


def _read_manifest(archive: zipfile.ZipFile) -> dict:
    try:
        raw = archive.read("manifest.json")
    except KeyError:
        raise PackageError("manifest.json missing from the archive root") from None
    try:
        manifest = json.loads(raw)
    except ValueError as exc:
        raise PackageError(f"manifest.json is not valid JSON: {exc}") from None
    if not isinstance(manifest, dict):
        raise PackageError("manifest.json must contain an object")
    return manifest


def validate_manifest(manifest: dict, entries: list[str]) -> None:
    for field in REQUIRED_FIELDS:
        if field not in manifest:
            raise PackageError(f"manifest is missing required field {field!r}")

    if manifest["schema"] != SCHEMA:
        raise PackageError(f"unsupported manifest schema {manifest['schema']!r}; expected {SCHEMA}")
    if not isinstance(manifest["id"], str) or not ID_RE.match(manifest["id"]):
        raise PackageError(f"invalid mod id {manifest['id']!r} (lowercase a-z0-9._-, max 127)")
    if not isinstance(manifest["version"], str) or not VERSION_RE.match(manifest["version"]):
        raise PackageError(f"invalid version {manifest['version']!r}")
    if not isinstance(manifest["api"], int) or manifest["api"] < 1:
        raise PackageError(f"invalid api field {manifest['api']!r}")
    platform = manifest["platform"]
    if platform not in PLATFORM_SUFFIX:
        raise PackageError(f"unsupported platform {platform!r}; expected one of {sorted(PLATFORM_SUFFIX)}")

    for field in ("dependencies", "optional_dependencies", "conflicts"):
        value = manifest.get(field, [])
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise PackageError(f"{field} must be a list of strings")
        for item in value:
            if not DEP_RE.match(item):
                raise PackageError(f"invalid {field} entry {item!r}")
    for item in manifest.get("capabilities", []):
        if not isinstance(item, str) or not item:
            raise PackageError("capabilities must be non-empty strings")

    entry = manifest["entry"]
    if not isinstance(entry, str) or not entry.startswith("native/"):
        raise PackageError("entry must live under native/")
    if not entry.endswith(PLATFORM_SUFFIX[platform]):
        raise PackageError(f"entry {entry!r} does not match platform {platform}")
    if entry not in entries:
        raise PackageError(f"entry {entry!r} is not present in the archive")


def inspect_package(path: Path) -> tuple[dict, list[str]]:
    """Validate an archive without extracting anything."""
    if not path.is_file():
        raise PackageError(f"{path} is not a file")
    try:
        archive = zipfile.ZipFile(path)
    except zipfile.BadZipFile as exc:
        raise PackageError(f"not a ZIP archive: {exc}") from None

    with archive:
        infos = archive.infolist()
        if len(infos) > MAX_ENTRIES:
            raise PackageError(f"too many entries ({len(infos)} > {MAX_ENTRIES})")

        names: list[str] = []
        seen: dict[str, str] = {}
        total = 0
        for info in infos:
            name = _check_entry_name(info.filename)
            if not _is_allowed_path(name):
                raise PackageError(f"file not allowed in a mod package: {name!r}")
            mode = (info.external_attr >> 16) & 0o170000
            if mode and mode != 0o100000:
                raise PackageError(f"only regular files are allowed: {name!r}")
            lowered = name.lower()
            if lowered in seen:
                raise PackageError(f"duplicate entry (case-insensitive): {name!r} and {seen[lowered]!r}")
            if name in seen:
                raise PackageError(f"duplicate entry: {name!r}")
            seen[lowered] = name
            if info.file_size > MAX_ENTRY_BYTES:
                raise PackageError(f"entry {name!r} is too large ({info.file_size} bytes)")
            total += info.file_size
            if total > MAX_TOTAL_BYTES:
                raise PackageError(f"package expands beyond {MAX_TOTAL_BYTES} bytes")
            if info.compress_size and info.file_size > 1024:
                ratio = info.file_size / info.compress_size
                if ratio > MAX_COMPRESSION_RATIO:
                    raise PackageError(
                        f"entry {name!r} has a suspicious compression ratio ({ratio:.0f}:1)")
            names.append(name)

        manifest = _read_manifest(archive)
        validate_manifest(manifest, names)
    return manifest, names


def describe_library(host: Path, library: Path) -> list[dict]:
    """Ask a built hyprlodr-host for the real ABI descriptor of a library."""
    with tempfile.TemporaryDirectory(prefix="hyprlodr-desc-") as tmp:
        root = Path(tmp)
        mods = root / "mods"
        mods.mkdir()
        shutil.copy(library, mods / library.name)
        proc = subprocess.run(
            [str(host), "--describe", "--log", str(root / "events.jsonl"), str(mods)],
            capture_output=True, text=True)
        line = next((l for l in proc.stdout.splitlines() if l.startswith("{")), "")
        if not line:
            raise PackageError(f"descriptor query produced no output: {proc.stderr.strip()}")
        payload = json.loads(line)
        if proc.returncode != 0:
            raise PackageError(f"library rejected by the loader: "
                               f"{[mod.get('id') for mod in payload.get('mods', [])] or proc.stderr.strip()}")
        return payload.get("mods", [])


def cross_check(manifest: dict, host: Path, library: Path) -> None:
    mods = describe_library(host, library)
    if len(mods) != 1:
        raise PackageError(f"expected exactly one mod descriptor, got {len(mods)}")
    descriptor = mods[0]
    if descriptor["id"] != manifest["id"]:
        raise PackageError(
            f"manifest id {manifest['id']!r} does not match descriptor id {descriptor['id']!r}")
    if descriptor["version"] != manifest["version"]:
        raise PackageError(
            f"manifest version {manifest['version']!r} does not match descriptor "
            f"version {descriptor['version']!r}")
    declared = sorted(manifest.get("dependencies", []))
    actual = sorted(descriptor.get("dependencies", []))
    if declared != actual:
        raise PackageError(f"manifest dependencies {declared} do not match descriptor {actual}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pack(source: Path, output: Path) -> dict:
    source = source.resolve()
    manifest_path = source / "manifest.json"
    if not manifest_path.is_file():
        raise PackageError(f"{manifest_path} not found")
    manifest = json.loads(manifest_path.read_text())

    files = sorted(p for p in source.rglob("*") if p.is_file())
    names = [_check_entry_name(str(p.relative_to(source)).replace("\\", "/")) for p in files]
    for name in names:
        if not _is_allowed_path(name):
            raise PackageError(f"file not allowed in a mod package: {name!r}")
    validate_manifest(manifest, names)

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, name in zip(files, names):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    # Re-validate what we just wrote; the archive is the artefact that ships.
    inspect_package(output)
    return output


def _index_path(mods_dir: Path) -> Path:
    return mods_dir / INDEX_NAME


def load_index(mods_dir: Path) -> dict:
    path = _index_path(mods_dir)
    if not path.is_file():
        return {"schema": 1, "mods": {}}
    try:
        data = json.loads(path.read_text())
    except ValueError:
        return {"schema": 1, "mods": {}}
    data.setdefault("mods", {})
    return data


def save_index(mods_dir: Path, index: dict) -> None:
    mods_dir.mkdir(parents=True, exist_ok=True)
    _index_path(mods_dir).write_text(json.dumps(index, indent=2, sort_keys=True))


def install(package: Path, mods_dir: Path, host: Path | None = None, force: bool = False) -> dict:
    manifest, _ = inspect_package(package)
    mod_id = manifest["id"]
    suffix = PLATFORM_SUFFIX[manifest["platform"]]
    destination = mods_dir / f"{mod_id}{suffix}"

    with zipfile.ZipFile(package) as archive:
        payload = archive.read(manifest["entry"])
    if len(payload) > MAX_ENTRY_BYTES:
        raise PackageError("entry grew between validation and extraction")

    mods_dir.mkdir(parents=True, exist_ok=True)
    index = load_index(mods_dir)
    existing = index["mods"].get(mod_id)
    digest = hashlib.sha256(payload).hexdigest()
    if existing and existing.get("sha256") != digest and not force:
        raise PackageError(
            f"mod {mod_id!r} is already installed with different content; pass --force to replace")
    if existing and existing.get("sha256") == digest:
        return existing

    destination.write_bytes(payload)
    if sha256_file(destination) != digest:
        raise PackageError("hash mismatch after extraction; the package may be corrupt")

    record = {
        "id": mod_id,
        "name": manifest["name"],
        "version": manifest["version"],
        "author": manifest["author"],
        "description": manifest.get("description", ""),
        "dependencies": manifest.get("dependencies", []),
        "optional_dependencies": manifest.get("optional_dependencies", []),
        "conflicts": manifest.get("conflicts", []),
        "capabilities": manifest.get("capabilities", []),
        "platform": manifest["platform"],
        "file": destination.name,
        "sha256": digest,
        "installed_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    if host is not None:
        cross_check(manifest, host, destination)
        record["descriptor_verified"] = True
    index["mods"][mod_id] = record
    save_index(mods_dir, index)
    return record


def remove(mods_dir: Path, mod_id: str) -> str:
    index = load_index(mods_dir)
    record = index["mods"].get(mod_id)
    if not record:
        raise PackageError(f"mod {mod_id!r} is not installed")
    path = mods_dir / record["file"]
    if path.is_file():
        if sha256_file(path) != record["sha256"]:
            raise PackageError(
                f"{path.name} changed since installation; refusing to delete it")
        path.unlink()
    del index["mods"][mod_id]
    save_index(mods_dir, index)
    return record["file"]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="HYPRLODR mod package tooling")
    sub = parser.add_subparsers(dest="command", required=True)

    pack_cmd = sub.add_parser("pack", help="build a package from a directory")
    pack_cmd.add_argument("source", type=Path)
    pack_cmd.add_argument("output", type=Path)

    validate_cmd = sub.add_parser("validate", help="validate a package")
    validate_cmd.add_argument("package", type=Path)

    install_cmd = sub.add_parser("install", help="install a package into a mods directory")
    install_cmd.add_argument("package", type=Path)
    install_cmd.add_argument("--mods-dir", type=Path, required=True)
    install_cmd.add_argument("--host", type=Path, help="hyprlodr-host binary for descriptor cross-check")
    install_cmd.add_argument("--force", action="store_true")

    list_cmd = sub.add_parser("list", help="list installed mods")
    list_cmd.add_argument("--mods-dir", type=Path, required=True)

    remove_cmd = sub.add_parser("remove", help="remove an installed mod")
    remove_cmd.add_argument("mod_id")
    remove_cmd.add_argument("--mods-dir", type=Path, required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "pack":
            archive = pack(args.source, args.output)
            manifest, entries = inspect_package(archive)
            print(f"packed {manifest['id']} {manifest['version']} ({len(entries)} entries) -> {archive}")
        elif args.command == "validate":
            manifest, entries = inspect_package(args.package)
            print(f"valid: {manifest['id']} {manifest['version']} "
                  f"({len(entries)} entries, platform {manifest['platform']})")
        elif args.command == "install":
            record = install(args.package, args.mods_dir, args.host, args.force)
            print(f"installed {record['id']} {record['version']} -> {args.mods_dir / record['file']}")
        elif args.command == "list":
            index = load_index(args.mods_dir)
            if not index["mods"]:
                print("no installed mods")
            for mod_id in sorted(index["mods"]):
                record = index["mods"][mod_id]
                verified = "verified" if record.get("descriptor_verified") else "unverified"
                print(f"{mod_id:<28} {record['version']:<10} {record['file']:<28} {verified}")
        else:
            print(f"removed {remove(args.mods_dir, args.mod_id)}")
    except PackageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
