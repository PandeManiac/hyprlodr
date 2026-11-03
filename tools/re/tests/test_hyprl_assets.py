import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest import mock

MODULE = pathlib.Path(__file__).resolve().parents[1] / "hyprl_assets.py"
SPEC = importlib.util.spec_from_file_location("hyprl_assets", MODULE)
assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(assets)


class AssetIndexTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.pack = self.root / "asset"
        self.pack.write_bytes(bytes(range(256)) * 4)
        self.exe = self.root / "game.exe"
        self.exe.write_bytes(b"exe fixture")
        self.evidence = self.root / "capture.jsonl"
        events = [
            {"event": "asset_record", "offset": 16, "size": 64, "tag": 4096},
            {"event": "asset_record", "offset": 16, "size": 64, "tag": 4096},
            {"event": "asset_record", "offset": 1008, "size": 16, "tag": 1},
            {"event": "asset_record", "offset": 1000, "size": 64, "tag": 1},
            {"event": "asset_record", "offset": 0, "size": 0, "tag": 2},
        ]
        self.evidence.write_text("\n".join(json.dumps(e) for e in events))

    def tearDown(self):
        self.temp.cleanup()

    def test_scan_deduplicates_and_rejects_out_of_bounds(self):
        index = assets.build_index(self.pack, self.exe, self.evidence, None)
        self.assertEqual(len(index["records"]), 2)
        row = index["records"][0]
        self.assertEqual(row["id"], "rec-000000010-00000040-1000")
        self.assertEqual(row["association"], "unknown")
        self.assertEqual(len(row["encrypted_sha256"]), 64)

    def test_name_map_collision_is_kept_ambiguous(self):
        names = self.root / "names.json"
        names.write_text(json.dumps({
            "one": {"offset": 16, "size": 64, "tag": 4096},
            "two": {"offset": 16, "size": 64, "tag": 4096},
        }))
        tables = (bytes(256), bytes(256), bytes(1024))
        with mock.patch.object(assets, "load_tables", return_value=tables):
            row = assets.build_index(self.pack, self.exe, self.evidence, names)["records"][0]
        self.assertIsNone(row["name"])
        self.assertEqual(row["association"], "ambiguous")
        self.assertEqual(row["name_candidates"], ["one", "two"])

    def test_extract_requires_explicit_key_for_unknown_record(self):
        row = {"offset": 16, "size": 32}
        with self.assertRaisesRegex(ValueError, "needs --name"):
            assets.extract(row, self.pack, self.exe, self.root / "out.bin")

    def test_extract_refuses_to_overwrite_source_archive(self):
        row = {"offset": 16, "size": 32}
        with self.assertRaisesRegex(ValueError, "refusing to modify"):
            assets.extract(row, self.pack, self.exe, self.pack, key_class="0")

    def test_cache_is_bound_to_archive_executable_evidence_and_name_map(self):
        names = self.root / "names.json"
        names.write_text("{}")
        index = assets.build_index(self.pack, self.exe, self.evidence, names)
        self.assertTrue(assets.cache_matches(index, self.pack, self.exe, self.evidence, names))
        names.write_text('{"changed":true}')
        self.assertFalse(assets.cache_matches(index, self.pack, self.exe, self.evidence, names))

    def test_extract_uses_bounded_slice_and_decrypts(self):
        row = {"offset": 16, "size": 32}
        destination = self.root / "out.bin"
        with mock.patch.object(assets, "load_tables", return_value=(bytes(256), bytes(256), bytes(1024))):
            seed = assets.extract(row, self.pack, self.exe, destination, key_class="0")
        self.assertEqual(seed, 0)
        self.assertEqual(len(destination.read_bytes()), 32)


if __name__ == "__main__":
    unittest.main()
