#!/usr/bin/env python3
"""Installer tests for the title glyph mask.

The replacement needs one file next to the game - the mask the proxy uploads into
the game's own title texture. It must arrive by installing, not by hand-copying,
and a mask the user already had must survive the round trip.

Runs against throwaway directories, never a real game installation.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "hyprl_install.py"
MASK = "hyprlodr-title-mask.raw"
ORIGINAL_BYTES = b"original library bytes"
PROXY_BYTES = b"hyprlodr proxy bytes"
MASK_BYTES = b"glyph mask: 853x480 rgba, alpha holds the letterforms"
USER_MASK = b"a mask the user rendered earlier"


def make_tree(root: Path) -> tuple[Path, Path]:
    game = root / "hyperdemon"
    game.mkdir()
    (game / "hyperdemon.exe").write_bytes(b"MZ fake exe")
    (game / "OpenAL32.dll").write_bytes(ORIGINAL_BYTES)
    build = root / "build"
    (build / "mods").mkdir(parents=True)
    (build / "OpenAL32.dll").write_bytes(PROXY_BYTES)
    (build / "mods" / "a.palette.dll").write_bytes(b"mod a.palette")
    return game, build


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def mask_is_delivered() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-mask-") as tmp:
        game, build = make_tree(Path(tmp))
        mask = Path(tmp) / "mask.raw"
        mask.write_bytes(MASK_BYTES)

        installed = run("install", "--game-dir", game, "--build", build,
                        "--title-mask", mask, "--yes")
        check(installed.returncode == 0, installed.stderr or installed.stdout)
        target = game / MASK
        check(target.is_file() and target.read_bytes() == MASK_BYTES,
              "install did not place the title mask")
        manifest = json.loads((game / "hyprlodr-install-manifest.json").read_text())
        check(MASK in manifest.get("files", {}), "the mask was not recorded in the manifest")

        removed = run("uninstall", "--game-dir", game, "--yes")
        check(removed.returncode == 0, removed.stderr or removed.stdout)
        check(not target.exists(), "uninstall left the title mask behind")
        check((game / "OpenAL32.dll").read_bytes() == ORIGINAL_BYTES,
              "original library was not restored")
    print("PASS mask_is_delivered")


def existing_mask_is_restored() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-mask-") as tmp:
        game, build = make_tree(Path(tmp))
        mask = Path(tmp) / "mask.raw"
        mask.write_bytes(MASK_BYTES)
        target = game / MASK
        target.write_bytes(USER_MASK)

        installed = run("install", "--game-dir", game, "--build", build,
                        "--title-mask", mask, "--yes")
        check(installed.returncode == 0, installed.stderr or installed.stdout)
        check(target.read_bytes() == MASK_BYTES, "install did not replace the mask")
        manifest = json.loads((game / "hyprlodr-install-manifest.json").read_text())
        check(MASK in manifest.get("backups", {}),
              "the user's own mask was overwritten without a backup")

        removed = run("uninstall", "--game-dir", game, "--yes")
        check(removed.returncode == 0, removed.stderr or removed.stdout)
        check(target.read_bytes() == USER_MASK,
              "uninstall did not restore the mask the user already had")
    print("PASS existing_mask_is_restored")


def missing_mask_is_reported() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-mask-") as tmp:
        game, build = make_tree(Path(tmp))
        result = run("install", "--game-dir", game, "--build", build,
                     "--title-mask", Path(tmp) / "absent.raw", "--yes")
        check(result.returncode != 0, "install accepted a mask path that does not exist")
        check((game / "OpenAL32.dll").read_bytes() == ORIGINAL_BYTES,
              "the proxy was replaced before the missing mask was noticed")
        check(not (game / "OpenAL32_real.dll").exists(),
              "a backup library was left behind")
    print("PASS missing_mask_is_reported")


def main() -> int:
    mask_is_delivered()
    existing_mask_is_restored()
    missing_mask_is_reported()
    print("3/3 mask installer scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
