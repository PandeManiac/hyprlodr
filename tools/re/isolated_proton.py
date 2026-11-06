#!/usr/bin/env python3
"""Stage and run a passive HYPER DEMON diagnostic in an offline Proton sandbox.

The copied game, compatibility prefix, HOME, temporary files, logs and captures
live below ``.tmp/first-load-diagnostic/isolated``. Bubblewrap exposes the
installed Proton runtime read-only while hiding the host HOME, /run and /tmp,
making the host root read-only, and removing network and host IPC access.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SANDBOX = (ROOT / ".tmp/first-load-diagnostic/isolated").resolve()
AUTH_SNAPSHOT_ROOT = (SANDBOX / "authenticated-snapshots").resolve()
EXPECTED_EXE = "7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611"
EXPECTED_ARCHIVE = "2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7"
EXPECTED_OPENAL = "396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4"
APP_ID = "1743850"
IGNORED_NAMES = {"mods", "hyprlodr-state", "OST"}
RUNTIME_PREFIXES = ("hyprlodr-",)
RUNTIME_NAMES = {"berds.log", "steamuser.log"}
AUTH_INSTALL_STATE_NAMES = {
    "OpenAL32.dll", "OpenAL32_real.dll", "hyprlodr-install-manifest.json",
    "hyprlodr-config.json", "hyprlodr-title-mask.raw",
}


def inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def current_source_identity() -> tuple[str, bool]:
    revision = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                       text=True).strip()
    clean = not subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"],
                                         text=True).strip()
    return revision, clean


def inventory(root: Path) -> dict[str, dict[str, Any]]:
    rows = {}
    if not root.exists():
        return rows
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        info = path.lstat()
        if path.is_symlink():
            rows[relative] = {"type": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            rows[relative] = {"type": "file", "size": info.st_size,
                              "sha256": sha256(path), "links": info.st_nlink}
        elif path.is_dir():
            rows[relative] = {"type": "directory"}
        else:
            rows[relative] = {"type": "special"}
    return rows


def _authenticated_state_files(game_root: Path, user_data_root: Path) -> list[tuple[str, Path]]:
    """Return only game-specific save/config and reversible install-state files."""
    rows: list[tuple[str, Path]] = []
    roots = [("user", user_data_root)]
    for prefix, root in roots:
        if not root.exists():
            continue
        if root.is_symlink() or not root.is_dir():
            raise ValueError(f"user state root is linked or not a directory: {root}")
        for directory, names, filenames in os.walk(root, followlinks=False):
            directory_path = Path(directory)
            for name in list(names):
                path = directory_path / name
                if path.is_symlink():
                    raise ValueError(f"user state contains a linked directory: {path}")
                if not path.is_dir():
                    raise ValueError(f"user state contains an unsupported directory entry: {path}")
            for name in filenames:
                path = directory_path / name
                info = path.lstat()
                if not path.is_file() or path.is_symlink() or info.st_nlink != 1:
                    raise ValueError(f"user state contains a linked or unsupported file: {path}")
                rows.append((prefix + "/" + path.relative_to(root).as_posix(), path))
    for name in sorted(AUTH_INSTALL_STATE_NAMES):
        path = game_root / name
        if path.is_symlink():
            raise ValueError(f"game install state is a symlink: {path}")
        if path.exists() and not path.is_file():
            raise ValueError(f"game install state is not a regular file: {path}")
        if path.is_file():
            rows.append(("install/" + name, path))
    mod_root = game_root / "mods"
    if mod_root.exists():
        if mod_root.is_symlink() or not mod_root.is_dir():
            raise ValueError(f"game mod state is not a directory: {mod_root}")
        for directory, names, filenames in os.walk(mod_root, followlinks=False):
            base = Path(directory)
            for name in list(names):
                path = base / name
                if path.is_symlink() or not path.is_dir():
                    raise ValueError(f"game mod state contains a linked or unsupported directory: {path}")
            for name in filenames:
                path = base / name
                info = path.lstat()
                if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
                    raise ValueError(f"game mod state contains a linked or unsupported file: {path}")
                rows.append(("install/mods/" + path.relative_to(mod_root).as_posix(), path))
    for relative, path in rows:
        info = path.lstat()
        if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
            raise ValueError(f"user state contains a linked or unsupported file: {path}")
    return rows


def snapshot_authenticated_state(game_root: Path, user_data_root: Path,
                                 snapshot_id: str) -> dict:
    """Make a private, hash-verified snapshot of game saves and install state.

    This is a copy-only operation. It never changes the Steam prefix or game
    installation. Snapshot files are mode 0600 under a mode 0700 .tmp root.
    """
    if not snapshot_id or any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
                              for character in snapshot_id):
        raise ValueError("snapshot ID may contain only letters, numbers, dash, and underscore")
    if user_data_root.is_symlink():
        raise ValueError(f"user state root is a symlink: {user_data_root}")
    game_root, user_data_root = game_root.resolve(), user_data_root.resolve()
    if not game_root.is_dir() or not user_data_root.is_dir():
        raise ValueError("game installation and game-specific user-data directories must exist")
    if inside(user_data_root, game_root) or inside(game_root, user_data_root):
        raise ValueError("game installation and user-data roots must be separate")
    AUTH_SNAPSHOT_ROOT.mkdir(parents=True, exist_ok=True, mode=0o700)
    if AUTH_SNAPSHOT_ROOT.is_symlink() or not AUTH_SNAPSHOT_ROOT.is_dir():
        raise ValueError(f"snapshot storage root is linked or not a directory: {AUTH_SNAPSHOT_ROOT}")
    os.chmod(AUTH_SNAPSHOT_ROOT, 0o700)
    final_root = AUTH_SNAPSHOT_ROOT / snapshot_id
    if final_root.exists() or final_root.is_symlink():
        raise ValueError(f"refusing to overwrite an authenticated snapshot: {final_root}")
    partial_root = AUTH_SNAPSHOT_ROOT / (".partial-" + uuid.uuid4().hex)
    partial_root.mkdir(mode=0o700)
    os.chmod(partial_root, 0o700)
    try:
        entries = []
        for relative, source in _authenticated_state_files(game_root, user_data_root):
            before = source.stat()
            digest = sha256(source)
            destination = partial_root / "files" / relative
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            os.chmod(destination.parent, 0o700)
            shutil.copyfile(source, destination)
            os.chmod(destination, 0o600)
            after = source.stat()
            copied = sha256(destination)
            if (before.st_size, before.st_mtime_ns, before.st_ino) != \
                    (after.st_size, after.st_mtime_ns, after.st_ino) or copied != digest:
                raise ValueError(f"user data changed while snapshotting: {source}")
            entries.append({"path": relative, "sha256": digest, "size": before.st_size,
                            "mtime_ns": before.st_mtime_ns, "mode": before.st_mode & 0o777})
        manifest = {"schema": 1, "snapshot_id": snapshot_id,
                    "game_root": str(game_root), "user_data_root": str(user_data_root),
                    "created_ns": time.time_ns(), "entries": entries}
        manifest_path = partial_root / "snapshot.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        os.chmod(manifest_path, 0o600)
        os.rename(partial_root, final_root)
        return {"snapshot_root": str(final_root),
                "manifest": str(final_root / "snapshot.json"),
                "entries": len(entries), "safe_permissions": True,
                "source_modified": False}
    except Exception:
        shutil.rmtree(partial_root, ignore_errors=True)
        raise


def verify_authenticated_snapshot(game_root: Path, user_data_root: Path,
                                  manifest_path: Path) -> dict:
    """Verify an existing protected snapshot still matches all selected inputs."""
    if user_data_root.is_symlink():
        return {"valid": False, "reason": "user_state_root_is_symlink"}
    game_root, user_data_root = game_root.resolve(), user_data_root.resolve()
    if manifest_path.is_symlink():
        return {"valid": False, "reason": "snapshot_manifest_is_symlink"}
    manifest_path = manifest_path.absolute()
    if not inside(manifest_path, AUTH_SNAPSHOT_ROOT) or not manifest_path.is_file():
        return {"valid": False, "reason": "snapshot_manifest_missing_or_outside_private_root"}
    if manifest_path.stat().st_mode & 0o777 != 0o600 or \
            manifest_path.parent.stat().st_mode & 0o777 != 0o700:
        return {"valid": False, "reason": "snapshot_permissions_are_not_private"}
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"valid": False, "reason": "snapshot_manifest_malformed"}
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or \
            manifest.get("snapshot_id") != manifest_path.parent.name or \
            manifest.get("game_root") != str(game_root) or \
            manifest.get("user_data_root") != str(user_data_root) or \
            not isinstance(manifest.get("entries"), list):
        return {"valid": False, "reason": "snapshot_identity_mismatch"}
    try:
        current = dict(_authenticated_state_files(game_root, user_data_root))
    except (OSError, ValueError) as error:
        return {"valid": False, "reason": f"current_state_unreadable:{error}"}
    expected = {row.get("path"): row for row in manifest["entries"]
                if isinstance(row, dict) and isinstance(row.get("path"), str)}
    if len(expected) != len(manifest["entries"]) or set(current) != set(expected):
        return {"valid": False, "reason": "game_or_user_state_set_changed"}
    for relative, source in current.items():
        row = expected[relative]
        copy_path = manifest_path.parent / "files" / relative
        try:
            if not isinstance(row.get("size"), int) or row["size"] < 0 or \
                    not isinstance(row.get("sha256"), str) or len(row["sha256"]) != 64 or \
                    source.stat().st_mtime_ns != row.get("mtime_ns") or \
                    source.stat().st_mode & 0o777 != row.get("mode") or \
                    not inside(copy_path, manifest_path.parent) or copy_path.is_symlink() or \
                    copy_path.stat().st_mode & 0o777 != 0o600 or \
                    sha256(source) != row.get("sha256") or sha256(copy_path) != row.get("sha256") or \
                    source.stat().st_size != row.get("size"):
                return {"valid": False, "reason": f"snapshot_hash_mismatch:{relative}"}
        except OSError:
            return {"valid": False, "reason": f"snapshot_file_missing:{relative}"}
    return {"valid": True, "entries": len(expected),
            "snapshot_id": manifest.get("snapshot_id")}


def live_wine_processes(proc_root: Path = Path("/proc")) -> list[dict[str, Any]]:
    """Report Wine/Proton processes without signaling or classifying ownership."""
    rows = []
    try:
        entries = list(proc_root.iterdir())
    except OSError:
        return rows
    for entry in entries:
        if not entry.name.isdigit():
            continue
        base = entry
        try:
            comm = (base / "comm").read_text(errors="replace").strip()
            executable = os.readlink(base / "exe")
            start_ticks = int((base / "stat").read_text().rsplit(")", 1)[1].split()[19])
        except (OSError, ValueError, IndexError):
            continue
        lower = (comm + " " + executable).lower()
        if "wine" in lower or "proton" in lower or comm.lower().endswith(".exe"):
            rows.append({"pid": int(entry.name), "start_ticks": start_ticks,
                         "comm": comm, "executable": executable})
    return sorted(rows, key=lambda row: row["pid"])


def authenticated_preflight(source_game: Path, game_root: Path, proton_root: Path,
                            steam_root: Path, compatdata: Path,
                            original_openal: Path, user_data_root: Path,
                            snapshot_manifest: Path | None,
                            proc_root: Path = Path("/proc")) -> dict:
    """Audit a proposed Steam-authenticated workflow; this function never launches."""
    source_game, game_root = source_game.resolve(), game_root.resolve()
    proton_root, steam_root, compatdata = proton_root.resolve(), steam_root.resolve(), compatdata.resolve()
    original_openal, user_data_root = original_openal.resolve(), user_data_root.resolve()
    try:
        from .first_load_diagnostics import live_steam_processes
    except ImportError:
        from first_load_diagnostics import live_steam_processes
    steam_pids = live_steam_processes(proc_root)
    wine_processes = live_wine_processes(proc_root)
    default_compatdata = steam_root / "steamapps/compatdata" / APP_ID
    snapshot = (verify_authenticated_snapshot(source_game, user_data_root, snapshot_manifest)
               if snapshot_manifest else {"valid": False, "reason": "snapshot_not_supplied"})
    mod_files = []
    mod_root = game_root / "mods"
    if mod_root.is_dir():
        for path in sorted(mod_root.rglob("*")):
            if path.is_file() and not path.is_symlink():
                mod_files.append({"name": path.relative_to(mod_root).as_posix(),
                                  "size": path.stat().st_size, "sha256": sha256(path)})
    source_mods = []
    source_mod_root = source_game / "mods"
    if source_mod_root.is_dir():
        for path in sorted(source_mod_root.rglob("*")):
            if path.is_file() and not path.is_symlink():
                source_mods.append({"name": path.relative_to(source_mod_root).as_posix(),
                                    "size": path.stat().st_size, "sha256": sha256(path)})
    try:
        candidate_plain_tree = _tree_has_plain_files(game_root)
    except OSError:
        candidate_plain_tree = False
    try:
        _validate_proton_tree(proton_root)
        proton_tree_safe = all(not path.is_file() or path.stat().st_nlink == 1
                               for path in proton_root.rglob("*") if not path.is_symlink())
    except (OSError, ValueError):
        proton_tree_safe = False
    stage_manifest_path = game_root.parent / "stage.json"
    try:
        stage_manifest = json.loads(stage_manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        stage_manifest = {}
    stage_runtime_matches = (
        stage_manifest.get("mode") == "vanilla" and
        stage_manifest.get("app_id") == APP_ID and
        Path(stage_manifest.get("game_copy", "")).resolve() == game_root and
        Path(stage_manifest.get("proton_root", "")).resolve() == proton_root and
        stage_manifest.get("source_executable_sha256") == EXPECTED_EXE and
        stage_manifest.get("source_archive_sha256") == EXPECTED_ARCHIVE and
        stage_manifest.get("proton_launcher_sha256") ==
        (sha256(proton_root / "proton") if (proton_root / "proton").is_file() else None))
    checks = {
        "supported_executable": (source_game / "hyperdemon.exe").is_file() and
            sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE,
        "original_archive": (source_game / "asset/asset").is_file() and
            sha256(source_game / "asset/asset") == EXPECTED_ARCHIVE,
        "shipped_openal_source": original_openal.is_file() and sha256(original_openal) == EXPECTED_OPENAL,
        "game_copy_is_separate": source_game != game_root and inside(game_root, SANDBOX),
        "candidate_game_tree_has_no_links": candidate_plain_tree,
        "candidate_executable_matches": (game_root / "hyperdemon.exe").is_file() and
            sha256(game_root / "hyperdemon.exe") == EXPECTED_EXE,
        "candidate_archive_matches": (game_root / "asset/asset").is_file() and
            sha256(game_root / "asset/asset") == EXPECTED_ARCHIVE,
        "candidate_game_copy_is_vanilla": (game_root / "OpenAL32.dll").is_file() and
            sha256(game_root / "OpenAL32.dll") == EXPECTED_OPENAL and
            not mod_files and (not mod_root.exists() or not any(mod_root.iterdir())) and
            not (game_root / "hyprlodr-install-manifest.json").exists() and
            not (game_root / "OpenAL32_real.dll").exists() and
            not any(game_root.rglob("hyprlodr-*")),
        "selected_proton_readable": (proton_root / "proton").is_file() and proton_tree_safe,
        "selected_runtime_matches_vanilla_stage_manifest": stage_runtime_matches,
        "game_runtime_prefix_share_fresh_run_root": game_root.parent == proton_root.parent == compatdata.parent,
        "selected_proton_under_private_storage": inside(proton_root, SANDBOX),
        "candidate_prefix_is_disposable": compatdata != default_compatdata and
            inside(compatdata, SANDBOX) and (not compatdata.exists() or not any(compatdata.iterdir())),
        "candidate_game_data_is_under_selected_prefix": inside(
            compatdata / "pfx/drive_c/users/steamuser/AppData/Roaming/HyperDemon", compatdata),
        "original_game_data_is_outside_candidate_prefix": not inside(user_data_root, compatdata),
        "snapshot_matches_game_and_user_data": snapshot.get("valid") is True,
    }
    blockers = [name for name, passed in checks.items() if not passed]
    # Neither a Steam PID nor environment variables prove login, the app's Steam
    # handoff to a copied executable, Cloud behavior, or which prefix Steam uses.
    blockers.extend([
        "steam_authentication_state_requires_user_confirmation",
        "authenticated_steam_handoff_to_disposable_game_and_prefix_unverified",
        "steam_cloud_behavior_not_proven_disabled_for_this_app",
        "actual_game_data_and_prefix_paths_not_observed_in_authenticated_process",
    ])
    if wine_processes:
        blockers.append("existing_wine_or_proton_processes_must_be_classified_by_the_user")
    return {"schema": 1, "safe_to_launch": False, "launch_attempted": False,
        "mode": "authenticated_user_initiated_audit_only",
        "steam_app_id": APP_ID, "working_directory": str(game_root),
        "source_game": str(source_game), "candidate_game_copy": str(game_root),
        "selected_proton": str(proton_root), "candidate_compatdata": str(compatdata),
        "selected_proton_launcher_sha256": (sha256(proton_root / "proton")
            if (proton_root / "proton").is_file() else None),
        "selected_proton_version": ((proton_root / "version").read_text(errors="replace").strip()
            if (proton_root / "version").is_file() else None),
        "stage_manifest": str(stage_manifest_path),
        "default_compatdata": str(default_compatdata), "user_data_root": str(user_data_root),
        "checks": checks, "blockers": sorted(set(blockers)),
        "steam_processes_observed": steam_pids,
        "wine_or_proton_processes_observed": wine_processes,
        "steam_authentication": "not inferred from process existence",
        "steam_cloud": "unknown; public store metadata and local cache absence are not proof",
        "snapshot": snapshot,
        "candidate_install_state": {"proxy_or_original_dlls": {
            name: (sha256(game_root / name) if (game_root / name).is_file() else None)
            for name in ("OpenAL32.dll", "OpenAL32_real.dll")},
            "install_manifest_present": (game_root / "hyprlodr-install-manifest.json").is_file(),
            "mods": mod_files},
        "source_install_state": {"proxy_or_original_dlls": {
            name: (sha256(source_game / name) if (source_game / name).is_file() else None)
            for name in ("OpenAL32.dll", "OpenAL32_real.dll")},
            "install_manifest_present": (source_game / "hyprlodr-install-manifest.json").is_file(),
            "mods": source_mods},
        "user_action_required": [
            "authenticate in the existing Steam client through its UI",
            "confirm no unrelated game session will be affected",
            "resolve the Steam Cloud and separate-prefix questions before launch",
        ]}


def snapshot_game_user_data(game_root: Path, user_data_root: Path, snapshot_id: str) -> dict:
    """CLI-safe wrapper that snapshots beneath this repository's ignored .tmp tree."""
    return snapshot_authenticated_state(game_root, user_data_root, snapshot_id)


def _ignore(directory: str, names: list[str]) -> set[str]:
    ignored = set()
    for name in names:
        if name in IGNORED_NAMES or name in RUNTIME_NAMES or name.startswith(RUNTIME_PREFIXES):
            ignored.add(name)
        elif name in {"OpenAL32.dll", "OpenAL32_real.dll"} or name.startswith("crash_"):
            ignored.add(name)
        elif name.endswith((".bmp", ".bin")) and name.startswith(("hyprlodr-", "crash_")):
            ignored.add(name)
    return ignored


def _validate_source_tree(source: Path) -> None:
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"source game tree contains a symlink: {path}")
        if not path.is_dir() and not path.is_file():
            raise ValueError(f"source game tree contains a special file: {path}")


def _tree_has_plain_files(source: Path) -> bool:
    try:
        _validate_source_tree(source)
        return all(not path.is_file() or path.stat().st_nlink == 1
                   for path in source.rglob("*"))
    except (OSError, ValueError):
        return False


def _validate_proton_tree(source: Path) -> None:
    """Require Proton's links to remain within the copied runtime tree."""
    for path in source.rglob("*"):
        if path.is_symlink():
            target = path.resolve()
            if not target.exists() or not inside(target, source):
                raise ValueError(f"Proton runtime symlink escapes or is broken: {path}")
        elif not path.is_dir() and not path.is_file():
            raise ValueError(f"Proton runtime contains a special file: {path}")


def stage(source_game: Path, proton_root: Path, proxy: Path | None,
          original_openal: Path, run_root: Path, app_id: str = APP_ID,
          input_probe: Path | None = None, mode: str = "diagnostic") -> dict:
    """Create a byte-verified game/runtime copy without mutating source paths."""
    source_game, proton_root = source_game.resolve(), proton_root.resolve()
    proxy = proxy.resolve() if proxy is not None else None
    original_openal = original_openal.resolve()
    run_root = run_root.resolve()
    if run_root == SANDBOX or SANDBOX not in run_root.parents:
        raise ValueError(f"run directory must be a new child of {SANDBOX}")
    if run_root.exists():
        raise ValueError(f"refusing to overwrite an existing run directory: {run_root}")
    if app_id != APP_ID:
        raise ValueError(f"unexpected Steam app ID for supported HYPER DEMON: {app_id}")
    if not (source_game / "hyperdemon.exe").is_file() or \
            sha256(source_game / "hyperdemon.exe") != EXPECTED_EXE:
        raise ValueError("source game executable does not match the verified supported build")
    if not (source_game / "asset/asset").is_file():
        raise ValueError("source game archive is missing")
    if sha256(source_game / "asset/asset") != EXPECTED_ARCHIVE:
        raise ValueError("source game archive does not match the verified original archive")
    if sha256(original_openal) != EXPECTED_OPENAL:
        raise ValueError("original OpenAL runtime does not match the verified shipped library")
    if mode not in {"diagnostic", "vanilla"}:
        raise ValueError(f"unsupported staging mode: {mode}")
    if mode == "vanilla" and (proxy is not None or input_probe is not None):
        raise ValueError("vanilla mode cannot include a HYPRLODR proxy or input probe")
    for path, label in ((proton_root / "proton", "Proton launcher"),
                        (original_openal, "original OpenAL runtime")):
        if not path.is_file():
            raise ValueError(f"{label} is missing: {path}")
    proxy_identity = None
    identity_path = None
    diagnostic_mod = None
    revision = None
    if mode == "diagnostic":
        if proxy is None:
            raise ValueError("diagnostic mode requires a HYPRLODR proxy")
        proxy = proxy.resolve()
        if not proxy.is_file():
            raise ValueError(f"HYPRLODR proxy is missing: {proxy}")
        identity_path = proxy.parent / "hyprlodr-proxy-build.json"
        try:
            proxy_identity = json.loads(identity_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ValueError("proxy build identity manifest is missing or malformed") from error
        revision, clean = current_source_identity()
        if proxy_identity.get("schema") != 1 or proxy_identity.get("source_revision") != revision or \
                proxy_identity.get("source_clean") is not True or not clean:
            raise ValueError("proxy must be built from the current clean committed source")
        if proxy_identity.get("proxy_sha256") != sha256(proxy):
            raise ValueError("proxy artifact hash does not match its build identity")
        required_caps = {"redirect-wait-v1", "build-identity-v1"}
        if not required_caps.issubset(set(proxy_identity.get("capabilities", []))):
            raise ValueError("proxy lacks required archive redirect and build identity capabilities")
        diagnostic_mod = proxy.parent / "mods/z.foundation.dll"
        if not diagnostic_mod.is_file() or \
                sha256(diagnostic_mod) != proxy_identity.get("foundation_diagnostic_mod_sha256"):
            raise ValueError("repository diagnostic mod does not match the clean proxy build identity")
    _validate_source_tree(source_game)
    proton_fixups = proton_root / "steampipe_fixups.json"
    proton_stamp = proton_root / "files/steampipe_fixups_mtime"
    if proton_fixups.exists() and proton_stamp.exists() and \
            proton_stamp.read_text(encoding="utf-8").strip() != str(proton_fixups.stat().st_mtime):
        raise ValueError("installed Proton has pending fixups; refusing to run a read-only runtime")
    _validate_proton_tree(proton_root)

    run_root.parent.mkdir(parents=True, exist_ok=True)
    run_root.mkdir()
    game = run_root / "game"
    shutil.copytree(source_game, game, ignore=_ignore, copy_function=shutil.copy2)
    if mode == "diagnostic":
        (game / "OpenAL32.dll").write_bytes(proxy.read_bytes())
        shutil.copy2(original_openal, game / "OpenAL32_real.dll")
        (game / "mods").mkdir(exist_ok=True)
        shutil.copy2(diagnostic_mod, game / "mods/z.foundation.dll")
        (game / "hyprlodr-config.json").write_text('{"delay_ms":0}\n', encoding="utf-8")
    else:
        shutil.copy2(original_openal, game / "OpenAL32.dll")
    if input_probe:
        probe = input_probe.resolve()
        if not probe.is_file():
            raise ValueError(f"Win32 input probe is missing: {probe}")
        shutil.copy2(probe, game / "hyprlodr-input-probe.exe")

    for relative in ("compatdata", "home", "xdg/config", "xdg/cache", "xdg/data",
                     "xdg/state", "runtime", "temp", "steam-client", "logs", "evidence"):
        (run_root / relative).mkdir(parents=True, exist_ok=True)
    shutil.copytree(proton_root, run_root / "proton", symlinks=True,
                    copy_function=shutil.copy2,
                    ignore=shutil.ignore_patterns("user_settings.py"))
    copied_proton = run_root / "proton"
    _validate_proton_tree(copied_proton)
    exe_hash = sha256(source_game / "hyperdemon.exe")
    archive_hash = sha256(source_game / "asset/asset")
    if sha256(game / "hyperdemon.exe") != exe_hash or \
            sha256(game / "asset/asset") != archive_hash:
        raise RuntimeError("copied executable/archive verification failed")
    if mode == "diagnostic":
        if sha256(game / "OpenAL32.dll") != sha256(proxy):
            raise RuntimeError("proxy copy verification failed")
        if sha256(game / "OpenAL32_real.dll") != sha256(original_openal):
            raise RuntimeError("original OpenAL copy verification failed")
    elif sha256(game / "OpenAL32.dll") != EXPECTED_OPENAL:
        raise RuntimeError("vanilla OpenAL copy verification failed")
    meta = {"schema": 1, "mode": mode, "hyprlodr_present": mode == "diagnostic",
        "app_id": app_id, "source_game": str(source_game),
        "game_copy": str(game), "proton_root": str(copied_proton),
        "source_proton_root": str(proton_root),
        "proton_launcher_sha256": sha256(copied_proton / "proton"),
        "source_proton_launcher_sha256": sha256(proton_root / "proton"),
        "proton_runtime_isolated_copy": True,
        "source_executable_sha256": exe_hash, "source_archive_sha256": archive_hash,
        "source_installed_openal_sha256": sha256(source_game / "OpenAL32.dll"),
        "source_openal_runtime_path": str(original_openal),
        "source_openal_runtime_sha256": sha256(original_openal),
        "copied_executable_sha256": sha256(game / "hyperdemon.exe"),
        "copied_archive_sha256": sha256(game / "asset/asset"),
        "staged_openal_sha256": sha256(game / "OpenAL32.dll"),
        "replacement_routing": False, "user_mods_copied": False,
        "excluded_source_entries": sorted(IGNORED_NAMES | RUNTIME_NAMES)}
    if mode == "diagnostic":
        meta.update({"proxy_build_manifest": str(identity_path),
            "proxy_sha256": sha256(game / "OpenAL32.dll"),
            "diagnostic_mod_sha256": sha256(game / "mods/z.foundation.dll"),
            "diagnostic_mod_id": "z.foundation",
            "diagnostic_mod_source": "repository sample; host state/capability/log only",
            "original_openal_sha256": sha256(game / "OpenAL32_real.dll"),
            "proxy_source_revision": proxy_identity["source_revision"]})
    (run_root / "stage.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return meta


def check_runtime(run_root: Path, source_game: Path, bwrap: str | Path | None = None,
                  proc_root: Path = Path("/proc"), allow_claimed: bool = False) -> dict:
    try:
        from .first_load_diagnostics import (
            live_steam_processes, probe_bubblewrap_isolation, reject_external_links)
    except ImportError:
        from first_load_diagnostics import (
            live_steam_processes, probe_bubblewrap_isolation, reject_external_links)

    run_root, source_game = run_root.resolve(), source_game.resolve()
    if run_root == SANDBOX or SANDBOX not in run_root.parents:
        raise ValueError(f"run directory must be a child of {SANDBOX}")
    meta_path = run_root / "stage.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    game = run_root / "game"
    proton = Path(meta["proton_root"])
    source_proton = Path(meta["source_proton_root"])
    mode = meta.get("mode", "diagnostic")
    if mode not in {"diagnostic", "vanilla"}:
        raise ValueError("stage manifest has an unknown launch mode")
    proxy_identity = None
    source_revision, source_clean = current_source_identity()
    if mode == "diagnostic":
        proxy_identity = json.loads(Path(meta["proxy_build_manifest"]).read_text(encoding="utf-8"))
    roots = {name: run_root / name for name in
             ("game", "compatdata", "home", "temp", "steam-client", "proton")}
    strict_roots = {name: path for name, path in roots.items() if name != "proton"}
    reject_external_links(strict_roots, run_root)
    reject_external_links({"proton": roots["proton"]}, run_root,
                          allowed_symlink_root=roots["proton"])
    reject_external_links({"complete_run_tree": run_root}, run_root,
                          allowed_symlink_root=roots["proton"])
    for path in roots.values():
        path.mkdir(parents=True, exist_ok=True)
    if source_game == game or source_game in game.parents or game in source_game.parents:
        raise ValueError("copied game path is not distinct from installed game")
    checks = {"source_executable_unchanged": sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE,
        "game_executable_matches_stage": sha256(game / "hyperdemon.exe") == meta["copied_executable_sha256"],
        "game_archive_matches_verified_original": sha256(game / "asset/asset") == EXPECTED_ARCHIVE and
            meta.get("copied_archive_sha256") == EXPECTED_ARCHIVE,
        "stage_mode_is_explicit": meta.get("mode") == mode and
            meta.get("hyprlodr_present") is (mode == "diagnostic"),
        "routing_disabled": meta.get("replacement_routing") is False,
        "proton_readable": (proton / "proton").is_file(),
        "proton_is_disposable_copy": proton.resolve() == roots["proton"].resolve() and
            meta.get("proton_runtime_isolated_copy") is True,
        "proton_source_launcher_unchanged": sha256(source_proton / "proton") ==
            meta.get("source_proton_launcher_sha256"),
        "proton_copy_launcher_matches_stage": sha256(proton / "proton") ==
            meta.get("proton_launcher_sha256"),
        "fresh_compatdata_prefix": not any(roots["compatdata"].iterdir()),
        "run_not_previously_claimed": (
            not (run_root / "launch.claimed").exists() if not allow_claimed else
            (run_root / "launch.claimed").read_text(encoding="ascii").splitlines()[0] ==
            f"pid={os.getpid()}"),
        "run_directory_separate": game != source_game}
    if mode == "diagnostic":
        checks.update({
            "proxy_matches_build_identity": sha256(game / "OpenAL32.dll") ==
                proxy_identity.get("proxy_sha256") and
                meta.get("proxy_sha256") == proxy_identity.get("proxy_sha256"),
            "diagnostic_mod_matches_build_identity":
                sha256(game / "mods/z.foundation.dll") ==
                proxy_identity.get("foundation_diagnostic_mod_sha256") ==
                meta.get("diagnostic_mod_sha256") and meta.get("diagnostic_mod_id") == "z.foundation",
            "proxy_built_from_current_clean_source": source_clean and
                proxy_identity.get("source_clean") is True and
                proxy_identity.get("source_revision") == source_revision == meta.get("proxy_source_revision")})
    else:
        hyprlodr_artifacts = [path.relative_to(game).as_posix() for path in game.rglob("*")
                              if path.name == "mods" or path.name.startswith("hyprlodr-") or
                              path.name == "hyprlodr-config.json" or
                              path.name == "OpenAL32_real.dll"]
        checks.update({
            "vanilla_original_openal_staged": sha256(game / "OpenAL32.dll") == EXPECTED_OPENAL and
                meta.get("staged_openal_sha256") == EXPECTED_OPENAL,
            "vanilla_hyprlodr_artifacts_absent": not hyprlodr_artifacts,
            "vanilla_manifest_declares_absence": meta.get("hyprlodr_present") is False and
                meta.get("replacement_routing") is False and meta.get("user_mods_copied") is False})
    original_archive_hash = sha256(source_game / "asset/asset")
    checks["source_archive_unchanged"] = original_archive_hash == meta["source_archive_sha256"]
    checks["source_installed_openal_unchanged"] = sha256(source_game / "OpenAL32.dll") == \
        meta["source_installed_openal_sha256"]
    checks["source_openal_runtime_unchanged"] = sha256(Path(meta["source_openal_runtime_path"])) == \
        meta["source_openal_runtime_sha256"] == EXPECTED_OPENAL
    if not all(checks.values()):
        failed = [name for name, passed in checks.items() if not passed]
        raise ValueError("isolated Proton preflight failed: " + ", ".join(failed))
    isolation = probe_bubblewrap_isolation(run_root, roots["home"], bwrap)
    processes = live_steam_processes(proc_root)
    return {"schema": 1, "safe": True, "run_root": str(run_root),
        "checks": checks, "bubblewrap": isolation,
        "active_steam_pids_observed": processes,
        "active_steam_policy": "recorded; inaccessible via isolated home, runtime, mount and network namespaces",
        "cloud": "blocked by no client IPC and no network route",
        "authentication": "not supplied; game may reject offline direct Proton startup",
        "experiment_mode": mode,
        "hyprlodr_absent": mode == "vanilla",
        "writable_host_roots": [str(run_root)],
        "game_process_will_be_network_isolated": True}


_SUPERVISOR = r'''import json, os, re, select, shutil, signal, subprocess, sys, time
root, proton, game, run_seconds, run_mode = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
evidence = os.path.join(root, "evidence")
logs = os.path.join(root, "logs")
for path in (evidence, logs): os.makedirs(path, exist_ok=True)
events_path = os.path.join(evidence, "runtime-events.jsonl")
process_path = os.path.join(evidence, "process-lifecycle.jsonl")
window_path = os.path.join(evidence, "window-lifecycle.jsonl")
sequence = 0
start = time.monotonic()
supervisor_run_id = "%x-%x" % (os.getpid(), time.monotonic_ns())
processes = {}
game_seen = set()
supervisor_signals = {}
def event(name, **fields):
    global sequence
    sequence += 1
    row = {"seq": sequence, "run_id": supervisor_run_id,
        "t_ms": int((time.monotonic()-start)*1000), "event": name, **fields}
    with open(events_path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(row)+"\n"); stream.flush()
    print(json.dumps(row), flush=True)
    if name.startswith("process_") or name in ("proton_wrapper_exit", "process_tree_complete"):
        with open(process_path, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row)+"\n"); stream.flush()
    if name.startswith("window_") or name.startswith("display_inventory") or name == "screen_capture":
        with open(window_path, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row)+"\n"); stream.flush()

ticks = os.sysconf("SC_CLK_TCK")
boot_time = 0
try:
    with open("/proc/stat", encoding="ascii") as stream:
        boot_time = int(next(line.split()[1] for line in stream if line.startswith("btime ")))
except Exception: pass
def scan_processes():
    found = {}
    try: entries = os.listdir("/proc")
    except OSError: return found
    for entry in entries:
        if not entry.isdigit(): continue
        pid = int(entry); base = "/proc/" + entry
        try:
            raw = open(base + "/stat", "rb").read().decode("utf-8", "replace")
            end = raw.rfind(")")
            fields = raw[end+2:].split()
            state, ppid, start_ticks = fields[0], int(fields[1]), int(fields[19])
            argv = open(base + "/cmdline", "rb").read().split(b"\0")
            argv = [item.decode("utf-8", "replace") for item in argv if item]
            cmdline = " ".join(argv)
            exe = os.readlink(base + "/exe")
            comm = open(base + "/comm", "rb").read().decode("utf-8", "replace").strip()
        except (OSError, ValueError, IndexError): continue
        role_args = argv if len(argv) < 2 or argv[1] != "-c" else argv[:1]
        roles = []
        proton_real = os.path.realpath(proton)
        is_proton = os.path.realpath(exe) == proton_real or any(
            os.path.realpath(arg) == proton_real for arg in argv[:3])
        steam_images = ("steam.exe", "steamservice", "steamservice.exe",
                        "steamclient", "steamclient.exe")
        image_names = {comm.lower(), os.path.basename(exe).lower()}
        image_names.update(os.path.basename(arg.replace("\\", "/")).lower() for arg in argv[:2])
        is_steam_compat = bool(image_names.intersection(steam_images))
        target_arg = any(arg.lower().replace("/", "\\").endswith("\\hyperdemon.exe") and
                         "\\game\\" in arg.lower().replace("/", "\\") for arg in argv)
        # Wine normally exposes the Windows image name through /proc/<pid>/comm
        # while /proc/<pid>/exe remains wine-preloader or a Unix helper. Keep
        # command-line matching as a fallback for Wine builds that preserve it.
        is_game = not is_proton and not is_steam_compat and \
            (comm.lower() == "hyperdemon.exe" or target_arg)
        if is_game: roles.append("hyperdemon")
        if any(arg.lower().replace("\\", "/").endswith("/xalia.exe") for arg in role_args): roles.append("xalia")
        if "wineserver" in os.path.basename(exe).lower(): roles.append("wineserver")
        if is_steam_compat: roles.append("steam_compat")
        if is_proton: roles.append("proton")
        if "wine" in os.path.basename(exe).lower() or "wine" in role_args[0].lower(): roles.append("wine")
        if not roles: continue
        key = str(pid) + ":" + str(start_ticks)
        found[key] = {"pid":pid,"ppid":ppid,"state":state,"start_ticks":start_ticks,
            "start_epoch_s":boot_time + start_ticks / ticks if boot_time else None,
            "exe":exe,"comm":comm,"cmdline":cmdline,"roles":sorted(set(roles)),"game_process":is_game}
    return found

def observe_processes(now_ms):
    current = scan_processes()
    for key, old in list(processes.items()):
        if key not in current:
            event("process_disappeared", identity=key, pid=old["pid"], ppid=old["ppid"],
                roles=old["roles"], last_state=old["state"], first_seen_ms=old["first_seen_ms"],
                last_seen_ms=old["last_seen_ms"], exit_code=None, exit_signal=None,
                exit_observation="not_available_from_procfs",
                supervisor_terminated=supervisor_signals.get(key, False))
            del processes[key]
    for key, sample in current.items():
        if key not in processes:
            sample["first_seen_ms"] = now_ms
            sample["last_seen_ms"] = now_ms
            processes[key] = sample
            event("process_started_observed", identity=key, **sample)
        else:
            tracked = processes[key]
            tracked.update({"state":sample["state"],"last_seen_ms":now_ms,
                            "ppid":sample["ppid"],"cmdline":sample["cmdline"],"exe":sample["exe"],
                            "comm":sample["comm"]})
        if sample["game_process"]: game_seen.add(key)
    active_games = {key: value for key,value in processes.items() if value["game_process"]}
    return current, active_games

def proc_identity_alive(key):
    try:
        pid_text, ticks_text = key.split(":", 1)
        raw = open("/proc/"+pid_text+"/stat", "rb").read().decode("utf-8", "replace")
        return int(raw[raw.rfind(")")+2:].split()[19]) == int(ticks_text)
    except (OSError, ValueError, IndexError): return False

read_fd, write_fd = os.pipe()
xvfb_log = open(os.path.join(logs, "xvfb.log"), "wb")
xvfb = subprocess.Popen(["/usr/bin/Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1920x1080x24",
    "+extension", "GLX", "+extension", "RANDR", "+render", "-noreset", "-nolisten", "tcp", "-ac"],
    pass_fds=(write_fd,), stdout=xvfb_log, stderr=subprocess.STDOUT, close_fds=True)
os.close(write_fd)
display = None
ready_deadline = time.monotonic() + 10
buffer = b""
while time.monotonic() < ready_deadline and xvfb.poll() is None:
    ready, _, _ = select.select([read_fd], [], [], 0.1)
    if ready:
        buffer += os.read(read_fd, 64)
        if b"\n" in buffer:
            value = buffer.splitlines()[0].decode().strip()
            if value.isdigit(): display = ":" + value
            break
os.close(read_fd)
if not display:
    event("xvfb_start_failed", returncode=xvfb.poll())
    if xvfb.poll() is None: xvfb.terminate(); xvfb.wait(timeout=5)
    sys.exit(20)
env = os.environ.copy()
env.update({"DISPLAY": display, "XAUTHORITY": "/dev/null"})
env.pop("WAYLAND_DISPLAY", None)
probe = None
for _ in range(100):
    probe = subprocess.run(["/usr/bin/xdpyinfo", "-display", display], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if probe.returncode == 0: break
    if xvfb.poll() is not None: break
    time.sleep(0.05)
if probe is None or probe.returncode != 0:
    event("xvfb_readiness_failed", display=display)
    xvfb.terminate(); xvfb.wait(timeout=5); sys.exit(21)
event("xvfb_ready", display=display, pid=xvfb.pid, resolution="1920x1080x24", glx=True, randr=True)
wm = None
wm_log = None
openbox = shutil.which("openbox")
if openbox:
    wm_log = open(os.path.join(logs, "window-manager.log"), "wb")
    wm = subprocess.Popen([openbox, "--sm-disable"], env=env, stdout=wm_log,
        stderr=subprocess.STDOUT, close_fds=True)
    event("window_manager_started", pid=wm.pid)

def window_inventory():
    xwininfo = shutil.which("xwininfo")
    wmctrl = shutil.which("wmctrl")
    if not xwininfo: return None, []
    try:
        root_result = subprocess.run([xwininfo,"-display",display,"-root"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
        result = subprocess.run([wmctrl,"-l","-p","-G"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
    except subprocess.TimeoutExpired:
        return {"returncode":None,"stderr":"xwininfo_timeout"}, []
    rows=[]
    clients=[]
    if wmctrl and result.returncode == 0:
        for line in result.stdout.splitlines():
            match=re.match(r"^(0x[0-9a-fA-F]+)\s+(-?\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+\S+\s+(.*)$",line)
            if match:
                wid,pid,x,y,width,height,desktop,title=match.groups()
                clients.append((wid,int(pid) if pid.isdigit() else None,title,
                    int(x),int(y),int(width),int(height)))
    else:
        # xwininfo's root tree indents direct children by two spaces and
        # nested toolkit children more deeply; keep only actual root clients.
        tree=subprocess.run([xwininfo,"-display",display,"-root","-tree"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
        for line in tree.stdout.splitlines():
            match=re.match(r"^  (0x[0-9a-fA-F]+)\s+(.+)$",line)
            if match: clients.append((match.group(1),None,match.group(2),None,None,None,None))
        result=tree
    for wid,wm_pid,wm_title,x,y,width,height in clients:
        try:
            prop=subprocess.run(["xprop","-display",display,"-id",wid,"WM_CLASS","_NET_WM_PID","_NET_WM_NAME","WM_NAME"],
                env=env,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,timeout=2,check=False)
        except subprocess.TimeoutExpired: continue
        info=prop.stdout.strip()
        try:
            detail=subprocess.run([xwininfo,"-display",display,"-id",wid],env=env,
                stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,timeout=2,check=False)
        except subprocess.TimeoutExpired: continue
        visible="IsViewable" in detail.stdout
        def xint(label):
            found=re.search(r"^\s*"+label+r":\s*(-?\d+)",detail.stdout,re.M)
            return int(found.group(1)) if found else None
        pid_match=re.search(r"_NET_WM_PID\(CARDINAL\)\s*=\s*(\d+)",info)
        x_pid=int(pid_match.group(1)) if pid_match else wm_pid
        title=(wm_title or "")
        rows.append({"id":wid,"title":title,"properties":info,"x_pid":x_pid,
            "visible":visible,"x":x if x is not None else xint("Absolute upper-left X"),
            "y":y if y is not None else xint("Absolute upper-left Y"),
            "width":width if width is not None else xint("Width"),
            "height":height if height is not None else xint("Height"),
            "game_candidate":("hyperdemon" in (title+" "+info).lower())})
    return {"returncode":result.returncode,"stderr":result.stderr.strip(),
        "root":root_result.stdout.strip()}, rows

def window_identity(row):
    """Fields that distinguish a new X client when the server reuses a window ID."""
    match=re.search(r'WM_CLASS\(STRING\)\s*=\s*"([^"]*)"\s*,\s*"([^"]*)"',
        row.get("properties", ""))
    window_class=tuple(match.groups()) if match else None
    process_identities=tuple(process.get("identity") for process in
                              row.get("associated_processes", []))
    return row.get("x_pid"), process_identities, window_class

def enrich_window_association(row):
    associated=[p for p in processes.values() if row["x_pid"] and p["pid"]==row["x_pid"]]
    row["associated_processes"]=[{"pid":p["pid"],"roles":p["roles"],
        "identity":str(p["pid"])+":"+str(p["start_ticks"])} for p in associated]
    row["game_process_associated"]=any(p["game_process"] for p in associated)
    return row

def capture_screen(reason, max_captures=6):
    if capture_screen.count >= max_captures: return
    xwd=shutil.which("xwd")
    if not xwd:
        event("screen_capture",reason=reason,ok=False,error="xwd_unavailable"); return
    path=os.path.join(evidence,"screen-%02d.xwd"%capture_screen.count)
    shot=subprocess.run([xwd,"-display",display,"-root","-silent","-out",path],env=env,
        stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=5,check=False)
    capture_screen.count += 1
    event("screen_capture",reason=reason,path=path,ok=shot.returncode==0,
        error=shot.stderr.decode("utf-8","replace") if shot.returncode else "")
capture_screen.count=0

seen_windows={}
next_window_scan=0.0
fallback_capture_at=time.monotonic()+10
next_fallback_capture=fallback_capture_at
attach_paths=set()
def observe_attach():
    for path in sorted(__import__("glob").glob(os.path.join(game,"hyprlodr-attach-*.json"))):
        if path in attach_paths: continue
        attach_paths.add(path)
        try: row=json.load(open(path,encoding="utf-8"))
        except Exception as error:
            event("hyprlodr_attach_unreadable",path=path,error=str(error)); continue
        event("hyprlodr_attach_observed",path=path,hyprlodr_run_id=row.get("run_id"),
            windows_pid=row.get("pid"),stage=row.get("stage"),build_matches=row.get("build_matches"),
            anchors_total=row.get("anchors_total"),anchors_matched=row.get("anchors_matched"),
            early_hook=row.get("experimental_early_hook_status"),
            relevant_linux_processes=[p for p in processes.values() if p["game_process"]])

proton_log = open(os.path.join(logs, "proton-console.log"), "wb")
env["WINEDEBUG"] = "+file,+seh" if run_mode == "vanilla" else "+seh"
proc = subprocess.Popen([proton, "run", os.path.join(game, "hyperdemon.exe")], cwd=game,
    env=env, stdin=subprocess.DEVNULL, stdout=proton_log, stderr=subprocess.STDOUT,
    close_fds=True, start_new_session=True)
wrapper_started=time.monotonic()
event("proton_game_started", pid=proc.pid, ppid=os.getpid(), start_monotonic_ns=time.monotonic_ns(),
    app_id=os.environ.get("STEAM_COMPAT_APP_ID"),game=game,
    compat_data=os.environ.get("STEAM_COMPAT_DATA_PATH"),
    steam_client_ipc="unavailable",network="isolated",stdin="closed_noninteractive")
stop_at = wrapper_started + run_seconds
wrapper_exit_at=None
wrapper_exit_code=None
wrapper_outlived_game=False
window_identities=set()
termination_reason=None
scan_period=0.1
next_capture = wrapper_started + 15
last_window_report=None
try:
    while True:
        now=time.monotonic(); now_ms=int((now-start)*1000)
        current, active_games=observe_processes(now_ms)
        observe_attach()
        if now >= next_window_scan:
            display_result, rows=window_inventory()
            if display_result is not None:
                new={row["id"]:row for row in rows}
                for wid,row in new.items():
                    previous=seen_windows.get(wid)
                    row=enrich_window_association(row)
                    new[wid]=row
                    reused=previous is not None and window_identity(previous)!=window_identity(row)
                    if reused:
                        event("window_disappeared",window=previous,display=display,
                            reason="xid_reused_for_different_client")
                    if previous is None or reused:
                        event("window_created",window=row,display=display)
                        if row["game_candidate"] or row["game_process_associated"]:
                            capture_screen("candidate_game_window")
                    elif (previous.get("title")!=row.get("title") or
                          previous.get("properties")!=row.get("properties") or
                          previous.get("visible")!=row.get("visible") or
                          previous.get("game_candidate")!=row.get("game_candidate") or
                          previous.get("game_process_associated")!=row.get("game_process_associated")):
                        event("window_updated",previous=previous,window=row,display=display)
                        candidate_promoted=(not previous.get("game_candidate") and
                                            row.get("game_candidate"))
                        process_promoted=(not previous.get("game_process_associated") and
                                          row.get("game_process_associated"))
                        if candidate_promoted or process_promoted:
                            capture_screen("candidate_game_window")
                for wid,row in seen_windows.items():
                    if wid not in new: event("window_disappeared",window=row,display=display)
                seen_windows=new
                inventory_row={"display":display,"root":display_result,"windows":rows}
                if inventory_row != last_window_report:
                    event("display_inventory",**inventory_row)
                    last_window_report=inventory_row
            next_window_scan=now+0.5
        if now >= next_capture:
            event("supervisor_heartbeat",proton_pid=proc.pid,proton_running=proc.poll() is None,
                active_game_identities=sorted(active_games),elapsed_seconds=int(now-start))
            next_capture += 15
        if now >= next_fallback_capture:
            capture_screen("bounded_fallback"); next_fallback_capture += 30

        code=proc.poll()
        if code is not None and wrapper_exit_at is None:
            wrapper_exit_at=now; wrapper_exit_code=code
            event("proton_wrapper_exit",pid=proc.pid,returncode=code,
                exit_kind="signal" if code<0 else "status",signal=-code if code<0 else None,
                active_game_identities=sorted(active_games))
            wrapper_outlived_game=bool(active_games)
            if wrapper_outlived_game:
                event("proton_wrapper_returned_before_game",proton_pid=proc.pid,
                    active_game_identities=sorted(active_games))
        if wrapper_exit_at is not None and game_seen and not active_games:
            termination_reason="observed_game_process_exit"; break
        if wrapper_exit_at is not None and not game_seen and now-wrapper_exit_at>=5.0:
            termination_reason="no_game_process_observed_after_wrapper_grace"; break
        if now >= stop_at:
            termination_reason="run_limit"; break
        time.sleep(scan_period)

    # Preserve the final visible state before any cleanup can close Wine windows.
    capture_screen("pre_cleanup_final")
    display_result, rows=window_inventory()
    if display_result is not None:
        event("display_inventory_final",display=display,root=display_result,windows=rows)
    event("process_tree_complete",reason=termination_reason,proton_pid=proc.pid,
        proton_returncode=proc.poll(),game_processes_ever_observed=sorted(game_seen),
        game_process_active_at_wrapper_exit=wrapper_outlived_game,
        supervisor_termination_required=termination_reason=="run_limit")

    if termination_reason=="run_limit":
        current, active_games=observe_processes(int((time.monotonic()-start)*1000))
        for key,p in list(active_games.items()):
            if not proc_identity_alive(key): continue
            try:
                os.kill(p["pid"],signal.SIGTERM); supervisor_signals[key]=True
                event("process_supervisor_signal",identity=key,pid=p["pid"],roles=p["roles"],signal="SIGTERM")
            except ProcessLookupError: pass
        term_deadline=time.monotonic()+5
        while time.monotonic()<term_deadline:
            _,active_games=observe_processes(int((time.monotonic()-start)*1000))
            if not active_games: break
            time.sleep(scan_period)
        for key,p in list(active_games.items()):
            if proc_identity_alive(key):
                try:
                    os.kill(p["pid"],signal.SIGKILL); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],roles=p["roles"],signal="SIGKILL")
                except ProcessLookupError: pass
    elif proc.poll() is None:
        # This can occur only if the overall limit elapsed before Proton returned.
        event("process_supervisor_signal",identity=str(proc.pid),pid=proc.pid,signal="SIGTERM",role="proton")
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            event("process_supervisor_signal",identity=str(proc.pid),pid=proc.pid,signal="SIGKILL",role="proton")
            proc.kill(); proc.wait(timeout=2)
    event("game_exit_observation",wrapper_returncode=wrapper_exit_code,
        game_exit_code=None,game_exit_status="unknown_unless_waitable_child",
        reason=termination_reason,supervisor_terminated_game=termination_reason=="run_limit")
finally:
    # Wrapper return alone is never a cleanup condition while the game remains.
    # wineserver -k is scoped by the private WINEPREFIX and runs only after the
    # observed game process has exited, or after explicit run-limit cleanup.
    wineserver=os.path.join(proton,"files/bin/wineserver")
    if proc.poll() is None or any(p["game_process"] for p in processes.values()):
        try:
            _, active_games=observe_processes(int((time.monotonic()-start)*1000))
            for key,p in active_games.items():
                if proc_identity_alive(key):
                    os.kill(p["pid"],signal.SIGTERM); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],
                        roles=p["roles"],signal="SIGTERM",reason="supervisor_cleanup")
            grace=time.monotonic()+2
            while time.monotonic()<grace:
                _,active_games=observe_processes(int((time.monotonic()-start)*1000))
                if not active_games: break
                time.sleep(scan_period)
            for key,p in active_games.items():
                if proc_identity_alive(key):
                    os.kill(p["pid"],signal.SIGKILL); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],
                        roles=p["roles"],signal="SIGKILL",reason="supervisor_cleanup")
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=2)
        except Exception as error: event("owned_process_cleanup_failed",error=str(error))
    if os.path.isfile(wineserver):
        try:
            result=subprocess.run([wineserver,"-k"],env=env,stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,timeout=10,check=False)
            event("wineserver_cleanup",returncode=result.returncode,after_game_observation=True)
        except Exception as error: event("wineserver_cleanup_failed",error=str(error))
    capture_screen("final_before_display_shutdown")
    display_result,rows=window_inventory()
    if display_result is not None: event("display_inventory_cleanup",display=display,root=display_result,windows=rows)
    if wm is not None and wm.poll() is None: wm.terminate()
    if wm is not None:
        try: wm.wait(timeout=5)
        except subprocess.TimeoutExpired: wm.kill(); wm.wait(timeout=2)
    if wm_log is not None: wm_log.close()
    if xvfb.poll() is None: xvfb.terminate()
    try: xvfb.wait(timeout=5)
    except subprocess.TimeoutExpired: xvfb.kill(); xvfb.wait(timeout=2)
    proton_log.close(); xvfb_log.close()
    event("owned_runtime_stopped",xvfb_returncode=xvfb.returncode,
        process_poll_interval_ms=int(scan_period*1000),supervisor_run_id=supervisor_run_id)
    summary={"schema":1,"supervisor_run_id":supervisor_run_id,"proton_pid":proc.pid,
        "proton_returncode":proc.poll(),"proton_outlived_by_game":wrapper_outlived_game,
        "game_processes_ever_observed":sorted(game_seen),"termination_reason":termination_reason,
        "game_exit_code":None,"game_exit_code_available":False,
        "supervisor_terminated_game":termination_reason=="run_limit",
        "process_poll_interval_ms":int(scan_period*1000),
        "window_inventory_path":window_path,"process_events_path":process_path}
    with open(os.path.join(evidence,"process-lifecycle.json"),"w",encoding="utf-8") as stream:
        json.dump(summary,stream,indent=2);stream.write("\n")
'''


def launch(run_root: Path, source_game: Path, duration: int = 150,
           bwrap: str | Path | None = None) -> int:
    if run_root.is_symlink():
        raise ValueError("refusing a symlinked run directory")
    run_root, source_game = run_root.resolve(), source_game.resolve()
    guard = run_root / "launch.claimed"
    try:
        descriptor = os.open(guard, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as error:
        raise ValueError("this disposable run has already been claimed; stage a fresh run") from error
    with os.fdopen(descriptor, "w", encoding="ascii") as stream:
        stream.write(f"pid={os.getpid()}\nclaimed_ns={time.time_ns()}\n")
    try:
        return _launch_claimed(run_root, source_game, duration, bwrap)
    except Exception as error:
        evidence = run_root / "evidence"
        if not evidence.is_symlink() and inside(evidence, run_root):
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / "launch-failure.json").write_text(json.dumps({
                "schema": 1, "launch_prevented_or_failed": True,
                "reason": f"{type(error).__name__}: {error}",
                "run_claimed": True, "retry_requires_fresh_stage": True}, indent=2) + "\n",
                encoding="utf-8")
        raise


def _launch_claimed(run_root: Path, source_game: Path, duration: int,
                    bwrap: str | Path | None) -> int:
    report = check_runtime(run_root, source_game, bwrap, allow_claimed=True)
    bwrap_bin = str(bwrap or shutil.which("bwrap"))
    meta = json.loads((run_root / "stage.json").read_text(encoding="utf-8"))
    proton = Path(meta["proton_root"]).resolve()
    if duration <= 0 or duration > 600:
        raise ValueError("runtime duration must be between 1 and 600 seconds")
    if not (run_root / "steam-client").is_dir():
        raise ValueError("isolated Steam client placeholder is missing")
    current_home = Path.home().resolve()
    if not inside(current_home, Path("/home")):
        raise ValueError("HOME is not under /home; cannot safely mask the host profile")
    user_home_target = f"/home/{current_home.name}"
    args = [bwrap_bin, "--die-with-parent", "--new-session", "--unshare-net", "--unshare-pid",
        "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
        "--tmpfs", "/run", "--tmpfs", "/tmp", "--dir", "/tmp/hyprlodr",
        "--bind", str(run_root), "/tmp/hyprlodr",
        "--bind", str(run_root / "home"), user_home_target,
        "--", sys.executable, "-c", _SUPERVISOR,
        "/tmp/hyprlodr", "/tmp/hyprlodr/proton/proton", "/tmp/hyprlodr/game",
        str(duration), meta.get("mode", "diagnostic")]
    env = {"PATH": "/usr/bin:/bin", "HOME": "/tmp/hyprlodr/home",
        "USER": os.environ.get("USER", current_home.name),
        "LOGNAME": os.environ.get("LOGNAME", current_home.name),
        "LANG": os.environ.get("LANG", "C.UTF-8"),
        "TMPDIR": "/tmp/hyprlodr/temp", "TMP": "/tmp/hyprlodr/temp",
        "TEMP": "/tmp/hyprlodr/temp", "XDG_CONFIG_HOME": "/tmp/hyprlodr/xdg/config",
        "XDG_CACHE_HOME": "/tmp/hyprlodr/xdg/cache", "XDG_DATA_HOME": "/tmp/hyprlodr/xdg/data",
        "XDG_STATE_HOME": "/tmp/hyprlodr/xdg/state", "XDG_RUNTIME_DIR": "/tmp/hyprlodr/runtime",
        "STEAM_COMPAT_DATA_PATH": "/tmp/hyprlodr/compatdata",
        "WINEPREFIX": "/tmp/hyprlodr/compatdata/pfx",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": "/tmp/hyprlodr/steam-client",
        "STEAM_COMPAT_APP_ID": meta["app_id"],
        "STEAM_COMPAT_INSTALL_PATH": "/tmp/hyprlodr/game",
        "STEAM_COMPAT_TOOL_PATHS": "/tmp/hyprlodr/proton",
        "PROTON_LOG": "1",
        # The control has no in-process file hook. Request Wine's file channel
        # as supplemental evidence; missing channel output stays unknown.
        "WINEDEBUG": "+file" if meta.get("mode") == "vanilla" else "-all"}
    if meta.get("mode") == "diagnostic":
        env.update({"HYPRLODR_EARLY_RECORD_HOOK": "1",
                    "HYPRLODR_FIRST_LOAD_DIAGNOSTICS": "1"})
    args.insert(args.index("--"), "--clearenv")
    for key, value in env.items():
        args[args.index("--") : args.index("--")] = ["--setenv", key, value]
    preflight_path = run_root / "evidence/preflight.json"
    preflight_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    before = inventory(run_root)
    (run_root / "evidence/prelaunch-inventory.json").write_text(
        json.dumps(before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (run_root / "evidence/launch-environment.json").write_text(json.dumps({
        "cwd": str(run_root / "game"), "command": ["/tmp/hyprlodr/proton/proton", "run",
        "/tmp/hyprlodr/game/hyperdemon.exe"], "environment": env,
        "sandbox": report["bubblewrap"]}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"event": "isolated_launch_preflight_passed", "preflight": report}, indent=2), flush=True)
    log_path = run_root / "logs/sandbox-supervisor-host.log"
    with log_path.open("wb") as log:
            process = subprocess.Popen(args, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1, close_fds=True,
                start_new_session=True, env={
                    "PATH": "/usr/bin:/bin", "HOME": str(run_root / "home"),
                    "TMPDIR": str(run_root / "temp"), "TMP": str(run_root / "temp"),
                    "TEMP": str(run_root / "temp"), "LANG": "C.UTF-8"})
            try:
                for line in process.stdout:
                    log.write(line.encode(errors="replace"))
                    log.flush()
                    print(line, end="", flush=True)
                code = process.wait()
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGTERM)
                try: code = process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    code = process.wait(timeout=5)
    after = inventory(run_root)
    changed = {name: {"before": before.get(name), "after": after.get(name)}
               for name in sorted(set(before) | set(after)) if before.get(name) != after.get(name)}
    (run_root / "evidence/postlaunch-inventory.json").write_text(
        json.dumps({"schema": 1, "created_or_modified": changed,
                    "deleted": sorted(set(before) - set(after)),
                    "host_game_source_unchanged":
                        sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE and
                        sha256(source_game / "asset/asset") == meta["source_archive_sha256"],
                    "host_game_openal_files_unchanged":
                        sha256(source_game / "OpenAL32.dll") == meta["source_installed_openal_sha256"] and
                        sha256(Path(meta["source_openal_runtime_path"])) ==
                        meta["source_openal_runtime_sha256"] == EXPECTED_OPENAL,
                    "source_proton_launcher_unchanged":
                        sha256(Path(meta["source_proton_root"]) / "proton") ==
                        meta["source_proton_launcher_sha256"]},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    attach_files = list((run_root / "game").glob("hyprlodr-attach-*.json"))
    if attach_files:
        attach = attach_files[-1]
        startup = run_root / "game" / f"hyprlodr-startup-{attach.stem.removeprefix('hyprlodr-attach-')}.jsonl"
        files = run_root / "game" / f"hyprlodr-files-{attach.stem.removeprefix('hyprlodr-attach-')}.jsonl"
        if startup.is_file() and files.is_file():
            try:
                from .first_load_diagnostics import build_coverage
            except ImportError:
                from first_load_diagnostics import build_coverage
            coverage = build_coverage(startup, files, attach, process_exited=True)
            (run_root / "evidence/coverage-report.json").write_text(
                json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"event": "isolated_launch_finished", "returncode": code,
        "run_root": str(run_root), "inventory": str(run_root / "evidence/postlaunch-inventory.json")}, indent=2))
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    staging = sub.add_parser("stage")
    staging.add_argument("--source-game", type=Path, required=True)
    staging.add_argument("--proton", type=Path, required=True)
    staging.add_argument("--proxy", type=Path)
    staging.add_argument("--original-openal", type=Path, required=True)
    staging.add_argument("--mode", choices=("diagnostic", "vanilla"), default="diagnostic")
    staging.add_argument("--run-id", required=True)
    staging.add_argument("--app-id", default=APP_ID)
    staging.add_argument("--input-probe", type=Path)
    snapshot_parser = sub.add_parser("snapshot-auth-data")
    snapshot_parser.add_argument("--game-root", type=Path, required=True,
                                 help="installed game directory whose install state must be preserved")
    snapshot_parser.add_argument("--user-data-root", type=Path, required=True,
                                 help="existing game-specific Proton AppData directory")
    snapshot_parser.add_argument("--snapshot-id", required=True)
    auth_preflight = sub.add_parser("authenticated-preflight")
    auth_preflight.add_argument("--source-game", type=Path, required=True)
    auth_preflight.add_argument("--game-copy", type=Path, required=True)
    auth_preflight.add_argument("--proton", type=Path, required=True)
    auth_preflight.add_argument("--steam-root", type=Path, required=True)
    auth_preflight.add_argument("--compatdata", type=Path, required=True)
    auth_preflight.add_argument("--original-openal", type=Path, required=True)
    auth_preflight.add_argument("--user-data-root", type=Path, required=True,
                                help="original installed-prefix game-specific AppData directory")
    auth_preflight.add_argument("--snapshot-manifest", type=Path)
    preflight = sub.add_parser("preflight")
    preflight.add_argument("--run", type=Path, required=True)
    preflight.add_argument("--source-game", type=Path, required=True)
    preflight.add_argument("--bwrap", type=Path)
    launch_parser = sub.add_parser("launch")
    launch_parser.add_argument("--run", type=Path, required=True)
    launch_parser.add_argument("--source-game", type=Path, required=True)
    launch_parser.add_argument("--bwrap", type=Path)
    launch_parser.add_argument("--seconds", type=int, default=150)
    args = parser.parse_args(argv)
    try:
        if args.command == "stage":
            run_root = SANDBOX / args.run_id
            result = stage(args.source_game, args.proton, args.proxy,
                           args.original_openal, run_root, args.app_id, args.input_probe,
                           args.mode)
            print(json.dumps(result, indent=2))
        elif args.command == "snapshot-auth-data":
            result = snapshot_game_user_data(args.game_root, args.user_data_root, args.snapshot_id)
            print(json.dumps(result, indent=2))
        elif args.command == "authenticated-preflight":
            result = authenticated_preflight(
                args.source_game, args.game_copy, args.proton, args.steam_root,
                args.compatdata, args.original_openal, args.user_data_root,
                args.snapshot_manifest)
            print(json.dumps(result, indent=2))
            return 0 if result["safe_to_launch"] else 3
        elif args.command == "preflight":
            result = check_runtime(args.run, args.source_game, args.bwrap)
            print(json.dumps(result, indent=2))
        else:
            return launch(args.run, args.source_game, args.seconds, args.bwrap)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"isolated Proton {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
