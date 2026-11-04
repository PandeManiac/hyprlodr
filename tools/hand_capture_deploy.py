#!/usr/bin/env python3
"""Preflight, deploy, and restore the controlled native hand-capture run.

All backups and the deployment journal live below the repository's ignored
``.tmp/`` directory. The installed archive and executable are never written.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "re"))
sys.path.insert(0, str(ROOT / "tools"))
from pack_crypt import decrypt, load_tables  # noqa: E402
from asset_model import inspect_payload  # noqa: E402

EXPECTED_EXE_SHA256 = "7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611"
HAND_OFFSET = 295758784
HAND_SIZE = 21972
STATE_ROOT = "hyprlodr-state"
RUNTIME_FILES = {
    "hyprlodr-attach.json", "hyprlodr-events.jsonl", "hyprlodr-files.jsonl",
    "hyprlodr-gl.jsonl", "hyprlodr-assets.jsonl", "hyprlodr-tables.bin",
    "steamuser.log",
}
RUNTIME_EXCLUDES = {"hyprlodr-config.json", "hyprlodr-install-manifest.json",
                    "hyprlodr-title-mask.raw"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def compare_untouched(original: Path, replacement: Path, offset: int = HAND_OFFSET,
                      size: int = HAND_SIZE) -> bool:
    if original.stat().st_size != replacement.stat().st_size:
        return False
    with original.open("rb") as left, replacement.open("rb") as right:
        if left.read(offset) != right.read(offset):
            return False
        left.seek(offset + size)
        right.seek(offset + size)
        while True:
            a, b = left.read(1 << 20), right.read(1 << 20)
            if a != b:
                return False
            if not a:
                return True


def preflight(game: Path, build: Path, archive: Path, expected_payload: Path) -> dict:
    game, build, archive = game.resolve(), build.resolve(), archive.resolve()
    errors: list[str] = []
    exe = game / "hyperdemon.exe"
    original = game / "asset" / "asset"
    if not exe.is_file() or sha256(exe) != EXPECTED_EXE_SHA256:
        errors.append("game executable hash does not match the verified build")
    proxy = build / "OpenAL32.dll"
    identity_path = build / "hyprlodr-proxy-build.json"
    try:
        identity = json.loads(identity_path.read_text())
    except (OSError, ValueError):
        identity = {}
        errors.append("proxy build identity manifest is missing or malformed")
    try:
        revision = __import__("subprocess").check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        revision = ""
    if identity.get("schema") != 1 or identity.get("source_revision") != revision:
        errors.append("proxy identity is not for the current source revision")
    if identity.get("source_clean") is not True:
        errors.append("proxy was not built from a clean committed source tree")
    required_caps = {"redirect-wait-v1", "build-identity-v1"}
    if not required_caps.issubset(set(identity.get("capabilities", []))):
        errors.append("proxy lacks required early-redirect/build-identity capabilities")
    if not proxy.is_file() or identity.get("proxy_sha256") != sha256(proxy):
        errors.append("proxy artifact hash does not match its build identity")
    hand_mod = build / "examples" / "example.hand-hook.dll"
    redirect_mod = build / "examples" / "example.asset-redirect.dll"
    for artifact, identity_key in ((hand_mod, "hand_capture_mod_sha256"),
                                   (redirect_mod, "asset_redirect_mod_sha256")):
        if not artifact.is_file() or artifact.stat().st_size == 0:
            errors.append(f"required mod artifact missing: {artifact.name}")
        elif identity.get(identity_key) != sha256(artifact):
            errors.append(f"mod artifact hash does not match build identity: {artifact.name}")
    if not inside(archive, ROOT / ".tmp") or archive == original.resolve():
        errors.append("replacement archive must be a separate file under repository .tmp/")
    if not original.is_file() or not archive.is_file() or archive.stat().st_size != original.stat().st_size:
        errors.append("original or replacement archive is missing or has the wrong size")
    if original.is_file() and archive.is_file() and not compare_untouched(original, archive):
        errors.append("archive differs outside the designated tag-1 hand2 record")
    try:
        if not expected_payload.is_file() or expected_payload.stat().st_size != HAND_SIZE:
            raise ValueError("expected edited payload missing or wrong size")
        t0, t1, _ = load_tables(exe)
        with archive.open("rb") as stream:
            stream.seek(HAND_OFFSET)
            cipher = stream.read(HAND_SIZE)
        plain = decrypt(cipher, t0, t1, 0x2FE)  # CRC-32("hand2") & 0x3ff
        if plain != expected_payload.read_bytes():
            raise ValueError("modified hand2 ciphertext does not decrypt to expected payload")
        model = inspect_payload(plain)
        if model.get("graph_version") != 3 or model.get("vertex_count") != 238 or \
                model.get("index_count") != 1188 or model.get("vertex_stride") != 72:
            raise ValueError("edited hand2 payload structure differs from the validated model")
    except (OSError, ValueError, KeyError, struct.error) as error:
        errors.append(f"archive payload validation failed: {error}")
    return {
        "ok": not errors,
        "game_dir": str(game),
        "executable_sha256": sha256(exe) if exe.is_file() else None,
        "proxy": str(proxy),
        "proxy_sha256": sha256(proxy) if proxy.is_file() else None,
        "proxy_identity": identity,
        "archive": str(archive),
        "archive_sha256": sha256(archive) if archive.is_file() else None,
        "hand2": {"offset": HAND_OFFSET, "size": HAND_SIZE,
                  "version": 3, "vertices": 238, "indices": 1188, "stride": 72},
        "errors": errors,
    }


def backup_one(game: Path, journal_dir: Path, rel: str) -> dict:
    source = game / rel
    exists = source.is_file()
    item = {"path": rel, "existed": exists, "before_sha256": sha256(source) if exists else None}
    if exists:
        backup = journal_dir / "backup" / rel
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, backup)
        if sha256(backup) != item["before_sha256"]:
            raise RuntimeError(f"backup verification failed for {rel}")
    return item


def deploy(args, result: dict) -> int:
    game = args.game_dir.resolve()
    journal_dir = args.journal.resolve()
    if not inside(journal_dir, ROOT / ".tmp"):
        raise RuntimeError("journal must be below repository .tmp/")
    journal_path = journal_dir / "deployment.json"
    if journal_path.exists():
        raise RuntimeError(f"deployment journal already exists: {journal_path}")
    runtime_files = set(RUNTIME_FILES)
    runtime_files.update(path.name for path in game.iterdir()
                         if path.is_file() and path.name.startswith("hyprlodr-")
                         and path.name not in RUNTIME_EXCLUDES)
    relative = [
        "OpenAL32.dll", "mods/example.hand-hook.dll", "mods/example.asset-redirect.dll",
        "mods/disabled.txt", "hyprlodr-config.json",
        f"{STATE_ROOT}/example.hand-hook/hand-mode.txt",
        f"{STATE_ROOT}/example.asset-redirect/asset-redirect.txt",
        *sorted(runtime_files),
    ]
    entries = [backup_one(game, journal_dir, rel) for rel in relative]
    journal = {"schema": 1, "game_dir": str(game), "backup_dir": str(journal_dir / "backup"),
               "runtime_files": sorted(runtime_files), "entries": entries}
    journal_dir.mkdir(parents=True, exist_ok=True)
    journal_path.write_text(json.dumps(journal, indent=2) + "\n")
    try:
        shutil.copy2(args.build / "OpenAL32.dll", game / "OpenAL32.dll")
        shutil.copy2(args.build / "examples" / "example.hand-hook.dll",
                     game / "mods" / "example.hand-hook.dll")
        shutil.copy2(args.build / "examples" / "example.asset-redirect.dll",
                     game / "mods" / "example.asset-redirect.dll")
        disabled = game / "mods" / "disabled.txt"
        lines = disabled.read_text().splitlines() if disabled.exists() else []
        lines = [line for line in lines if line.strip() not in
                 {"example.hand-hook", "example.asset-redirect"}]
        disabled.write_text("\n".join(lines) + ("\n" if lines else ""))
        config_path = game / "hyprlodr-config.json"
        config = json.loads(config_path.read_text()) if config_path.exists() else {}
        config["delay_ms"] = 0
        config_path.write_text(json.dumps(config, indent=2) + "\n")
        for runtime_file in runtime_files:
            (game / runtime_file).unlink(missing_ok=True)
        hand_state = game / STATE_ROOT / "example.hand-hook"
        hand_state.mkdir(parents=True, exist_ok=True)
        capture = ROOT / ".tmp" / "hand-capture" / "runtime" / "hand2.capture"
        capture.parent.mkdir(parents=True, exist_ok=True)
        capture_win = "Z:" + capture.as_posix().replace("/", "\\")
        (hand_state / "hand-mode.txt").write_text(f"capture\n{capture_win}\n")
        redirect_state = game / STATE_ROOT / "example.asset-redirect"
        redirect_state.mkdir(parents=True, exist_ok=True)
        archive_win = "Z:" + result["archive"].replace("/", "\\")
        (redirect_state / "asset-redirect.txt").write_text(archive_win + "\n")
        for item in entries:
            target = game / item["path"]
            item["deployed_sha256"] = sha256(target) if target.is_file() else None
        journal_path.write_text(json.dumps(journal, indent=2) + "\n")
    except Exception:
        restore_journal(journal_path, keep_runtime=False, force=True)
        raise
    print(f"deployment ready; journal: {journal_path}")
    return 0


def restore_journal(journal_path: Path, keep_runtime: bool = True,
                    force: bool = False) -> int:
    journal = json.loads(journal_path.read_text())
    if journal.get("restored") and not force:
        print("deployment was already restored")
        return 0
    game = Path(journal["game_dir"])
    backup_dir = Path(journal["backup_dir"])
    run_dir = journal_path.parent / "runtime-collected"
    conflicts: list[str] = []
    if not force:
        for item in journal["entries"]:
            rel = item["path"]
            if rel in journal["runtime_files"]:
                continue
            target = game / rel
            expected = item.get("deployed_sha256")
            if expected is not None and (not target.is_file() or sha256(target) != expected):
                conflicts.append(rel)
            elif expected is None and target.exists():
                conflicts.append(rel)
        if conflicts:
            print("restore conflicts (left untouched): " + ", ".join(conflicts), file=sys.stderr)
            return 2
    for item in reversed(journal["entries"]):
        rel = item["path"]
        target = game / rel
        if keep_runtime and rel in journal["runtime_files"] and target.is_file():
            collected = run_dir / rel
            collected.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, collected)
        if rel in journal["runtime_files"]:
            pass
        if item["existed"]:
            source = backup_dir / rel
            if not source.is_file() or sha256(source) != item["before_sha256"]:
                conflicts.append(rel + " (backup missing or changed)")
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        else:
            target.unlink(missing_ok=True)
    known = {item["path"] for item in journal["entries"]}
    if keep_runtime:
        for target in game.iterdir():
            if (target.is_file() and target.name.startswith("hyprlodr-") and
                    target.name not in RUNTIME_EXCLUDES and target.name not in known):
                collected = run_dir / target.name
                collected.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, collected)
                target.unlink()
    for rel in (f"{STATE_ROOT}/example.hand-hook/hand-mode.txt",
                f"{STATE_ROOT}/example.asset-redirect/asset-redirect.txt"):
        parent = game / rel
        for directory in (parent.parent, parent.parent.parent):
            try:
                directory.rmdir()
            except OSError:
                pass
    if conflicts:
        print("restore conflicts (left untouched): " + ", ".join(conflicts), file=sys.stderr)
        return 2
    journal["restored"] = True
    journal_path.write_text(json.dumps(journal, indent=2) + "\n")
    print(f"restored experiment-owned installation changes; runtime files copied to {run_dir}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("preflight", "deploy"):
        item = sub.add_parser(command)
        item.add_argument("--game-dir", type=Path, required=True)
        item.add_argument("--build", type=Path, required=True)
        item.add_argument("--archive", type=Path, required=True)
        item.add_argument("--expected-payload", type=Path, required=True)
        if command == "deploy":
            item.add_argument("--journal", type=Path, default=ROOT / ".tmp" / "hand-capture" / "deployment")
    restore = sub.add_parser("restore")
    restore.add_argument("--journal", type=Path, default=ROOT / ".tmp" / "hand-capture" / "deployment" / "deployment.json")
    args = parser.parse_args(argv)
    os.environ.update({"TMPDIR": str(ROOT / ".tmp"), "TMP": str(ROOT / ".tmp"), "TEMP": str(ROOT / ".tmp")})
    if args.command == "restore":
        if not args.journal.is_file():
            parser.error(f"deployment journal not found: {args.journal}")
        return restore_journal(args.journal)
    report = preflight(args.game_dir, args.build, args.archive, args.expected_payload)
    print(json.dumps(report, indent=2))
    if not report["ok"]:
        return 2
    if args.command == "deploy":
        return deploy(args, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
