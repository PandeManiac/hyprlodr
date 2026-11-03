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
    return bytes(24) + struct.pack("<II", 3, len(section)) + section + struct.pack("<II", 0, 0)


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
        self.assertEqual([a["name"] for a in parsed["vertex_attributes"]],
                         ["position", "normal", "uv0", "attribute3", "tangent", "bitangent"])
        self.assertEqual(parsed["graph_tail"]["entry_count"], 0)
        self.assertEqual(parsed["graph_tail"]["reference_count"], 0)

    def test_preserves_named_graph_prefix_and_unknown_tail(self):
        payload = bytearray(fixture_payload())
        tail = struct.pack("<I", 1) + bytes([3]) + b"foo" + bytes.fromhex("0102030405060708")
        tail += struct.pack("<HHI", 0x1234, 0x5678, 0) + b"unknown!"
        section_end = 32 + struct.unpack_from("<I", payload, 28)[0]
        payload = payload[:section_end] + tail
        parsed = model.inspect_payload(bytes(payload))
        graph = parsed["graph_tail"]
        self.assertEqual(graph["entries"][0]["name"], "foo")
        self.assertEqual(graph["entries"][0]["value_raw_hex"], "0102030405060708")
        self.assertEqual(graph["entries"][0]["field_u16_a"], 0x1234)
        self.assertEqual(graph["unparsed_hex"], b"unknown!".hex())

    def test_rejects_truncated_section(self):
        payload = fixture_payload()
        with self.assertRaisesRegex(ValueError, "exceeds the payload"):
            model.inspect_payload(payload[:-12])

    def test_rejects_out_of_range_indices(self):
        with self.assertRaisesRegex(ValueError, "exceeds vertex count"):
            model.inspect_payload(fixture_payload((0, 1, 3)))


if __name__ == "__main__":
    unittest.main()
