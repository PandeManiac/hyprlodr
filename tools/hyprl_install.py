#!/usr/bin/env python3
"""HYPRLODR loader installation with verification and rollback.

The shipped OpenAL32.dll is never destroyed: it is copied to OpenAL32_real.dll
(the forwarder target) before the proxy takes its name. Every file the installer
touches is recorded with a SHA-256 hash in hyprlodr-install-manifest.json, so
uninstall can refuse to "restore" a file that changed underneath it.

Usage:
    hyprl_install.py status    [--game-dir DIR]
    hyprl_install.py install   [--game-dir DIR] [--build DIR] [--yes] [--dry-run]
    hyprl_install.py uninstall [--game-dir DIR] [--yes] [--dry-run]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hyprl_diagnose import find_installations, sha256  # noqa: E402

MANIFEST = "hyprlodr-install-manifest.json"
PROXY = "OpenAL32.dll"
ORIGINAL = "OpenAL32_real.dll"
MOD_DIR = "mods"


def resolve_game(explicit: str | None) -> Path:
    installs = find_installations(explicit)
    if not installs:
        raise SystemExit("error: HYPER DEMON not found. Pass --game-dir with the folder "
                         "containing hyperdemon.exe.")
    return Path(installs[0]["path"])


def manifest_path(game: Path) -> Path:
    return game / MANIFEST


def load_manifest(game: Path) -> dict | None:
    path = manifest_path(game)
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def confirm(prompt: str, assume_yes: bool) -> bool:
    if assume_yes:
        return True
    if not sys.stdin.isatty():
        print(f"{prompt}\nRefusing without --yes in a non-interactive session.")
        return False
    return input(f"{prompt} [y/N] ").strip().lower() in ("y", "yes")


def cmd_status(args) -> int:
    game = resolve_game(args.game_dir)
    print(f"game directory: {game}")
    manifest = load_manifest(game)
    proxy = game / PROXY
    original = game / ORIGINAL
    print(f"proxy present     : {proxy.is_file()}")
    print(f"original preserved: {original.is_file()}")
    print(f"mods directory    : {(game / MOD_DIR).is_dir()}")
    if manifest:
        print(f"manifest          : installed {manifest.get('installed_at')} "
              f"(loader {manifest.get('loader_version')})")
        if proxy.is_file():
            current = sha256(proxy)
            expected = manifest.get("files", {}).get(PROXY, {}).get("installed_sha256")
            print(f"proxy hash        : {'matches manifest' if current == expected else 'CHANGED since install'}")
        if original.is_file():
            current = sha256(original)
            expected = manifest.get("files", {}).get(ORIGINAL, {}).get("installed_sha256")
            print(f"original hash     : {'matches manifest' if current == expected else 'CHANGED since install'}")
    else:
        print("manifest          : none (HYPRLODR not installed, or installed by hand)")
    return 0


def cmd_install(args) -> int:
    game = resolve_game(args.game_dir)
    build = Path(args.build).resolve()
    proxy_source = build / PROXY
    mods_source = build / MOD_DIR
    if not proxy_source.is_file():
        raise SystemExit(f"error: {proxy_source} not found. Build it with tools/build_windows.sh")
    if (game / ORIGINAL).is_file():
        raise SystemExit(
            f"error: {ORIGINAL} already exists in {game}. HYPRLODR appears to be installed "
            "already, or a previous installation was interrupted. Run "
            "'hyprl_install.py uninstall' first, or inspect the files manually."
        )
    if not (game / "hyperdemon.exe").is_file():
        raise SystemExit(f"error: {game} does not contain hyperdemon.exe")

    print(f"game        : {game}")
    print(f"proxy       : {proxy_source} ({proxy_source.stat().st_size} bytes)")
    print(f"mods        : {sorted(p.name for p in mods_source.glob('*.dll')) if mods_source.is_dir() else []}")
    print(f"will rename : {PROXY} -> {ORIGINAL} (copy, the original bytes are preserved)")
    if args.dry_run:
        print("dry run: nothing changed")
        return 0
    if not confirm("Install HYPRLODR into this game directory?", args.yes):
        return 1

    files: dict[str, dict] = {}
    original_hash = sha256(game / PROXY)
    shutil.copy2(game / PROXY, game / ORIGINAL)
    if sha256(game / ORIGINAL) != original_hash:
        (game / ORIGINAL).unlink(missing_ok=True)
        raise SystemExit("error: backup verification failed; nothing was replaced")
    files[ORIGINAL] = {"sha256": original_hash, "installed_sha256": original_hash,
                       "role": "original library, preserved verbatim"}

    shutil.copy2(proxy_source, game / PROXY)
    proxy_hash = sha256(game / PROXY)
    files[PROXY] = {"sha256": proxy_hash, "installed_sha256": proxy_hash,
                    "role": "HYPRLODR proxy DLL"}

    installed_mods = []
    if mods_source.is_dir():
        target = game / MOD_DIR
        target.mkdir(exist_ok=True)
        for mod in sorted(mods_source.glob("*.dll")):
            shutil.copy2(mod, target / mod.name)
            installed_mods.append(mod.name)
            files[f"{MOD_DIR}/{mod.name}"] = {"sha256": sha256(target / mod.name)}

    manifest = {
        "loader_version": args.loader_version,
        "installed_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "game_dir": str(game),
        "files": files,
        "mods": installed_mods,
    }
    manifest_path(game).write_text(json.dumps(manifest, indent=2))
    print(f"installed: {len(installed_mods)} mod(s); manifest at {manifest_path(game)}")
    print("launch the game normally through Steam. Evidence appears in "
          f"{game / 'hyprlodr-attach.json'} and {game / 'hyprlodr-events.jsonl'}.")
    return 0


def cmd_uninstall(args) -> int:
    game = resolve_game(args.game_dir)
    manifest = load_manifest(game)
    print(f"game: {game}")
    if manifest is None and not (game / ORIGINAL).is_file():
        raise SystemExit("error: no HYPRLODR installation detected")
    if args.dry_run:
        print("dry run: nothing changed")
        return 0
    if not confirm("Remove HYPRLODR and restore the original library?", args.yes):
        return 1

    expected = (manifest or {}).get("files", {}).get(PROXY, {}).get("installed_sha256")
    if expected and (game / PROXY).is_file() and sha256(game / PROXY) != expected:
        raise SystemExit(
            f"error: {PROXY} changed since installation; refusing to remove it. "
            "Inspect the file, or restore it from Steam (verify integrity of game files)."
        )

    if (game / ORIGINAL).is_file():
        (game / PROXY).unlink(missing_ok=True)
        (game / ORIGINAL).rename(game / PROXY)
        print(f"restored original {PROXY}")
    if manifest:
        for name in manifest.get("mods", []):
            (game / MOD_DIR / Path(name).name).unlink(missing_ok=True)
    mods_dir = game / MOD_DIR
    if mods_dir.is_dir() and not any(mods_dir.iterdir()):
        mods_dir.rmdir()
    for name in ("hyprlodr-attach.json", "hyprlodr-events.jsonl"):
        (game / name).unlink(missing_ok=True)
    manifest_path(game).unlink(missing_ok=True)
    print("uninstalled; run tools/hyprl_diagnose.py to confirm the original state")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Install or remove HYPRLODR from HYPER DEMON")
    parser.add_argument("--game-dir", help="folder containing hyperdemon.exe")
    parser.add_argument("--build", default="build-win", help="directory built by tools/build_windows.sh")
    parser.add_argument("--loader-version", default="0.2.0")
    parser.add_argument("--yes", action="store_true", help="do not prompt")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("action", choices=["status", "install", "uninstall"])
    args = parser.parse_args(argv)
    if args.action == "status":
        return cmd_status(args)
    if args.action == "install":
        return cmd_install(args)
    return cmd_uninstall(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
