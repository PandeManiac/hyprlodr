#!/usr/bin/env python3
"""Tests for tools/title_mask_from_game.py.

The tool recovers a typeface out of an encrypted container, so these tests build their own
container: bytes encrypted with the same recurrence but with synthetic tables, because the real
tables live inside the game executable, which is exactly what the tests must not depend on.

Two halves are tested separately on purpose:

* name recovery uses a hand-made sfnt with only a ``name`` table, which is all the tool reads to
  identify a face;
* rendering uses a real font installed on the system, so PIL has outlines to draw with. That font
  is encrypted into a throwaway container and recovered by the tool before it is rendered, so the
  whole chain - records, crack, decrypt, choose, draw, write - runs without touching the game.

No game file is read, written or committed.
"""
from __future__ import annotations

import importlib.util
import random
import struct
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module          # dataclasses resolve the module by name
    spec.loader.exec_module(module)
    return module


tool = load("title_mask_from_game", TOOLS / "title_mask_from_game.py")
pack_crypt = load("pack_crypt", TOOLS / "re" / "pack_crypt.py")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def synthetic_sfnt(family: str, style: str) -> bytes:
    """A valid-enough sfnt with one `name` table and nothing else."""
    records = b""
    strings = b""
    for name_id, text in ((1, family), (2, style), (4, f"{family} {style}")):
        raw = text.encode("utf-16-be")
        records += struct.pack(">HHHHHH", 3, 1, 0x409, name_id, len(raw), len(strings))
        strings += raw
    name_table = struct.pack(">HHH", 0, 3, 6 + 12 * 3) + records + strings
    header = struct.pack(">IHHHH", 0x00010000, 1, 0, 0, 0)
    entry = b"name" + struct.pack(">III", 0, 12 + 16, len(name_table))
    return header + entry + name_table


def tables(seed: int) -> tuple[bytes, bytes]:
    rng = random.Random(seed)
    return rng.randbytes(256), rng.randbytes(256)


def system_font() -> Path | None:
    """Any installed TrueType face, for the render half."""
    for root in ("/usr/share/fonts", "/usr/local/share/fonts", str(Path.home() / ".fonts")):
        for path in sorted(Path(root).rglob("*.ttf"))[:200] if Path(root).is_dir() else []:
            try:
                if path.stat().st_size > 20_000:
                    return path
            except OSError:
                continue
    return None


def recovers_face_names() -> None:
    t0, t1 = tables(1234)
    plain = synthetic_sfnt("Test Family", "Bold")
    cipher = pack_crypt.encrypt(plain, t0, t1, 0x0234)
    with tempfile.TemporaryDirectory(prefix="hyprlodr-face-") as tmp:
        pack = Path(tmp) / "asset"
        pack.write_bytes(b"\0" * 64 + cipher + b"\0" * 64)   # records never start at 0
        faces = tool.find_faces([(64, len(cipher))], pack, t0, t1)
        check(len(faces) == 1, f"expected one face, found {len(faces)}")
        face = faces[0]
        check((face.family, face.style) == ("Test Family", "Bold"),
              f"name table read incorrectly: {face.family!r} / {face.style!r}")
        check(face.data == plain, "the recovered bytes are not the original file")
        check(face.offset == 64 and face.size == len(cipher), "record geometry was not recorded")
        check(tool.choose(faces, "test", "bold") is face, "choose did not pick the matching face")
    print("PASS recovers_face_names")


def recovers_and_renders() -> None:
    path = system_font()
    if path is None:
        print("SKIP recovers_and_renders (no system font to encrypt)")
        return
    original = path.read_bytes()
    expected = tool.describe(original)
    check(expected is not None, f"the tool cannot read the name table of {path}")
    family, style, _ = expected
    t0, t1 = tables(77)
    cipher = pack_crypt.encrypt(original, t0, t1, 0x03ff)
    # The shipped container puts every font behind a 24-byte slice header, so the test does too.
    prefix = b"\x5c\x52\x00\x01" * 6
    check(len(prefix) == 24, "the test prefix is not 24 bytes")
    with tempfile.TemporaryDirectory(prefix="hyprlodr-face-") as tmp:
        pack = Path(tmp) / "asset"
        pack.write_bytes(b"\0" * 64 + prefix + cipher)
        faces = tool.find_faces([(64, len(prefix) + len(cipher))], pack, t0, t1)
        check(len(faces) == 1, f"expected one face, found {len(faces)}")
        check(faces[0].data == original, "a real font did not survive the container round trip")
        check(faces[0].prefix == 24, f"slice header not stripped (prefix={faces[0].prefix})")
        chosen = tool.choose(faces, family, style)
        face_path = Path(tmp) / "face.ttf"
        face_path.write_bytes(chosen.data)
        coverage = tool.make_title_mask.build(face_path, "HYPR", "LODR", 1.0)
        out = Path(tmp) / "mask.raw"
        size = tool.make_title_mask.write_mask(coverage, out)
        check(size == 853 * 480 * 4, f"mask is {size} B, expected {853 * 480 * 4}")
        check(out.stat().st_size == size, "the mask on disk is the wrong size")
        ink = float((coverage > 127).mean())
        check(ink > 0.05, f"the rendered mask carries almost no lettering (ink={ink:.3f})")
        check(ink < 0.75, f"the rendered mask is a filled block (ink={ink:.3f})")
    print(f"PASS recovers_and_renders ({family} / {style} from {path.name})")


def ignores_non_font_records() -> None:
    t0, t1 = tables(99)
    noise = bytes(range(256)) * 4
    with tempfile.TemporaryDirectory(prefix="hyprlodr-face-") as tmp:
        pack = Path(tmp) / "asset"
        pack.write_bytes(b"\0" * 64 + noise)
        faces = tool.find_faces([(64, len(noise))], pack, t0, t1)
        check(faces == [], "a non-font record was reported as a face")
    print("PASS ignores_non_font_records")


def unknown_family_is_reported() -> None:
    face = tool.Face(offset=0, size=1, seed=0, family="Some Serif", style="Regular",
                     full="Some Serif Regular", data=b"")
    try:
        tool.choose([face], "Georgia", "bold")
    except SystemExit as error:
        check("Some Serif" in str(error), f"the error does not name what was found: {error}")
    else:
        raise AssertionError("choose accepted a family that is not present")
    print("PASS unknown_family_is_reported")


def records_are_read_from_both_shapes() -> None:
    with tempfile.TemporaryDirectory(prefix="hyprlodr-face-") as tmp:
        lines = Path(tmp) / "log.jsonl"
        lines.write_text('{"event":"asset_record","offset":4096,"size":512}\n'
                         '{"event":"other"}\n'
                         '{"offset":"0x1000","size":"0x200"}\n')
        check(tool.load_records(lines) == [(4096, 512)],
              f"JSON lines read incorrectly: {tool.load_records(lines)}")
        single = Path(tmp) / "named.json"
        single.write_text('{"font_a": {"offset": 64, "size": 128}, "junk": 3}')
        check(tool.load_records(single) == [(64, 128)],
              f"single-object log read incorrectly: {tool.load_records(single)}")
    print("PASS records_are_read_from_both_shapes")


def main() -> int:
    recovers_face_names()
    recovers_and_renders()
    ignores_non_font_records()
    unknown_family_is_reported()
    records_are_read_from_both_shapes()
    print("5/5 face recovery scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
