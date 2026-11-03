import importlib.util
import pathlib
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / "pack_patch.py"
SPEC = importlib.util.spec_from_file_location("pack_patch", MODULE)
patch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patch)


class PackPatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "asset"
        self.source.write_bytes(bytes(range(256)) * 4)

    def tearDown(self):
        self.temp.cleanup()

    def test_patch_changes_only_requested_slice_and_preserves_size(self):
        target = self.root / "copy" / "asset.patched"
        patch.patch_pack(self.source, target, [(300, b"replacement")],
                         expected_size=self.source.stat().st_size)
        original = self.source.read_bytes()
        result = target.read_bytes()
        self.assertEqual(len(result), len(original))
        self.assertEqual(result[:300], original[:300])
        self.assertEqual(result[300:311], b"replacement")
        self.assertEqual(result[311:], original[311:])
        self.assertEqual(self.source.read_bytes(), original)

    def test_rejects_source_alias_and_out_of_bounds_edits_before_writing(self):
        with self.assertRaisesRegex(ValueError, "alias"):
            patch.patch_pack(self.source, self.source, [(0, b"x")])
        target = self.root / "out-of-bounds"
        with self.assertRaisesRegex(ValueError, "bounds"):
            patch.patch_pack(self.source, target, [(1023, b"xx")])
        self.assertFalse(target.exists())

    def test_rejects_overlapping_edits_before_writing(self):
        target = self.root / "overlap"
        with self.assertRaisesRegex(ValueError, "overlap"):
            patch.patch_pack(self.source, target, [(10, b"abcd"), (13, b"ef")])
        self.assertFalse(target.exists())

    def test_rejects_payload_size_mismatch_before_copying(self):
        target = self.root / "wrong-size"
        with self.assertRaisesRegex(ValueError, "expected archive size"):
            patch.patch_pack(self.source, target, [(10, b"x")], expected_size=1)
        self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
