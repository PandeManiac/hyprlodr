#!/usr/bin/env python3
"""Tests for package payload files - the part that lets a package ship the title mask.

A mod package normally installs one library into `mods/`. The title replacement needs a file
beside the game instead (the mask the proxy uploads), so `files` in the manifest copies payload
into the game directory. These tests keep that path honest: it must land, it must be recorded,
it must be removed on uninstall, and it must never be able to write outside the game directory.

Everything runs in throwaway directories.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / "hyprl_package.py"
MOD_ID = "example.title-replacement"
MASK = bytes(range(256)) * 64


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def make_package(root: Path, files: list[dict] | None, payload: bytes = MASK) -> Path:
    source = root / "src"
    (source / "native" / "windows-x86_64").mkdir(parents=True)
    (source / "assets").mkdir(parents=True)
    (source / "native" / "windows-x86_64" / f"{MOD_ID}.dll").write_bytes(b"MZ fake mod")
    (source / "assets" / "title-mask.raw").write_bytes(payload)
    manifest = {
        "schema": 1, "id": MOD_ID, "name": "Title replacement", "version": "1.0.0",
        "author": "HYPRLODR", "api": 7, "platform": "windows-x86_64",
        "entry": f"native/windows-x86_64/{MOD_ID}.dll",
    }
    if files is not None:
        manifest["files"] = files
    (source / "manifest.json").write_text(json.dumps(manifest))
    archive = root / "pkg.zip"
    packed = run("pack", source, archive)
    check(packed.returncode == 0, packed.stderr or packed.stdout)
    return archive


def payload_installs_beside_the_game() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-pkg-") as tmp:
        root = Path(tmp)
        archive = make_package(root, [{"from": "assets/title-mask.raw",
                                       "to": "hyprlodr-title-mask.raw"}])
        game = root / "game"
        mods = game / "mods"
        installed = run("install", archive, "--mods-dir", mods)
        check(installed.returncode == 0, installed.stderr or installed.stdout)

        target = game / "hyprlodr-title-mask.raw"
        check(target.is_file() and target.read_bytes() == MASK,
              "the mask was not installed beside the game")
        check((mods / f"{MOD_ID}.dll").is_file(), "the mod library was not installed")
        index = json.loads((mods / "hyprlodr-mods.json").read_text())
        check("hyprlodr-title-mask.raw" in index["mods"][MOD_ID]["files"],
              "the installed mask was not recorded in the index")

        removed = run("remove", MOD_ID, "--mods-dir", mods)
        check(removed.returncode == 0, removed.stderr or removed.stdout)
        check(not target.exists(), "removing the mod left the mask behind")
        check(not (mods / f"{MOD_ID}.dll").exists(), "the mod library was left behind")
    print("PASS payload_installs_beside_the_game")


def reinstall_restores_a_deleted_payload() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-pkg-") as tmp:
        root = Path(tmp)
        archive = make_package(root, [{"from": "assets/title-mask.raw",
                                       "to": "hyprlodr-title-mask.raw"}])
        game = root / "game"
        mods = game / "mods"
        check(run("install", archive, "--mods-dir", mods).returncode == 0, "install failed")
        (game / "hyprlodr-title-mask.raw").unlink()

        again = run("install", archive, "--mods-dir", mods)
        check(again.returncode == 0, again.stderr or again.stdout)
        check((game / "hyprlodr-title-mask.raw").is_file(),
              "re-installing did not put the deleted mask back")
    print("PASS reinstall_restores_a_deleted_payload")


def changed_payload_is_kept() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-pkg-") as tmp:
        root = Path(tmp)
        archive = make_package(root, [{"from": "assets/title-mask.raw",
                                       "to": "hyprlodr-title-mask.raw"}])
        game = root / "game"
        mods = game / "mods"
        check(run("install", archive, "--mods-dir", mods).returncode == 0, "install failed")
        target = game / "hyprlodr-title-mask.raw"
        target.write_bytes(b"the user's own mask")

        removed = run("remove", MOD_ID, "--mods-dir", mods)
        check(removed.returncode == 0, removed.stderr or removed.stdout)
        check(target.exists() and target.read_bytes() == b"the user's own mask",
              "removing the mod deleted a mask the user had changed")
        check("kept hyprlodr-title-mask.raw" in (removed.stdout + removed.stderr),
              f"the removal did not say what it kept: {removed.stdout}{removed.stderr}")
    print("PASS changed_payload_is_kept")


def escaping_target_is_refused() -> None:
    for target in ("../evil.bin", "/etc/evil.bin", "mods/evil.dll", "hyprlodr-mods.json"):
        with tempfile.TemporaryDirectory(prefix="hyprlodr-pkg-") as tmp:
            root = Path(tmp)
            source = root / "src"
            (source / "native" / "windows-x86_64").mkdir(parents=True)
            (source / "assets").mkdir(parents=True)
            (source / "native" / "windows-x86_64" / f"{MOD_ID}.dll").write_bytes(b"MZ")
            (source / "assets" / "title-mask.raw").write_bytes(MASK)
            (source / "manifest.json").write_text(json.dumps({
                "schema": 1, "id": MOD_ID, "name": "x", "version": "1.0.0", "author": "y",
                "api": 7, "platform": "windows-x86_64",
                "entry": f"native/windows-x86_64/{MOD_ID}.dll",
                "files": [{"from": "assets/title-mask.raw", "to": target}],
            }))
            packed = run("pack", source, root / "pkg.zip")
            check(packed.returncode != 0, f"packing accepted the target {target!r}")
            check(not (root / "pkg.zip").exists() or packed.returncode != 0,
                  "a package with an escaping target was produced")
    print("PASS escaping_target_is_refused")


def source_outside_payload_prefix_is_refused() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-pkg-") as tmp:
        root = Path(tmp)
        source = root / "src"
        (source / "native" / "windows-x86_64").mkdir(parents=True)
        (source / "native" / "windows-x86_64" / f"{MOD_ID}.dll").write_bytes(b"MZ")
        (source / "manifest.json").write_text(json.dumps({
            "schema": 1, "id": MOD_ID, "name": "x", "version": "1.0.0", "author": "y",
            "api": 7, "platform": "windows-x86_64",
            "entry": f"native/windows-x86_64/{MOD_ID}.dll",
            "files": [{"from": f"native/windows-x86_64/{MOD_ID}.dll", "to": "should-not-appear"}],
        }))
        packed = run("pack", source, root / "pkg.zip")
        check(packed.returncode != 0, "a files entry sourced from native/ was accepted")
    print("PASS source_outside_payload_prefix_is_refused")


def main() -> int:
    payload_installs_beside_the_game()
    reinstall_restores_a_deleted_payload()
    changed_payload_is_kept()
    escaping_target_is_refused()
    source_outside_payload_prefix_is_refused()
    print("5/5 package payload scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
