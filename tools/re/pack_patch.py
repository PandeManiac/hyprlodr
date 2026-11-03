#!/usr/bin/env python3
"""Rewrite one encrypted slice of HYPER DEMON's ``asset/asset`` in place.

This is the delivery path for a title replacement. A record's ``(offset, size)`` is fixed by
the record table the game was compiled with, so a replacement payload has to be **exactly the
same length** as the slice it replaces; everything else in the pack is copied verbatim, which
keeps the untouched slices byte-identical instead of re-encrypting 307 MB of data.

The seed is ``zlib.crc32(name) & 0x3ff`` (confirmed against the game's fonts and materials), so
a slice is patched by *name*, and the result is written to a new file — the installed pack is
never modified.

Usage::

    python3 tools/re/pack_patch.py --exe "$GAME/hyperdemon.exe" --pack "$GAME/asset/asset" \\
        --out .tmp/asset.patched --name georgia --offset 303451631 --size 219736 \\
        --payload new_georgia.bin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from pack_crypt import decrypt, encrypt, load_tables, seed_for_name

CHUNK = 1 << 20


def patch_pack(source: pathlib.Path, target: pathlib.Path,
               edits: list[tuple[int, bytes]], expected_size: int | None = None):
    """Copy ``source`` to ``target``, replacing each ``(offset, ciphertext)`` edit.

    The caller supplies already-encrypted bytes: the edit has to occupy exactly as many bytes
    as the slice it replaces, because every other record's offset is baked into the table.
    """
    source = pathlib.Path(source)
    target = pathlib.Path(target)
    if not source.is_file():
        raise ValueError(f"source archive is not a regular file: {source}")
    if source.resolve() == target.resolve() or (target.exists() and os.path.samefile(source, target)):
        raise ValueError("output archive must not alias the source archive")
    source_size = source.stat().st_size
    if expected_size is not None and expected_size != source_size:
        raise ValueError(f"expected archive size {expected_size} does not match source size {source_size}")
    edits = sorted(edits)
    for offset, cipher in edits:
        if offset < 0 or not isinstance(cipher, bytes) or not cipher:
            raise ValueError("each edit must have a non-negative offset and non-empty bytes")
        if offset > source_size or len(cipher) > source_size - offset:
            raise ValueError(f"edit at {offset} with {len(cipher)} bytes exceeds source archive bounds")
    for (a, a_cipher), (b, _) in zip(edits, edits[1:]):
        if a + len(a_cipher) > b:
            raise ValueError(f"edits at {a} and {b} overlap")
    written = 0
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, target.open("wb") as dst:
        position = 0
        for offset, cipher in edits:
            while position < offset:
                chunk = src.read(min(CHUNK, offset - position))
                if not chunk:
                    raise ValueError("source ended before an edit")
                dst.write(chunk)
                position += len(chunk)
                written += len(chunk)
            dst.write(cipher)
            written += len(cipher)
            position += len(cipher)
            src.seek(position)
        while True:
            chunk = src.read(CHUNK)
            if not chunk:
                break
            dst.write(chunk)
            written += len(chunk)
    if expected_size is not None and written != expected_size:
        raise ValueError(f"wrote {written} bytes, expected {expected_size}")
    return target


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    parser.add_argument("--pack", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--name", required=True, help="entry name whose CRC-32 keys the slice")
    parser.add_argument("--offset", required=True, type=lambda s: int(s, 0))
    parser.add_argument("--size", required=True, type=lambda s: int(s, 0))
    parser.add_argument("--payload", required=True, type=pathlib.Path,
                        help="replacement plaintext, exactly --size bytes")
    args = parser.parse_args(argv)

    t0, t1, _ = load_tables(args.exe)
    plaintext = args.payload.read_bytes()
    if len(plaintext) != args.size:
        parser.error(f"replacement payload is {len(plaintext)} bytes; record size is {args.size}")
    if args.out.resolve() == args.pack.resolve() or (args.out.exists() and
                                                       os.path.samefile(args.out, args.pack)):
        parser.error("--out must not alias the installed/source archive")
    seed = seed_for_name(args.name) & 0x3FF
    cipher = encrypt(plaintext, t0, t1, seed)

    with args.pack.open("rb") as handle:
        handle.seek(args.offset)
        shipped = handle.read(args.size)
    if len(shipped) != args.size:
        parser.error("the pack ended before that slice")
    if decrypt(shipped, t0, t1, seed) != plaintext:
        print("note: the replacement differs from what the pack holds (that is the point)",
              file=sys.stderr)

    patch_pack(args.pack, args.out, [(args.offset, cipher)],
               expected_size=args.pack.stat().st_size)
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes); "
          f"slice {args.name!r} at {args.offset} keyed with seed {seed:#05x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
