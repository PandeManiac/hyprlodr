#!/usr/bin/env python3
"""Package format test suite: packing, defensive validation, install, cross-check.

Usage: test_package.py <hyprlodr-host> <sample-mod-library>
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import hyprl_package as pkg  # noqa: E402

HOST = Path(sys.argv[1])
SAMPLE = Path(sys.argv[2])
RESULTS: list[tuple[str, callable]] = []


def scenario(name):
    def register(fn):
        RESULTS.append((name, fn))
        return fn
    return register


def base_manifest(**overrides) -> dict:
    manifest = {
        "schema": 1,
        "id": "demo.package",
        "name": "Demo package",
        "version": "1.0.0",
        "author": "tests",
        "description": "fixture",
        "api": 2,
        "platform": "linux-x86_64",
        "entry": "native/linux-x86_64/demo.package.so",
        "dependencies": [],
        "conflicts": [],
        "capabilities": [],
    }
    manifest.update(overrides)
    return manifest


def make_mod_dir(root: Path, manifest: dict | None = None) -> Path:
    manifest = manifest or base_manifest()
    mod = root / "src"
    entry = mod / manifest["entry"]
    entry.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(SAMPLE, entry)
    (mod / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return mod


def write_zip(path: Path, entries: dict[str, bytes], *, duplicate: bool = False,
              symlink: str | None = None) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            info = zipfile.ZipInfo(name)
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)
            if duplicate:
                info2 = zipfile.ZipInfo(name)
                info2.external_attr = 0o644 << 16
                archive.writestr(info2, data)
        if symlink:
            info = zipfile.ZipInfo(symlink)
            info.external_attr = (0o120777 << 16)
            archive.writestr(info, b"/etc/passwd")
    return path


def expect_reject(package: Path, fragment: str) -> None:
    try:
        pkg.inspect_package(package)
    except pkg.PackageError as exc:
        assert fragment in str(exc), f"expected {fragment!r} in error, got {exc}"
        return
    raise AssertionError(f"package was accepted but should have been rejected ({fragment})")


@scenario("pack_and_validate_roundtrip")
def pack_roundtrip():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        mod = make_mod_dir(root)
        archive = pkg.pack(mod, root / "out.zip")
        manifest, entries = pkg.inspect_package(root / "out.zip")
        assert manifest["id"] == "demo.package", manifest
        assert "manifest.json" in entries and any(e.startswith("native/") for e in entries)


@scenario("rejects_traversal")
def rejects_traversal():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
            "../evil.so": b"x",
        })
        expect_reject(path, "unsafe path")


@scenario("rejects_absolute_path")
def rejects_absolute():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
            "/etc/evil.so": b"x",
        })
        expect_reject(path, "absolute paths")


@scenario("rejects_backslash_path")
def rejects_backslash():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
            "native\\evil.so": b"x",
        })
        expect_reject(path, "backslash")


@scenario("rejects_unknown_root_file")
def rejects_unknown_root():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
            "installer.exe": b"MZ",
        })
        expect_reject(path, "not allowed")


@scenario("rejects_symlink_entry")
def rejects_symlink():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
        }, symlink="assets/link")
        expect_reject(path, "regular files")


@scenario("rejects_duplicate_entry")
def rejects_duplicate():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "evil.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
        }, duplicate=True)
        expect_reject(path, "duplicate entry")


@scenario("rejects_missing_entry_file")
def rejects_missing_entry():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "bad.zip", {
            "manifest.json": json.dumps(base_manifest()).encode(),
        })
        expect_reject(path, "not present in the archive")


@scenario("rejects_invalid_id")
def rejects_invalid_id():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "bad.zip", {
            "manifest.json": json.dumps(base_manifest(id="Bad Id!")).encode(),
            "native/linux-x86_64/demo.package.so": b"x",
        })
        expect_reject(path, "invalid mod id")


@scenario("rejects_invalid_dependency")
def rejects_invalid_dependency():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "bad.zip", {
            "manifest.json": json.dumps(base_manifest(dependencies=["z.foundation@!!1"])).encode(),
            "native/linux-x86_64/demo.package.so": b"x",
        })
        expect_reject(path, "invalid dependencies entry")


@scenario("rejects_bad_api")
def rejects_bad_api():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "bad.zip", {
            "manifest.json": json.dumps(base_manifest(api=0)).encode(),
            "native/linux-x86_64/demo.package.so": b"x",
        })
        expect_reject(path, "invalid api")


@scenario("rejects_wrong_platform_suffix")
def rejects_wrong_suffix():
    with tempfile.TemporaryDirectory() as tmp:
        path = write_zip(Path(tmp) / "bad.zip", {
            "manifest.json": json.dumps(base_manifest(entry="native/linux-x86_64/x.dll")).encode(),
            "native/linux-x86_64/x.dll": b"x",
        })
        expect_reject(path, "does not match platform")


@scenario("rejects_zip_bomb_ratio")
def rejects_bomb_ratio():
    original = pkg.MAX_COMPRESSION_RATIO
    pkg.MAX_COMPRESSION_RATIO = 4
    try:
        with tempfile.TemporaryDirectory() as tmp:
            archive_path = Path(tmp) / "bomb.zip"
            with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("manifest.json", json.dumps(base_manifest()))
                info = zipfile.ZipInfo("native/linux-x86_64/demo.package.so")
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, b"\0" * 500_000)
            expect_reject(archive_path, "compression ratio")
    finally:
        pkg.MAX_COMPRESSION_RATIO = original


@scenario("rejects_oversized_entry")
def rejects_oversized():
    original = pkg.MAX_ENTRY_BYTES
    pkg.MAX_ENTRY_BYTES = 512
    try:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_zip(Path(tmp) / "big.zip", {
                "manifest.json": json.dumps(base_manifest()).encode(),
                "native/linux-x86_64/demo.package.so": b"x" * 4096,
            })
            expect_reject(path, "too large")
    finally:
        pkg.MAX_ENTRY_BYTES = original


@scenario("rejects_non_zip")
def rejects_non_zip():
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "not.zip"
        path.write_bytes(b"definitely not a zip")
        expect_reject(path, "not a ZIP archive")


@scenario("install_list_remove")
def install_list_remove():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # The sample binary's real descriptor is z.foundation, so the manifest
        # must say so for the descriptor cross-check to succeed.
        manifest = base_manifest(id="z.foundation",
                                 entry="native/linux-x86_64/z.foundation.so")
        mod = make_mod_dir(root, manifest)
        archive = pkg.pack(mod, root / "pkg.zip")
        mods_dir = root / "mods"
        record = pkg.install(archive, mods_dir, host=HOST)
        assert record["descriptor_verified"] is True, record
        assert (mods_dir / "z.foundation.so").is_file()
        index = pkg.load_index(mods_dir)
        assert "z.foundation" in index["mods"]
        # installing identical content again is idempotent
        pkg.install(archive, mods_dir, host=HOST)
        removed = pkg.remove(mods_dir, "z.foundation")
        assert removed == "z.foundation.so"
        assert not (mods_dir / "z.foundation.so").exists()


@scenario("install_refuses_different_content")
def install_refuses_conflict():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        mods_dir = root / "mods"
        first = pkg.pack(make_mod_dir(root / "a"), root / "a.zip")
        pkg.install(first, mods_dir)
        # same id, different binary content (packing does not validate ABI, so an
        # arbitrary payload is enough to change the content hash)
        mod_b = make_mod_dir(root / "b", base_manifest(version="1.0.1"))
        (mod_b / "native/linux-x86_64/demo.package.so").write_bytes(b"different payload")
        second = pkg.pack(mod_b, root / "b.zip")
        try:
            pkg.install(second, mods_dir)
        except pkg.PackageError as exc:
            assert "already installed" in str(exc), exc
            return
        raise AssertionError("conflicting install was accepted")


@scenario("descriptor_cross_check_detects_mismatch")
def cross_check_mismatch():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        mod = make_mod_dir(root, base_manifest(version="9.9.9"))
        archive = pkg.pack(mod, root / "pkg.zip")
        try:
            pkg.install(archive, root / "mods", host=HOST)
        except pkg.PackageError as exc:
            assert "does not match descriptor" in str(exc), exc
            return
        raise AssertionError("version mismatch between manifest and descriptor was accepted")


@scenario("descriptor_cross_check_accepts_real_mod")
def cross_check_ok():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        manifest = base_manifest(id="z.foundation", version="1.0.0",
                                 entry="native/linux-x86_64/z.foundation.so")
        mod = make_mod_dir(root, manifest)
        archive = pkg.pack(mod, root / "pkg.zip")
        record = pkg.install(archive, root / "mods", host=HOST)
        assert record["id"] == "z.foundation"
        assert record["descriptor_verified"] is True


def main() -> int:
    failures = 0
    for name, fn in RESULTS:
        try:
            fn()
            print(f"PASS {name}")
        except Exception as exc:  # noqa: BLE001 - report any failure
            failures += 1
            print(f"FAIL {name}: {type(exc).__name__}: {exc}")
    print(f"\n{len(RESULTS) - failures}/{len(RESULTS)} package scenarios passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
