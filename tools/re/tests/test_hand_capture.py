import struct
import unittest

from tools.re import hand_capture


def payload_with_vertices(vertices, indices):
    count = len(vertices) // hand_capture.HAND_STRIDE
    index_count = len(indices) // 4
    section_size = 14 + len(vertices) + len(indices)
    data = bytearray(32 + section_size + 8)
    struct.pack_into("<II", data, 24, 3, section_size)
    struct.pack_into("<IIIBB", data, 32, 1, index_count, count,
                     hand_capture.HAND_STRIDE, 0)
    data[46:46 + len(vertices)] = vertices
    data[46 + len(vertices):46 + len(vertices) + len(indices)] = indices
    return bytes(data)


def make_source():
    vertices = bytearray()
    for index in range(hand_capture.HAND_VERTICES):
        x = float(index % 17)
        y = float(index // 17)
        z = float((index * 7) % 13)
        uv = (index / 300.0, index / 400.0)
        row = struct.pack("<18f", x, y, z, 0, 0, 1, *uv,
                          index / 500.0, 0.25, 0, 0,
                          1, 0, 0, 0, 1, 0)
        vertices.extend(row)
    indices = struct.pack(f"<{hand_capture.HAND_INDICES}I",
                           *(i % hand_capture.HAND_VERTICES
                             for i in range(hand_capture.HAND_INDICES)))
    return bytes(vertices), indices, payload_with_vertices(vertices, indices)


def transformed_vertices(vertices, scale, translation):
    result = bytearray(vertices)
    for index in range(hand_capture.HAND_VERTICES):
        offset = index * hand_capture.HAND_STRIDE
        position = struct.unpack_from("<3f", result, offset)
        struct.pack_into("<3f", result, offset,
                         *(position[i] * scale + translation[i] for i in range(3)))
    return bytes(result)


def capture_fixture(vertices, indices):
    fields = (
        hand_capture.MAGIC, 1, hand_capture.HEADER.size, 1, 0, 7,
        hand_capture.HAND_INDICES, hand_capture.GL_UNSIGNED_INT,
        hand_capture.HAND_VERTICES, hand_capture.HAND_STRIDE,
        hand_capture.HAND_VERTICES * hand_capture.HAND_STRIDE,
        hand_capture.HAND_INDICES, 4,
        0, 123456, 41, 42, *hand_capture.ATTRIBUTE_OFFSETS,
    )
    return hand_capture.HEADER.pack(*fields) + vertices + indices


class HandCaptureTests(unittest.TestCase):
    def test_parses_header_and_hashes_exact_draw_bytes(self):
        vertices, indices, _ = make_source()
        parsed = hand_capture.parse_capture(capture_fixture(vertices, indices))
        self.assertEqual(parsed["draw_ordinal"], 1)
        self.assertEqual(parsed["frame_id_pair_estimate"], 0)
        self.assertEqual(parsed["vertex_sha256"], hand_capture.sha256(vertices))
        self.assertEqual(parsed["index_sha256"], hand_capture.sha256(indices))

    def test_rejects_truncation_and_invalid_indices(self):
        vertices, indices, _ = make_source()
        capture = capture_fixture(vertices, indices)
        with self.assertRaisesRegex(ValueError, "expected"):
            hand_capture.parse_capture(capture[:-1])
        bad_indices = bytearray(indices)
        struct.pack_into("<I", bad_indices, 0, hand_capture.HAND_VERTICES)
        with self.assertRaisesRegex(ValueError, "outside"):
            hand_capture.parse_capture(capture_fixture(vertices, bad_indices))

    def test_recognizes_exact_source_and_transform_consistent_edit(self):
        vertices, indices, original = make_source()
        exact = hand_capture.analyze(capture_fixture(vertices, indices), [("original", original)])
        self.assertEqual(exact["comparisons"][0]["assessment"], "exact-source-bytes")

        edited_vertices = transformed_vertices(vertices, 1.35, (0.125, -0.25, 0.5))
        edited = payload_with_vertices(edited_vertices, indices)
        posed_vertices = transformed_vertices(edited_vertices, 0.75, (-1.0, 0.5, 2.0))
        report = hand_capture.analyze(
            capture_fixture(posed_vertices, indices), [("modified", edited)])
        comparison = report["comparisons"][0]
        self.assertEqual(comparison["assessment"], "transform-consistent-correspondence")
        self.assertTrue(comparison["topology_correspondence"])
        self.assertTrue(comparison["exact_attributes_in_runtime_order"]["uv0"])
        self.assertLess(comparison["position_affine_fit"]["normalized_rmse"], 1e-5)
        self.assertLess(comparison["normal_inverse_transpose_fit"][
            "max_angular_error_degrees"], 0.01)

    def test_recovers_unique_vertex_reordering_from_attributes_and_topology(self):
        vertices, indices, original = make_source()
        permutation = list(reversed(range(hand_capture.HAND_VERTICES)))
        reordered = bytearray(len(vertices))
        for source_index, target_index in enumerate(permutation):
            source_offset = source_index * hand_capture.HAND_STRIDE
            target_offset = target_index * hand_capture.HAND_STRIDE
            reordered[target_offset:target_offset + hand_capture.HAND_STRIDE] = (
                vertices[source_offset:source_offset + hand_capture.HAND_STRIDE])
        remapped_indices = struct.pack(
            f"<{hand_capture.HAND_INDICES}I",
            *(permutation[index] for (index,) in struct.iter_unpack("<I", indices)))
        capture = capture_fixture(bytes(reordered), remapped_indices)
        report = hand_capture.analyze(capture, [("reordered", original)])
        comparison = report["comparisons"][0]
        self.assertEqual(comparison["assessment"], "transform-consistent-correspondence")
        self.assertFalse(comparison["exact_index_array"])
        self.assertTrue(comparison["unique_vertex_remap_from_uv0_and_attribute3"])
        self.assertTrue(comparison["topology_correspondence"])

    def test_does_not_call_positions_transform_consistent_when_normals_disagree(self):
        vertices, indices, original = make_source()
        rotated_normals = bytearray(vertices)
        for index in range(hand_capture.HAND_VERTICES):
            struct.pack_into("<3f", rotated_normals,
                             index * hand_capture.HAND_STRIDE + 12, 0, 1, 0)
        report = hand_capture.analyze(
            capture_fixture(bytes(rotated_normals), indices), [("normal-mismatch", original)])
        comparison = report["comparisons"][0]
        self.assertIn("normals disagree", comparison["assessment"])
        self.assertGreater(comparison["normal_inverse_transpose_fit"][
            "max_angular_error_degrees"], 89.0)

    def test_does_not_guess_duplicate_attribute_mappings(self):
        vertices, indices, original = make_source()
        duplicate = bytearray(vertices)
        duplicate[hand_capture.HAND_STRIDE + 24:hand_capture.HAND_STRIDE + 48] = duplicate[24:48]
        changed = payload_with_vertices(duplicate, indices)
        capture = capture_fixture(vertices, indices)
        report = hand_capture.analyze(capture, [("ambiguous", changed)])
        comparison = report["comparisons"][0]
        self.assertFalse(comparison["unique_vertex_remap_from_uv0_and_attribute3"])
        self.assertEqual(comparison["assessment"], "inconclusive")


if __name__ == "__main__":
    unittest.main()
