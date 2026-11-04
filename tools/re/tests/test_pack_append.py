import hashlib
import importlib.util
import json
import pathlib
import struct
import tempfile
import unittest
from unittest import mock

MODULE = pathlib.Path(__file__).resolve().parents[1] / "pack_append.py"
SPEC = importlib.util.spec_from_file_location("pack_append", MODULE)
append = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(append)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def model_payload(vertices=4):
    stride = 72
    indices = [0, 1, 2, 1, 2, 3]
    section_size = 14 + vertices * stride + len(indices) * 4
    data = bytearray(0x18 + 8 + section_size + 4)
    struct.pack_into("<II", data, 0x18, 3, section_size)
    struct.pack_into("<IIIB", data, 0x20, 0, len(indices), vertices, stride)
    vertex_start = 0x20 + 14
    for i in range(vertices):
        struct.pack_into("<6f", data, vertex_start + i * stride,
                         float(i), 0.0, 0.0, 0.0, 0.0, 1.0)
    struct.pack_into(f"<{len(indices)}I", data, vertex_start + vertices * stride, *indices)
    return bytes(data)


class PackAppendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.exe = self.root / "hyperdemon.exe"
        self.pack = self.root / "asset"
        self.payload = self.root / "hand2.bin"
        self.index_path = self.root / "index.json"
        self.out = self.root / "asset.appended"
        self.manifest = self.root / "manifest.json"
        self.exe.write_bytes(b"synthetic executable fingerprint fixture")
        archive = bytearray(b"prefix!!" + bytes(range(32)) + b"untouched-tail")
        self.offset, self.size = 8, 32
        self.pack.write_bytes(archive)
        self.payload.write_bytes(model_payload())
        self.cipher = bytes(archive[self.offset:self.offset + self.size])
        self.pack_hash, self.exe_hash = digest(bytes(archive)), digest(self.exe.read_bytes())
        self.cipher_hash = digest(self.cipher)
        row = {
            "id": "rec-011a0ebc0-00000020-0001", "name": "hand2",
            "offset": self.offset, "size": self.size, "tag": 1,
            "association": "verified", "record_evidence": "resolver-linked",
            "encrypted_sha256": self.cipher_hash,
            "resolver_mappings": [{"name": "hand2", "offset": self.offset,
                "size": self.size, "tag": 1, "resolver_table_id": "tbl-e91fb3b80004"}],
        }
        self.index = {"source": {"pack_sha256": self.pack_hash, "exe_sha256": self.exe_hash},
                      "records": [row]}
        self.index_path.write_text(json.dumps(self.index))
        self.crypt_patch = mock.patch.multiple(append,
            load_tables=mock.DEFAULT, seed_for_name=mock.DEFAULT,
            encrypt=mock.DEFAULT, decrypt=mock.DEFAULT)
        mocks = self.crypt_patch.start()
        mocks["load_tables"].return_value = (b"t0", b"t1", None)
        mocks["seed_for_name"].return_value = 0x2FE
        mocks["encrypt"].side_effect = lambda data, *_: data
        mocks["decrypt"].side_effect = lambda data, *_: data
        self.addCleanup(self.crypt_patch.stop)

    def tearDown(self):
        self.temp.cleanup()

    def build(self):
        return append.build_append_archive(exe=self.exe, pack=self.pack,
            index_path=self.index_path, payload_path=self.payload, output=self.out,
            manifest_path=self.manifest, name="hand2", record_id=self.index["records"][0]["id"],
            table_id="tbl-e91fb3b80004", offset=self.offset, size=self.size, tag=1,
            expected_pack_sha256=self.pack_hash, expected_exe_sha256=self.exe_hash,
            expected_record_cipher_sha256=self.cipher_hash)

    def test_appends_payload_and_stand_in_selects_exact_verified_record(self):
        manifest = self.build()
        original = self.pack.read_bytes()
        result = self.out.read_bytes()
        record = manifest["record"]
        self.assertEqual(result[:len(original)], original)
        self.assertEqual(record["replacement_offset"], len(original))
        self.assertEqual(record["replacement_size"], len(self.payload.read_bytes()))
        self.assertEqual(record["mesh"], {"vertex_count": 4, "index_count": 6, "vertex_stride": 72})
        decoded, model = append.validate_override_read(exe=self.exe, archive=self.out,
            manifest_path=self.manifest, observed_record={"name": "hand2", "record_id": record["record_id"],
            "resolver_table_id": record["resolver_table_id"], "offset": self.offset,
            "size": self.size, "tag": 1})
        self.assertEqual(decoded, self.payload.read_bytes())
        self.assertEqual((model["vertex_count"], model["index_count"]), (4, 6))

    def test_rejects_wrong_tag_identity_and_ambiguous_table(self):
        with self.assertRaisesRegex(ValueError, "metadata"):
            append._record(self.index, name="hand2", record_id=self.index["records"][0]["id"],
                table_id="tbl-e91fb3b80004", offset=self.offset, size=self.size, tag=2)
        bad = json.loads(json.dumps(self.index))
        bad["records"][0]["resolver_mappings"][0]["resolver_table_id"] = "other"
        with self.assertRaisesRegex(ValueError, "exactly one"):
            append._record(bad, name="hand2", record_id=bad["records"][0]["id"],
                table_id="tbl-e91fb3b80004", offset=self.offset, size=self.size, tag=1)
        bad["records"][0]["association"] = "inferred"
        with self.assertRaisesRegex(ValueError, "not resolver-verified"):
            append._record(bad, name="hand2", record_id=bad["records"][0]["id"],
                table_id="tbl-e91fb3b80004", offset=self.offset, size=self.size, tag=1)

    def test_rejects_fingerprint_metadata_and_output_range_mismatches(self):
        with self.assertRaisesRegex(ValueError, "fingerprint mismatch"):
            append.build_append_archive(exe=self.exe, pack=self.pack, index_path=self.index_path,
                payload_path=self.payload, output=self.out, manifest_path=self.manifest,
                name="hand2", record_id=self.index["records"][0]["id"], table_id="tbl-e91fb3b80004",
                offset=self.offset, size=self.size, tag=1, expected_pack_sha256="wrong",
                expected_exe_sha256=self.exe_hash, expected_record_cipher_sha256=self.cipher_hash)
        with self.assertRaisesRegex(ValueError, "metadata"):
            append.build_append_archive(exe=self.exe, pack=self.pack, index_path=self.index_path,
                payload_path=self.payload, output=self.out, manifest_path=self.manifest,
                name="hand2", record_id=self.index["records"][0]["id"], table_id="tbl-e91fb3b80004",
                offset=self.offset + 1, size=self.size, tag=1, expected_pack_sha256=self.pack_hash,
                expected_exe_sha256=self.exe_hash, expected_record_cipher_sha256=self.cipher_hash)
        self.build()
        record = json.loads(self.manifest.read_text())["record"]
        with self.assertRaisesRegex(ValueError, "does not match"):
            append.validate_override_read(exe=self.exe, archive=self.out, manifest_path=self.manifest,
                observed_record={"name": "hand2", "record_id": record["record_id"],
                    "resolver_table_id": "wrong", "offset": self.offset,
                    "size": self.size, "tag": 1})
        self.out.write_bytes(self.out.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, "fingerprint mismatch"):
            append.validate_override_read(exe=self.exe, archive=self.out, manifest_path=self.manifest,
                observed_record={"name": "hand2", "record_id": record["record_id"],
                    "resolver_table_id": record["resolver_table_id"], "offset": self.offset,
                    "size": self.size, "tag": 1})

    def test_rejects_invalid_record_ranges_and_missing_payload(self):
        for offset, size in ((1000, self.size), (self.offset, 1000)):
            index = json.loads(json.dumps(self.index))
            row = index["records"][0]
            row["offset"], row["size"] = offset, size
            row["resolver_mappings"][0]["offset"] = offset
            row["resolver_mappings"][0]["size"] = size
            self.index_path.write_text(json.dumps(index))
            with self.assertRaisesRegex(ValueError, "bounds"):
                append.build_append_archive(exe=self.exe, pack=self.pack, index_path=self.index_path,
                    payload_path=self.payload, output=self.out, manifest_path=self.manifest,
                    name="hand2", record_id=row["id"], table_id="tbl-e91fb3b80004",
                    offset=offset, size=size, tag=1, expected_pack_sha256=self.pack_hash,
                    expected_exe_sha256=self.exe_hash,
                    expected_record_cipher_sha256=self.cipher_hash)
        with self.assertRaisesRegex(ValueError, "must all exist"):
            append.build_append_archive(exe=self.exe, pack=self.pack, index_path=self.index_path,
                payload_path=self.root / "absent.bin", output=self.out, manifest_path=self.manifest,
                name="hand2", record_id=self.index["records"][0]["id"],
                table_id="tbl-e91fb3b80004", offset=self.offset, size=self.size, tag=1,
                expected_pack_sha256=self.pack_hash, expected_exe_sha256=self.exe_hash,
                expected_record_cipher_sha256=self.cipher_hash)

    def test_initialization_and_write_failures_leave_no_partial_artifacts(self):
        with mock.patch.object(append, "load_tables", side_effect=RuntimeError("fixture init failure")):
            with self.assertRaisesRegex(RuntimeError, "fixture init failure"):
                self.build()
        self.assertFalse(self.out.exists())
        self.assertFalse(self.manifest.exists())
        with mock.patch.object(append.os, "fsync", side_effect=OSError("fixture flush failure")):
            with self.assertRaisesRegex(OSError, "fixture flush failure"):
                self.build()
        self.assertFalse(self.out.exists())
        self.assertFalse(self.manifest.exists())
        self.assertFalse(self.out.with_name(self.out.name + ".partial").exists())
        self.assertFalse(self.manifest.with_name(self.manifest.name + ".partial").exists())

    def test_refuses_to_remove_an_existing_partial_artifact(self):
        partial = self.out.with_name(self.out.name + ".partial")
        partial.write_bytes(b"user-owned partial")
        with self.assertRaisesRegex(ValueError, "existing partial"):
            self.build()
        self.assertEqual(partial.read_bytes(), b"user-owned partial")

    def test_stand_in_rejects_invalid_replacement_extent(self):
        self.build()
        manifest = json.loads(self.manifest.read_text())
        manifest["record"]["replacement_offset"] = self.out.stat().st_size + 1
        self.manifest.write_text(json.dumps(manifest))
        record = manifest["record"]
        with self.assertRaisesRegex(ValueError, "range is invalid"):
            append.validate_override_read(exe=self.exe, archive=self.out,
                manifest_path=self.manifest, observed_record={"name": "hand2",
                    "record_id": record["record_id"], "resolver_table_id": record["resolver_table_id"],
                    "offset": self.offset, "size": self.size, "tag": 1})


if __name__ == "__main__":
    unittest.main()
