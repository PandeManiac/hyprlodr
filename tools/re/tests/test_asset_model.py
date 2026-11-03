import importlib.util
import pathlib
import struct
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / "asset_model.py"
SPEC = importlib.util.spec_from_file_location("asset_model", MODULE)
model = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(model)


def fixture_payload(indices=(0, 1, 2)):
    stride = 72
    vertex_count = 3
    index_count = len(indices)
    vertices = bytearray()
    for position in ((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)):
        vertices.extend(struct.pack("<18f", *position, 0.0, 0.0, 1.0,
                                    0.25, 0.75, *([0.0] * 10)))
    section = (struct.pack("<IIIBB", 1, index_count, vertex_count, stride, 0) +
               vertices + struct.pack(f"<{index_count}I", *indices))
    return bytes(24) + struct.pack("<II", 3, len(section)) + section


class AssetModelTests(unittest.TestCase):
    def test_parses_versioned_geometry_and_exports_obj(self):
        parsed = model.inspect_payload(fixture_payload())
        self.assertEqual(parsed["vertex_count"], 3)
        self.assertEqual(parsed["vertex_stride"], 72)
        self.assertEqual(parsed["index_count"], 3)
        self.assertEqual(parsed["bounds"]["x"], [0.0, 1.0])
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp) / "mesh.obj"
            model.write_obj(parsed, output)
            text = output.read_text()
        self.assertIn("vt 0.25 0.75", text)
        self.assertIn("f 1/1/1 2/2/2 3/3/3", text)

    def test_rejects_truncated_section(self):
        payload = fixture_payload()
        with self.assertRaisesRegex(ValueError, "exceeds the payload"):
            model.inspect_payload(payload[:-4])

    def test_rejects_out_of_range_indices(self):
        with self.assertRaisesRegex(ValueError, "exceeds vertex count"):
            model.inspect_payload(fixture_payload((0, 1, 3)))


if __name__ == "__main__":
    unittest.main()
