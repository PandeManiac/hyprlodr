import os
import pathlib
import unittest

from tools.hand_capture_deploy import compare_untouched, inside, restore_journal, sha256

ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / ".tmp" / "tests" / "hand-capture-deploy"


class HandCaptureDeployTests(unittest.TestCase):
    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        self.original = TMP / "original.pack"
        self.modified = TMP / "modified.pack"
        self.original.write_bytes(bytes(range(256)) * 4)

    def tearDown(self):
        self.original.unlink(missing_ok=True)
        self.modified.unlink(missing_ok=True)

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
        self.assertTrue(inside(TMP / "asset.patched", ROOT / ".tmp"))
        self.assertFalse(inside(ROOT / "asset.patched", ROOT / ".tmp"))

    def test_restore_rejects_user_changes_before_restoring_any_file(self):
        game = TMP / "game-conflict"
        journal_dir = TMP / "journal-conflict"
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
        game = TMP / "game-restore"
        journal_dir = TMP / "journal-restore"
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


if __name__ == "__main__":
    os.environ.update({"TMPDIR": str(ROOT / ".tmp"), "TMP": str(ROOT / ".tmp"),
                       "TEMP": str(ROOT / ".tmp")})
    unittest.main()
