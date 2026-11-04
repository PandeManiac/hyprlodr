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


def topology_edit(payload, source_vertices, indices):
    parsed = model.inspect_payload(payload)
    data = parsed["_data"]
    attributes = {}
    for spec in model.VERTEX_ATTRIBUTES:
        rows = []
        for source_index in source_vertices:
            offset = parsed["vertex_offset"] + source_index * parsed["vertex_stride"] + spec["offset"]
            rows.append(list(struct.unpack_from("<" + "f" * spec["components"], data, offset)))
        attributes[spec["name"]] = rows
    return {"vertex_count": len(source_vertices), "source_vertices": source_vertices,
            "indices": indices, "attributes": attributes}


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

    def test_topology_edit_adds_vertices_and_triangle_and_preserves_tail(self):
        payload = fixture_payload()
        # Add three vertices by cloning source templates; known attributes are supplied
        # while any unsupported per-vertex bytes would come from those source rows.
        templates = [0, 1, 2, 0, 1, 2]
        edit = topology_edit(payload, templates, [0, 1, 2, 3, 4, 5])
        edit["attributes"]["position"][3:] = [[0, 0, 1], [1, 0, 1], [0, 1, 1]]
        graph_tail = (struct.pack("<I", 1) + b"\x03foo" + struct.pack("<II", 0, 3) +
                      struct.pack("<HHI", 0xFFFF, 0, 0) + b"opaque!")
        section_end = 32 + struct.unpack_from("<I", payload, 28)[0]
        payload = payload[:section_end] + graph_tail
        rebuilt = model.rebuild_payload(payload, edit)
        parsed = model.inspect_payload(rebuilt)
        self.assertEqual((parsed["vertex_count"], parsed["index_count"]), (6, 6))
        self.assertEqual(parsed["graph_tail"]["offset"], 32 + parsed["mesh_section_size"])
        self.assertEqual(rebuilt[-len(graph_tail):], graph_tail)
        self.assertEqual(parsed["graph_tail"]["entries"][0]["value_raw_hex"], "0000000003000000")
        self.assertEqual(struct.unpack_from("<3f", rebuilt, parsed["vertex_offset"] + 3 * 72), (0, 0, 1))

    def test_topology_edit_removes_face_and_reorders_vertices(self):
        payload = fixture_payload((0, 1, 2, 0, 2, 1))
        edit = topology_edit(payload, [2, 0, 1], [1, 2, 0])
        rebuilt = model.rebuild_payload(payload, edit)
        parsed = model.inspect_payload(rebuilt)
        self.assertEqual((parsed["vertex_count"], parsed["index_count"]), (3, 3))
        self.assertEqual(struct.unpack_from("<3I", rebuilt, parsed["index_offset"]), (1, 2, 0))
        self.assertEqual(struct.unpack_from("<3f", rebuilt, parsed["vertex_offset"]), (0, 1, 0))

    def test_topology_edits_reject_bad_counts_indices_and_templates(self):
        payload = fixture_payload()
        valid = topology_edit(payload, [0, 1, 2], [0, 1, 2])
        for key, value, message in [
            ("indices", [0, 1], "divisible by three"),
            ("indices", [0, 1, 3], "exceeds vertex count"),
            ("indices", [0, True, 2], "indices\\[1\\] must be an integer"),
            ("source_vertices", [0, 1, 9], "exceeds source vertex count"),
        ]:
            edit = dict(valid)
            edit[key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, message):
                model.rebuild_payload(payload, edit)
        edit = dict(valid)
        edit["attributes"] = dict(valid["attributes"])
        edit["attributes"]["position"] = [[float("inf"), 0, 0], [1, 0, 0], [0, 1, 0]]
        with self.assertRaisesRegex(ValueError, "non-finite"):
            model.rebuild_payload(payload, edit)

    def test_topology_edit_refuses_changed_count_without_template(self):
        payload = fixture_payload()
        edit = topology_edit(payload, [0, 1, 2], [0, 1, 2])
        edit["vertex_count"] = 4
        del edit["source_vertices"]
        with self.assertRaisesRegex(ValueError, "source_vertices templates"):
            model.rebuild_payload(payload, edit)

    def test_topology_rebuild_preserves_declared_section_padding(self):
        payload = bytearray(fixture_payload())
        section_end = 32 + struct.unpack_from("<I", payload, 28)[0]
        payload = payload[:section_end] + b"PAD!" + payload[section_end:]
        struct.pack_into("<I", payload, 28, struct.unpack_from("<I", payload, 28)[0] + 4)
        parsed = model.inspect_payload(bytes(payload))
        self.assertEqual(bytes.fromhex(parsed["mesh_section_padding"]), b"PAD!")
        edit = topology_edit(bytes(payload), [0, 1, 2], [0, 1, 2])
        rebuilt = model.rebuild_payload(bytes(payload), edit)
        result = model.inspect_payload(rebuilt)
        self.assertEqual(bytes.fromhex(result["mesh_section_padding"]), b"PAD!")
        self.assertTrue(rebuilt.endswith(b"\0\0\0\0"))

    def test_rejects_degenerate_indexed_triangles(self):
        with self.assertRaisesRegex(ValueError, "repeated vertex indices"):
            model.inspect_payload(fixture_payload((0, 1, 1)))

    def test_topology_edit_rejects_float32_overflow(self):
        payload = fixture_payload()
        edit = topology_edit(payload, [0, 1, 2], [0, 1, 2])
        edit["attributes"]["position"][0][0] = 1e100
        with self.assertRaisesRegex(ValueError, "float32 range"):
            model.rebuild_payload(payload, edit)

if __name__ == "__main__":
    unittest.main()
