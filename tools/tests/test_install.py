#!/usr/bin/env python3
"""Installation safety tests for tools/hyprl_install.py.

The installers must be non-destructive: a mod file the user already had is kept
and restored, a failed installation leaves the game directory exactly as it was,
and uninstall never deletes a file that changed since installation.

Runs against throwaway directories, never a real game installation.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "hyprl_install.py"
ORIGINAL_BYTES = b"original library bytes"
PROXY_BYTES = b"hyprlodr proxy bytes"


def make_tree(root: Path) -> tuple[Path, Path]:
    game = root / "hyperdemon"
    game.mkdir()
    (game / "hyperdemon.exe").write_bytes(b"MZ fake exe")
    (game / "OpenAL32.dll").write_bytes(ORIGINAL_BYTES)
    build = root / "build"
    (build / "mods").mkdir(parents=True)
    (build / "OpenAL32.dll").write_bytes(PROXY_BYTES)
    for name in ("a.palette", "b.audio"):
        (build / "mods" / f"{name}.dll").write_bytes(f"mod {name}".encode())
    return game, build


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def preserves_existing_mod() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-install-") as tmp:
        game, build = make_tree(Path(tmp))
        mods = game / "mods"
        mods.mkdir()
        (mods / "a.palette.dll").write_bytes(b"user mod bytes")

        installed = run("install", "--game-dir", game, "--build", build, "--yes")
        check(installed.returncode == 0, installed.stderr or installed.stdout)
        check((mods / "a.palette.dll").read_bytes() == b"mod a.palette",
              "install did not place the build's mod")
        manifest = json.loads((game / "hyprlodr-install-manifest.json").read_text())
        check("mods/a.palette.dll" in manifest.get("backups", {}),
              "the pre-existing mod was not recorded as a backup")

        removed = run("uninstall", "--game-dir", game, "--yes")
        check(removed.returncode == 0, removed.stderr or removed.stdout)
        check((mods / "a.palette.dll").read_bytes() == b"user mod bytes",
              "uninstall did not restore the mod the user already had")
        check(not (mods / "b.audio.dll").exists(), "installed mod left behind")
        check((game / "OpenAL32.dll").read_bytes() == ORIGINAL_BYTES,
              "original library was not restored")
    print("PASS install_preserves_existing_mod")


def failed_install_rolls_back() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-install-") as tmp:
        game, build = make_tree(Path(tmp))
        # A file where the mods directory belongs makes the copy step fail after
        # the proxy has already been replaced.
        (game / "mods").write_bytes(b"not a directory")

        result = run("install", "--game-dir", game, "--build", build, "--yes")
        check(result.returncode != 0, "install should fail when mods is not a directory")
        check("rolled back" in (result.stderr + result.stdout),
              result.stderr + result.stdout)
        check((game / "OpenAL32.dll").read_bytes() == ORIGINAL_BYTES,
              "the proxy was left in place after a failed install")
        check(not (game / "OpenAL32_real.dll").exists(),
              "a backup library was left behind after a failed install")
        check(not (game / "hyprlodr-install-manifest.json").exists(),
              "a manifest was left behind after a failed install")
        check((game / "mods").read_bytes() == b"not a directory",
              "the file that blocked the install was damaged")
    print("PASS failed_install_rolls_back")


def uninstall_keeps_changed_mod() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-install-") as tmp:
        game, build = make_tree(Path(tmp))
        check(run("install", "--game-dir", game, "--build", build, "--yes").returncode == 0,
              "install failed")
        mod = game / "mods" / "b.audio.dll"
        mod.write_bytes(b"tampered by the user")

        result = run("uninstall", "--game-dir", game, "--yes")
        check(result.returncode == 0, result.stderr or result.stdout)
        check(mod.exists() and mod.read_bytes() == b"tampered by the user",
              "uninstall deleted a mod that changed since installation")
        check(not (game / "mods" / "a.palette.dll").exists(),
              "an untouched mod was not removed")
        check((game / "OpenAL32.dll").read_bytes() == ORIGINAL_BYTES,
              "original library was not restored")
    print("PASS uninstall_keeps_changed_mod")


def main() -> int:
    preserves_existing_mod()
    failed_install_rolls_back()
    uninstall_keeps_changed_mod()
    print("3/3 installer scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
