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

    def test_live_tree_mapping_is_verified(self):
        with self.evidence.open("a") as stream:
            stream.write("\n" + json.dumps({
                "event": "asset_mapping", "offset": 16, "size": 64, "tag": 4096,
                "name": "live_name", "index": 0, "node": "0x1234",
            }))
        with mock.patch.object(assets, "load_tables", return_value=(bytes(256), bytes(256), bytes(1024))):
            row = assets.build_index(self.pack, self.exe, self.evidence, None)["records"][0]
        self.assertEqual(row["name"], "live_name")
        self.assertEqual(row["association"], "verified")
        self.assertEqual(len(row["name_evidence_lines"]), 1)
        self.assertEqual(row["record_evidence"], "resolver-linked")
        self.assertEqual(row["resolver_mappings"][0]["index"], 0)
        table_id = row["resolver_table_ids"][0]
        selected = assets.pick({"records": [row]}, f"{table_id}:live_name")
        self.assertEqual(selected["id"], row["id"])
        self.assertEqual(selected["_selected_name"], "live_name")

    def test_qualified_duplicate_name_selects_exact_resolver_mapping(self):
        events = [
            {"event": "asset_mapping", "offset": 16, "size": 64, "tag": 4096,
             "name": "shared", "index": 0, "record_array": "0x1000", "table": "0x2000"},
            {"event": "asset_mapping", "offset": 100, "size": 32, "tag": 1,
             "name": "other", "index": 0, "record_array": "0x3000", "table": "0x4000"},
            {"event": "asset_mapping", "offset": 100, "size": 32, "tag": 1,
             "name": "shared", "index": 1, "record_array": "0x3000", "table": "0x4000"},
        ]
        self.evidence.write_text("\n".join(json.dumps(e) for e in events))
        tables = (bytes(256), bytes(256), bytes(1024))
        with mock.patch.object(assets, "load_tables", return_value=tables):
            index = assets.build_index(self.pack, self.exe, self.evidence, None)
        ids = {row["name"]: row["id"] for row in index["records"]}
        first = next(row for row in index["records"] if row["offset"] == 16)
        second = next(row for row in index["records"] if row["offset"] == 100)
        first_table = first["resolver_mappings"][0]["resolver_table_id"]
        second_table = second["resolver_mappings"][1]["resolver_table_id"]
        self.assertNotEqual(first_table, second_table)
        self.assertEqual(assets.pick(index, f"{first_table}:shared")["_selected_name"], "shared")
        self.assertEqual(assets.pick(index, f"{second_table}:shared")["_selected_name"], "shared")
        self.assertNotEqual(first["id"], second["id"])

    def test_unresolved_tree_key_hex_recovers_printable_name(self):
        with self.evidence.open("a") as stream:
            stream.write("\n" + json.dumps({
                "event": "asset_mapping_unresolved", "offset": 16, "size": 64,
                "tag": 4096, "name_length": 11, "name_capacity": 15,
                "name_hex": "7061696e206261636b7570", "index": 0,
                "record_array": "0x1000", "table": "0x2000",
            }))
        with mock.patch.object(assets, "load_tables", return_value=(bytes(256), bytes(256), bytes(1024))):
            row = assets.build_index(self.pack, self.exe, self.evidence, None)["records"][0]
        self.assertEqual(row["name"], "pain backup")
        self.assertEqual(row["association"], "verified")
        self.assertEqual(row["resolver_mappings"][0]["source_event"],
                         "asset_mapping_unresolved")

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
