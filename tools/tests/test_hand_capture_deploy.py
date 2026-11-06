import os
import pathlib
import json
import shutil
import tempfile
from types import SimpleNamespace
import unittest

from tools.hand_capture_deploy import (compare_untouched, deploy, inside,
    install_path_errors, restore_journal, sha256)

ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / ".tmp" / "tests" / "hand-capture-deploy"


class HandCaptureDeployTests(unittest.TestCase):
    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        self.case = pathlib.Path(tempfile.mkdtemp(prefix="deploy-test-", dir=TMP))
        self.original = self.case / "original.pack"
        self.modified = self.case / "modified.pack"
        self.original.write_bytes(bytes(range(256)) * 4)

    def tearDown(self):
        shutil.rmtree(self.case, ignore_errors=True)

    def test_archive_comparison_ignores_only_hand2_record(self):
        source = self.original.read_bytes()
        offset, size = 211, 31
        replacement = bytearray(source)
        replacement[offset:offset + size] = b"X" * size
        self.modified.write_bytes(replacement)
        self.assertTrue(compare_untouched(self.original, self.modified, offset, size))
        replacement[-1] ^= 1
        self.modified.write_bytes(replacement)
        self.assertFalse(compare_untouched(self.original, self.modified, offset, size))

    def test_archive_comparison_rejects_wrong_size(self):
        self.modified.write_bytes(self.original.read_bytes()[:-1])
        self.assertFalse(compare_untouched(self.original, self.modified))

    def test_generated_archive_must_live_under_persistent_project_tmp(self):
        self.assertTrue(inside(self.case / "asset.patched", ROOT / ".tmp"))
        self.assertFalse(inside(ROOT / "asset.patched", ROOT / ".tmp"))

    def test_install_targets_reject_symlinks_and_hardlinks(self):
        game = self.case / "game-links"
        (game / "mods").mkdir(parents=True, exist_ok=True)
        source = self.case / "external-user-file"
        source.write_bytes(b"user data")
        (game / "hyprlodr-config.json").symlink_to(source)
        errors = install_path_errors(game)
        self.assertTrue(any("hyprlodr-config.json" in error for error in errors), errors)
        (game / "hyprlodr-config.json").unlink()
        os.link(source, game / "mods" / "example.hand-hook.dll")
        errors = install_path_errors(game)
        self.assertTrue(any("example.hand-hook.dll" in error for error in errors), errors)
        (game / "mods" / "example.hand-hook.dll").unlink()
        source.unlink()

    def test_restore_rejects_user_changes_before_restoring_any_file(self):
        game = self.case / "game-conflict"
        journal_dir = self.case / "journal-conflict"
        game.mkdir(parents=True, exist_ok=True)
        target = game / "mods" / "example.hand-hook.dll"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"deployed")
        changed = game / "hyprlodr-config.json"
        changed.write_bytes(b"user changed this")
        backup_dir = journal_dir / "backup"
        (backup_dir / "mods").mkdir(parents=True, exist_ok=True)
        (backup_dir / "hyprlodr-config.json").write_bytes(b"original config")
        (backup_dir / "mods" / "example.hand-hook.dll").write_bytes(b"original mod")
        journal_dir.mkdir(parents=True, exist_ok=True)
        journal = {
            "schema": 1, "game_dir": str(game), "backup_dir": str(backup_dir),
            "runtime_files": [],
            "entries": [
                {"path": "mods/example.hand-hook.dll", "existed": True,
                 "before_sha256": sha256(backup_dir / "mods" / "example.hand-hook.dll"),
                 "deployed_sha256": sha256(target)},
                {"path": "hyprlodr-config.json", "existed": True,
                 "before_sha256": sha256(backup_dir / "hyprlodr-config.json"),
                 "deployed_sha256": "0" * 64},
            ],
        }
        path = journal_dir / "deployment.json"
        path.write_text(__import__("json").dumps(journal))
        self.assertEqual(restore_journal(path, keep_runtime=False), 2)
        self.assertEqual(target.read_bytes(), b"deployed")
        self.assertEqual(changed.read_bytes(), b"user changed this")

    def test_restore_recovers_backups_and_collects_runtime(self):
        game = self.case / "game-restore"
        journal_dir = self.case / "journal-restore"
        game.mkdir(parents=True, exist_ok=True)
        target = game / "mods" / "example.hand-hook.dll"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"deployed")
        log = game / "hyprlodr-events.jsonl"
        log.write_bytes(b"new runtime log")
        backup_dir = journal_dir / "backup"
        (backup_dir / "mods").mkdir(parents=True, exist_ok=True)
        (backup_dir / "hyprlodr-events.jsonl").parent.mkdir(parents=True, exist_ok=True)
        (backup_dir / "mods" / "example.hand-hook.dll").write_bytes(b"original mod")
        (backup_dir / "hyprlodr-events.jsonl").write_bytes(b"previous log")
        journal_dir.mkdir(parents=True, exist_ok=True)
        journal = {
            "schema": 1, "game_dir": str(game), "backup_dir": str(backup_dir),
            "runtime_files": ["hyprlodr-events.jsonl"],
            "entries": [
                {"path": "mods/example.hand-hook.dll", "existed": True,
                 "before_sha256": sha256(backup_dir / "mods" / "example.hand-hook.dll"),
                 "deployed_sha256": sha256(target)},
                {"path": "hyprlodr-events.jsonl", "existed": True,
                 "before_sha256": sha256(backup_dir / "hyprlodr-events.jsonl"),
                 "deployed_sha256": sha256(log)},
            ],
        }
        path = journal_dir / "deployment.json"
        path.write_text(__import__("json").dumps(journal))
        self.assertEqual(restore_journal(path), 0)
        self.assertEqual(target.read_bytes(), b"original mod")
        self.assertEqual(log.read_bytes(), b"previous log")
        collected = journal_dir / "runtime-collected" / "hyprlodr-events.jsonl"
        self.assertEqual(collected.read_bytes(), b"new runtime log")

    def test_append_route_deploy_stages_forwarder_and_restores_everything(self):
        game = self.case / "game-route-deploy"
        build = self.case / "build-route-deploy"
        journal = self.case / "journal-route-deploy"
        capture = self.case / "route-run" / "hand2.capture"
        archive = self.case / "route-run" / "asset-appended"
        manifest = self.case / "route-run" / "manifest.json"
        for path in (game / "mods", build / "examples", game / "asset",
                     game / "hyprlodr-state" / "example.hand-hook",
                     game / "hyprlodr-state" / "example.asset-redirect",
                     archive.parent):
            path.mkdir(parents=True, exist_ok=True)
        (game / "OpenAL32.dll").write_bytes(b"shipped-openal")
        (game / "OpenAL32_real.dll").write_bytes(b"prior-forwarder")
        (game / "mods" / "disabled.txt").write_text("")
        (game / "hyprlodr-config.json").write_text(json.dumps({
            "experimental_record_route": True,
            "experimental_record_route_manifest": "stale-manifest",
            "experimental_record_route_archive": "stale-archive"}))
        (build / "OpenAL32.dll").write_bytes(b"hyprlodr-proxy")
        (build / "examples" / "example.hand-hook.dll").write_bytes(b"capture-mod")
        (build / "examples" / "example.asset-redirect.dll").write_bytes(b"redirect-mod")
        archive.write_bytes(b"append-archive")
        manifest.write_text("{}")
        capture_output = str(capture.resolve())
        args = SimpleNamespace(game_dir=game, build=build, archive=archive,
            route_manifest=manifest, capture_output=capture, journal=journal)
        result = {"mode": "append-route", "archive": str(archive.resolve())}
        self.assertEqual(deploy(args, result), 0)
        self.assertEqual((game / "OpenAL32_real.dll").read_bytes(), b"shipped-openal")
        self.assertEqual((game / "OpenAL32.dll").read_bytes(), b"hyprlodr-proxy")
        config = json.loads((game / "hyprlodr-config.json").read_text())
        self.assertIs(config["experimental_record_route"], True)
        self.assertEqual(config["experimental_record_route_manifest"],
                         "Z:" + manifest.resolve().as_posix().replace("/", "\\"))
        self.assertEqual(config["experimental_record_route_archive"],
                         "Z:" + archive.resolve().as_posix().replace("/", "\\"))
        self.assertEqual((game / "hyprlodr-state/example.hand-hook/hand-mode.txt").read_text(),
                         f"capture\nZ:{capture.resolve().as_posix().replace('/', chr(92))}\n")
        deployment = journal / "deployment.json"
        self.assertEqual(restore_journal(deployment, keep_runtime=False), 0)
        self.assertEqual((game / "OpenAL32.dll").read_bytes(), b"shipped-openal")
        self.assertEqual((game / "OpenAL32_real.dll").read_bytes(), b"prior-forwarder")
        restored_config = json.loads((game / "hyprlodr-config.json").read_text())
        self.assertEqual(restored_config["experimental_record_route_manifest"], "stale-manifest")
        self.assertFalse((game / "hyprlodr-state/example.hand-hook/hand-mode.txt").exists())
        control_capture = self.case / "route-run" / "control.capture"
        control_args = SimpleNamespace(game_dir=game, build=build, archive=archive,
            route_manifest=None, capture_output=control_capture,
            journal=self.case / "journal-control-deploy")
        self.assertEqual(deploy(control_args,
            {"mode": "original-capture", "archive": str(archive.resolve())}), 0)
        control_config = json.loads((game / "hyprlodr-config.json").read_text())
        self.assertIs(control_config["experimental_record_route"], False)
        self.assertNotIn("experimental_record_route_manifest", control_config)
        self.assertNotIn("experimental_record_route_archive", control_config)
        self.assertEqual(restore_journal(control_args.journal / "deployment.json",
                                         keep_runtime=False), 0)


if __name__ == "__main__":
    os.environ.update({"TMPDIR": str(ROOT / ".tmp"), "TMP": str(ROOT / ".tmp"),
                       "TEMP": str(ROOT / ".tmp")})
    unittest.main()
