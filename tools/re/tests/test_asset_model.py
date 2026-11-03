import importlib.util
import pathlib
import struct
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / "asset_model.py"
SPEC = importlib.util.spec_from_file_location("asset_model", MODULE)
model = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(model)
CRYPT_MODULE = MODULE.with_name("pack_crypt.py")
CRYPT_SPEC = importlib.util.spec_from_file_location("pack_crypt", CRYPT_MODULE)
crypt = importlib.util.module_from_spec(CRYPT_SPEC)
CRYPT_SPEC.loader.exec_module(crypt)


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

    def test_lossless_no_change_round_trip(self):
        payload = fixture_payload()
        self.assertEqual(model.rebuild_payload(payload), payload)

    def test_edit_patches_only_requested_vertex_components(self):
        payload = fixture_payload()
        parsed = model.inspect_payload(payload)
        edit = {"vertex_count": 3, "attributes": {"position": [[0.125, 0, 0]]}}
        with self.assertRaisesRegex(ValueError, "one row per source vertex"):
            model.rebuild_payload(payload, edit)
        edit["attributes"]["position"] = [[0.125, 0, 0], [1, 0, 0], [0, 1, 0]]
        rebuilt = model.rebuild_payload(payload, edit)
        start = parsed["vertex_offset"]
        self.assertEqual(struct.unpack_from("<f", rebuilt, start)[0], 0.125)
        self.assertEqual(rebuilt[:start], payload[:start])
        self.assertEqual(rebuilt[start + 4:], payload[start + 4:])
        self.assertEqual(model.inspect_payload(rebuilt)["index_count"], 3)

    def test_edit_rejects_nonfinite_unknown_and_count_changing_data(self):
        payload = fixture_payload()
        for edit, message in [
            ({"vertex_count": 4, "attributes": {"position": []}}, "vertex_count"),
            ({"vertex_count": 3, "attributes": {"weights": [[0]]}}, "unsupported"),
            ({"vertex_count": 3, "attributes": {"position": [[float("nan"), 0, 0]] * 3}}, "non-finite"),
        ]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                model.rebuild_payload(payload, edit)

    def test_structural_corruption_is_rejected_before_round_trip(self):
        payload = bytearray(fixture_payload())
        struct.pack_into("<I", payload, 32 + 4, 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "invalid triangle index count"):
            model.rebuild_payload(bytes(payload))
        payload = bytearray(fixture_payload())
        graph_offset = 32 + struct.unpack_from("<I", payload, 28)[0]
        struct.pack_into("<I", payload, graph_offset, 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "graph tail"):
            model.rebuild_payload(bytes(payload))

    def test_rebuilt_payload_encrypts_and_decrypts_identically(self):
        payload = fixture_payload()
        rebuilt = model.rebuild_payload(payload)
        t0 = bytes((i * 17 + 3) & 0xff for i in range(256))
        t1 = bytes((i * 29 + 11) & 0xff for i in range(256))
        seed = crypt.seed_for_name("synthetic-hand")
        encrypted = crypt.encrypt(rebuilt, t0, t1, seed)
        self.assertNotEqual(encrypted, rebuilt)
        self.assertEqual(crypt.decrypt(encrypted, t0, t1, seed), payload)

    def test_rejects_truncated_section(self):
        payload = fixture_payload()
        with self.assertRaisesRegex(ValueError, "exceeds the payload"):
            model.inspect_payload(payload[:-12])

    def test_rejects_out_of_range_indices(self):
        with self.assertRaisesRegex(ValueError, "exceeds vertex count"):
            model.inspect_payload(fixture_payload((0, 1, 3)))


if __name__ == "__main__":
    unittest.main()
