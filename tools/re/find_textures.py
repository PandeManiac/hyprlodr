#!/usr/bin/env python3
"""Find image-shaped slices in HYPER DEMON's ``asset/asset``.

The pack stores no DDS/PNG/KTX magic: a payload is a 0x18-byte header followed by raw pixel
data, so a texture has to be recognised by its *geometry* — a width/height pair near the start
whose extent accounts for the remaining bytes at 1, 2, 3 or 4 bytes per pixel.

Every record is decrypted with the key class recovered from its own content, then checked for
such a pair. Candidates are written out as 8-bit PNGs (alpha channel when 4 bpp) so they can be
looked at; nothing is written into the game directory.

Usage::

    python3 tools/re/find_textures.py --game "$GAME" --out .tmp/tex [--max-candidates 40]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from pack_crypt import keystream_bytes, load_tables

MIN_SIDE = 48
MAX_SIDE = 8192


def decrypt_all(cipher: bytes, seed: int, key_stream: np.ndarray) -> bytes:
    data = np.frombuffer(cipher, dtype=np.uint8)
    signed = data.astype(np.int32)
    signed[signed >= 128] -= 256
    steps = np.cumsum(1 + signed, dtype=np.int64) - (1 + signed)
    return (key_stream[(seed + steps) & 0x3FF] ^ data).tobytes()


def best_seed(cipher: bytes, key_stream: np.ndarray) -> int:
    """Key class whose decryption looks most like structured payload (printable-run count)."""
    data = np.frombuffer(cipher[:512], dtype=np.uint8)
    signed = data.astype(np.int32)
    signed[signed >= 128] -= 256
    steps = np.cumsum(1 + signed, dtype=np.int64) - (1 + signed)
    matrix = key_stream[(np.arange(0x400, dtype=np.int64)[:, None] + steps[None, :]) & 0x3FF] \
        ^ data[None, :]
    printable = ((matrix >= 0x20) & (matrix < 0x7F)).astype(np.int32)
    if matrix.shape[1] >= 4:
        cumulative = np.cumsum(printable, axis=1)
        score = (cumulative[:, 4:] - cumulative[:, :-4] == 4).sum(axis=1)
    else:
        score = printable.sum(axis=1)
    return int(score.argmax())


def write_png(pixels: bytes, width: int, height: int, path: pathlib.Path) -> None:
    rows = b"".join(b"\x00" + pixels[y * width:(y + 1) * width] for y in range(height))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 6))
                     + chunk(b"IEND", b""))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", required=True, type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, help="write candidate PNGs here")
    parser.add_argument("--max-candidates", type=int, default=40)
    parser.add_argument("--max-bytes", type=int, default=40_000_000,
                        help="skip records larger than this")
    parser.add_argument("--names", type=pathlib.Path,
                        help="name -> record map (see name_records.py --json); only these "
                             "records can be searched, because their seed is trustworthy")
    args = parser.parse_args(argv)

    records = []
    for line in (args.game / "hyprlodr-assets.jsonl").read_text(errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "asset_record":
            records.append(event)
    t0, t1, _ = load_tables(args.game / "hyperdemon.exe")
    key_stream = np.frombuffer(keystream_bytes(t0, t1), dtype=np.uint8)
    pack = (args.game / "asset" / "asset").read_bytes()

    names: dict[int, str] = {}
    if args.names and args.names.exists():
        for name, entry in json.loads(args.names.read_text()).items():
            names[int(entry["offset"])] = name
    candidates = []
    skipped = 0
    for record in records:
        if not 1024 <= record["size"] <= args.max_bytes:
            continue
        cipher = pack[record["offset"]:record["offset"] + record["size"]]
        # A seed may only be taken from a *name* (crc32(name) & 0x3ff). Guessing it from the
        # content works for text payloads only: every binary slice scores the same under the
        # printable-run heuristic, so a guessed seed there is noise and any "no pixels found"
        # conclusion drawn from it is meaningless. Unnamed records are therefore skipped.
        name = names.get(record["offset"])
        if name is None:
            skipped += 1
            continue
        seed = zlib.crc32(name.encode()) & 0x3FF
        plain = decrypt_all(cipher, seed, key_stream)
        seen_pairs = set()
        for width_offset in range(0x08, 0x30):
            for channels in (1, 2, 3, 4):
                width, height = struct.unpack_from("<II", plain, width_offset)
                if not (MIN_SIDE <= width <= MAX_SIDE and MIN_SIDE <= height <= MAX_SIDE):
                    continue
                if (width, height, channels) in seen_pairs:
                    continue
                slack = record["size"] - (width_offset + 8) - width * height * channels
                if -64 <= slack <= 512:
                    seen_pairs.add((width, height, channels))
                    candidates.append({"width": width, "height": height, "channels": channels,
                                       "at": width_offset, "slack": slack, "seed": seed,
                                       "record": record, "plain": plain})
    candidates.sort(key=lambda c: c["width"] * c["height"], reverse=True)
    print(f"{len(candidates)} image-shaped slices across {len(records) - skipped} named records "
          f"({skipped} unnamed records skipped: no trustworthy seed)")
    for entry in candidates[:args.max_candidates]:
        record = entry["record"]
        print("  %5dx%-5d ch=%d at +%#04x slack=%-5d off=%-10d size=%-9d tag=%-5d seed=%#05x"
              % (entry["width"], entry["height"], entry["channels"], entry["at"], entry["slack"],
                 record["offset"], record["size"], record["tag"], entry["seed"]))
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        for entry in candidates[:args.max_candidates]:
            width, height = entry["width"], entry["height"]
            start = entry["at"] + 8
            channels = entry["channels"]
            if channels == 1:
                pixels = entry["plain"][start:start + width * height]
            else:
                frame = np.frombuffer(entry["plain"][start:start + width * height * channels],
                                      dtype=np.uint8).reshape(height, width, channels)
                pixels = (frame[:, :, channels - 1] if channels == 4
                          else frame[:, :, 0]).tobytes()
            path = args.out / ("%09d_%dx%d_c%d.png" % (entry["record"]["offset"], width,
                                                       height, channels))
            write_png(pixels, width, height, path)
        print(f"wrote {min(len(candidates), args.max_candidates)} PNGs to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
