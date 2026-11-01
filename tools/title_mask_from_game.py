#!/usr/bin/env python3
"""Recover the title typeface from the user's own installation and render the mask.

The replacement's letterforms come from a face inside the game's own asset container, and no byte
of that container - nor a copy of the face - is committed to this repository. So the face is
recovered at run time from the installation the user already has:

1. while the game runs, the proxy records every slice the resolver touches in
   ``hyprlodr-assets.jsonl`` as ``(offset, size)`` (see ``docs/RE_PACK.md``);
2. the slice cipher has only 1024 keys, so the first bytes of every record are decrypted under all
   of them at once and tested for an sfnt signature - a TrueType/OpenType file;
3. the ``name`` table then identifies the family and subfamily, which is how the right face is
   picked even though the container never stores the names it ships under;
4. the words are drawn onto the grid measured from the original mask, by ``make_title_mask``.

Run the game once with the loader installed, then::

    python3 tools/title_mask_from_game.py --list          # what is inside this installation
    python3 tools/title_mask_from_game.py --out assets/hyprlodr-title-mask.raw

``--dump-dir`` keeps the recovered face, so later runs can go straight to
``make_title_mask.py --font <face>``.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module          # dataclasses resolve the module by name
    spec.loader.exec_module(module)
    return module


pack_crypt = _load("pack_crypt", HERE / "re" / "pack_crypt.py")
make_title_mask = _load("make_title_mask", HERE / "make_title_mask.py")

SFNT_MAGIC = (b"\x00\x01\x00\x00", b"OTTO", b"true", b"ttcf", b"typ1")
NAME_IDS = {1: "family", 2: "style", 4: "full"}
ITALIC = ("italic", "oblique", "cursiv", "kurzív", "kurziv", "kursiv")
BOLD = ("bold", "negreta", "negrito", "gras", "fett", "black", "heavy", "tučn")


@dataclass
class Face:
    offset: int
    size: int
    seed: int
    family: str
    style: str
    full: str
    data: bytes
    prefix: int = 0

    @property
    def label(self) -> str:
        return f"{self.family} {self.style}".strip()


def default_game() -> Path:
    override = os.environ.get("HYPRLODR_GAME_DIR")
    if override:
        return Path(override).expanduser()
    return Path.home() / ".local/share/Steam/steamapps/common/hyperdemon"


def _as_int(value) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def load_records(path: Path) -> list[tuple[int, int]]:
    """``(offset, size)`` pairs from the proxy's evidence log, JSON lines or one object."""
    raw = path.read_text(errors="replace")
    entries = []
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            entries.append(json.loads(line))
        except json.JSONDecodeError:
            entries = None
            break
    if entries is None:
        try:
            whole = json.loads(raw)
        except json.JSONDecodeError as error:
            raise SystemExit(f"error: {path} is neither JSON lines nor JSON: {error}")
        entries = list(whole.values()) if isinstance(whole, dict) else list(whole)
    records = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        # A log written as one object keyed by entry name holds its records one level down.
        candidates = [entry]
        if "offset" not in entry:
            candidates = [value for value in entry.values() if isinstance(value, dict)]
        for candidate in candidates:
            try:
                offset, size = _as_int(candidate["offset"]), _as_int(candidate["size"])
            except (KeyError, TypeError, ValueError):
                continue
            if offset > 0 and size > 0:
                records.add((offset, size))
    return sorted(records)


def sfnt_tables(plain: bytes) -> dict[bytes, tuple[int, int]]:
    """Table tag -> (offset, length), or an empty map when the header is not an sfnt."""
    if len(plain) < 12 or plain[:4] not in SFNT_MAGIC:
        return {}
    count = struct.unpack_from(">H", plain, 4)[0]
    if not 0 < count < 96:
        return {}
    tables: dict[bytes, tuple[int, int]] = {}
    for index in range(count):
        base = 12 + index * 16
        if base + 16 > len(plain):
            return {}
        tag = plain[base:base + 4]
        offset, length = struct.unpack_from(">II", plain, base + 8)
        if offset + length <= len(plain):
            tables[tag] = (offset, length)
    return tables


def name_strings(plain: bytes, table: tuple[int, int]) -> dict[int, str]:
    """The ``name`` table's family/style/full strings, keyed by name ID."""
    base = table[0]
    if base + 6 > len(plain):
        return {}
    _, count, string_offset = struct.unpack_from(">HHH", plain, base)
    strings: dict[int, str] = {}
    for index in range(min(count, 512)):
        position = base + 6 + index * 12
        if position + 12 > len(plain):
            break
        platform, _, _, name_id, length, offset = struct.unpack_from(">HHHHHH", plain, position)
        if name_id not in NAME_IDS or name_id in strings:
            continue
        start = base + string_offset + offset
        chunk = plain[start:start + length]
        if not chunk:
            continue
        text = chunk.decode("utf-16-be" if platform in (0, 3) else "latin-1", "ignore")
        text = text.replace("\x00", "").strip()
        if text:
            strings[name_id] = text
    return strings


def describe(plain: bytes) -> tuple[str, str, str] | None:
    """``(family, style, full)`` for an sfnt that carries a usable name table."""
    tables = sfnt_tables(plain)
    if b"name" not in tables:
        return None
    names = name_strings(plain, tables[b"name"])
    family = names.get(1)
    if not family:
        return None
    return family, names.get(2, ""), names.get(4, family)


def sfnt_hits(matrix, limit: int = 64) -> list[tuple[int, int]]:
    """``(key, prefix)`` pairs whose decrypted head starts an sfnt within ``limit`` bytes."""
    import numpy as np

    window = matrix.shape[1] - 3
    if window <= 0:
        return []
    hits: list[tuple[int, int]] = []
    for magic in SFNT_MAGIC:
        target = np.frombuffer(magic, dtype=np.uint8)
        match = np.ones((matrix.shape[0], window), dtype=bool)
        for offset in range(4):
            match &= matrix[:, offset:offset + window] == target[offset]
        for key, prefix in np.argwhere(match):
            if prefix < limit:
                hits.append((int(key), int(prefix)))
    return hits


def find_faces(records: list[tuple[int, int]], pack: Path, t0: bytes, t1: bytes,
               head: int = 160) -> list[Face]:
    """Decrypt the head of every record under all 1024 keys and keep the fonts.

    A slice can carry a header before the file it names - every title font in the shipped
    container sits behind a 24-byte one, which is why the signature is searched for rather
    than assumed to be at byte 0.
    """
    stream = pack_crypt.keystream_bytes(t0, t1)
    faces: list[Face] = []
    with pack.open("rb") as handle:
        for offset, size in records:
            if size < 64 or size > (32 << 20):
                continue
            handle.seek(offset)
            payload = handle.read(size)
            if len(payload) != size:
                continue
            matrix = pack_crypt.crack_matrix(payload[:head], t0, t1, stream)
            for key, prefix in sfnt_hits(matrix):
                plain = pack_crypt.decrypt(payload, t0, t1, key)
                info = describe(plain[prefix:])
                if info is None:
                    continue
                family, style, full = info
                faces.append(Face(offset=offset, size=size, seed=key, family=family,
                                  style=style, full=full, data=plain[prefix:],
                                  prefix=prefix))
    return faces


def choose(faces: list[Face], family: str, style: str) -> Face:
    """The face matching ``family`` that is closest to ``style``."""
    matches = [face for face in faces if family.lower() in face.family.lower()]
    if not matches:
        seen = sorted({face.label for face in faces})
        raise SystemExit("error: no recovered face matches %r; found: %s"
                         % (family, ", ".join(seen) if seen else "none"))

    wants_italic = any(word in style.lower() for word in ITALIC)
    wants_bold = any(word in style.lower() for word in BOLD)

    def rank(face: Face) -> tuple:
        name = face.label.lower()
        italic = any(word in name for word in ITALIC)
        bold = any(word in name for word in BOLD)
        return (italic != wants_italic, bold != wants_bold, face.size)

    return sorted(matches, key=rank)[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", type=Path, default=default_game(),
                        help="installation folder (default: %(default)s)")
    parser.add_argument("--records", type=Path,
                        help="asset record log (default: <game>/hyprlodr-assets.jsonl)")
    parser.add_argument("--pack", type=Path, help="default: <game>/asset/asset")
    parser.add_argument("--exe", type=Path, help="default: <game>/hyperdemon.exe")
    parser.add_argument("--family", default="Georgia", help="family to pick (default: %(default)s)")
    parser.add_argument("--style", default="bold", help="style to prefer (default: %(default)s)")
    parser.add_argument("--top", default="HYPR")
    parser.add_argument("--bottom", default="LODR")
    parser.add_argument("--out", type=Path, default=REPO / "assets" / "hyprlodr-title-mask.raw")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--dump-dir", type=Path,
                        help="keep the recovered face here as a .ttf")
    parser.add_argument("--list", action="store_true",
                        help="report the faces and their names, render nothing")
    args = parser.parse_args(argv)

    exe = args.exe or args.game / "hyperdemon.exe"
    pack = args.pack or args.game / "asset" / "asset"
    records_path = args.records or args.game / "hyprlodr-assets.jsonl"
    for path, what in ((exe, "executable"), (pack, "asset container"),
                       (records_path, "asset record log")):
        if not path.is_file():
            raise SystemExit(f"error: no {what} at {path}\n"
                             "Run the game once with the loader installed so the proxy records "
                             "the slices the resolver touches.")

    t0, t1, _ = pack_crypt.load_tables(exe)
    records = load_records(records_path)
    faces = find_faces(records, pack, t0, t1)
    print(f"records     : {len(records)} slices recorded by the proxy")
    print(f"faces       : {len(faces)} font file(s) inside them")
    for face in sorted(faces, key=lambda item: (item.family, item.style)):
        print(f"  {face.family} / {face.style or '-'}  ({face.size} B at {face.offset:#x})")
    if args.list:
        return 0
    if not faces:
        raise SystemExit("error: none of the recorded slices is a font; the resolver may not have "
                         "touched the face this far into the log")

    face = choose(faces, args.family, args.style)
    print(f"chosen      : {face.label} ({face.size} B)")
    with tempfile.TemporaryDirectory(prefix="hyprlodr-face-") as tmp:
        if args.dump_dir:
            path = args.dump_dir / (face.label.replace(" ", "_") + ".ttf")
            path.parent.mkdir(parents=True, exist_ok=True)
        else:
            path = Path(tmp) / "face.ttf"
        path.write_bytes(face.data)
        coverage = make_title_mask.build(path, args.top, args.bottom, 1.0)
        make_title_mask.write_mask(coverage, args.out, args.preview)
        if args.dump_dir:
            print(f"face saved  : {path}")
    ink = float((coverage > 127).mean())
    print(f"{args.out}: {make_title_mask.WIDTH}x{make_title_mask.HEIGHT} "
          f"ink={ink:.3f} (words {args.top} / {args.bottom})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
