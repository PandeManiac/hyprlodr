#!/usr/bin/env python3
"""Decrypt HYPER DEMON's ``asset/asset`` slices.

Reverse-engineered from the game's slice reader (see ``docs/RE_PACK.md``):

* a record is ``(offset u32, size u32, tag u32)`` into the pack;
* the payload is read at ``offset`` for ``size`` bytes;
* ``FUN_140215280`` computes the seed over the *entry name* and the loader checks the
  payload against the same function, so the seed is a plain CRC-32 of the name
  (the binary's table at RVA ``0x36f700`` is the standard reflected polynomial);
* ``FUN_140213370`` then un-XORs the bytes with a rolling 16-bit state:

      plain[i] = T0[((T1[(s >> 2) & 0xff] >> 1) + s) & 0xff] ^ T1[s & 0xff] ^ cipher[i]
      s        = (s + 1 + sbyte(cipher[i])) & 0xffff

The two 256-byte tables are read from the game executable at run time, so no game
data lives in this repository.

Usage::

    python3 tools/re/pack_crypt.py --exe /path/hyperdemon.exe --pack /path/asset/asset \
        --name entity_lobby_dagger_hyper --offset 0 --size 256 -o /tmp/slice.bin
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

T0_RVA = 0x361710
T1_RVA = 0x361810
CRC_RVA = 0x36F700


def rva_to_offset(data: bytes, rva: int) -> int:
    """Map an RVA to a file offset using the PE section table."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e_lfanew + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    for index in range(section_count):
        offset = optional + optional_size + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, offset + 8)
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return raw_pointer + (rva - virtual_address)
    raise ValueError(f"RVA 0x{rva:x} is not inside any section")


def load_tables(exe_path: pathlib.Path) -> tuple[bytes, bytes, bytes]:
    data = exe_path.read_bytes()
    t0 = data[rva_to_offset(data, T0_RVA):][:256]
    t1 = data[rva_to_offset(data, T1_RVA):][:256]
    crc = data[rva_to_offset(data, CRC_RVA):][:1024]
    if len(t0) != 256 or len(t1) != 256 or len(crc) != 1024:
        raise ValueError("could not read the cipher tables from the executable")
    return t0, t1, crc


def seed_for_name(name: str | bytes) -> int:
    """CRC-32 of the entry name, matching the game's ``FUN_140215280``."""
    raw = name.encode() if isinstance(name, str) else name
    return zlib.crc32(raw) & 0xFFFFFFFF


def decrypt(data: bytes, t0: bytes, t1: bytes, seed: int) -> bytes:
    state = seed & 0xFFFF
    out = bytearray(len(data))
    for index, cipher in enumerate(data):
        mixed = ((t1[(state >> 2) & 0xFF] >> 1) + state) & 0xFF
        out[index] = t0[mixed] ^ t1[state & 0xFF] ^ cipher
        signed = cipher - 256 if cipher >= 128 else cipher
        state = (state + 1 + signed) & 0xFFFF
    return bytes(out)


def encrypt(data: bytes, t0: bytes, t1: bytes, seed: int) -> bytes:
    """Inverse of :func:`decrypt`.

    The keystream byte depends only on the *ciphertext* byte that advanced the state, so
    encryption is done forward: the stream is known, the ciphertext is produced, and the
    state advances on the byte just produced.
    """
    state = seed & 0xFFFF
    out = bytearray(len(data))
    for index, plain in enumerate(data):
        mixed = ((t1[(state >> 2) & 0xFF] >> 1) + state) & 0xFF
        cipher = plain ^ t0[mixed] ^ t1[state & 0xFF]
        out[index] = cipher
        signed = cipher - 256 if cipher >= 128 else cipher
        state = (state + 1 + signed) & 0xFFFF
    return bytes(out)


def decrypt_slice(pack: pathlib.Path, offset: int, size: int, name: str,
                  exe_path: pathlib.Path) -> bytes:
    t0, t1, _ = load_tables(exe_path)
    with pack.open("rb") as handle:
        handle.seek(offset)
        payload = handle.read(size)
    if len(payload) != size:
        raise ValueError(f"short read: wanted {size} bytes at {offset:#x}, got {len(payload)}")
    return decrypt(payload, t0, t1, seed_for_name(name))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", required=True, type=pathlib.Path,
                        help="path to hyperdemon.exe (source of the cipher tables)")
    parser.add_argument("--pack", required=True, type=pathlib.Path,
                        help="path to asset/asset")
    parser.add_argument("--name", required=True,
                        help="entry name, used as the CRC-32 seed")
    parser.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    parser.add_argument("--size", type=lambda s: int(s, 0), default=64)
    parser.add_argument("-o", "--out", type=pathlib.Path,
                        help="write the plaintext here instead of stdout")
    parser.add_argument("--encrypt", action="store_true",
                        help="treat --name's seed as a key and encrypt stdin/file to --out")
    parser.add_argument("--in", dest="source", type=pathlib.Path,
                        help="with --encrypt, plaintext to encrypt (default: stdin)")
    parser.add_argument("--hex", action="store_true", help="print a hex dump")
    args = parser.parse_args(argv)

    if args.encrypt:
        t0, t1, _ = load_tables(args.exe)
        payload = args.source.read_bytes() if args.source else sys.stdin.buffer.read()
        cipher = encrypt(payload, t0, t1, seed_for_name(args.name))
        if not args.out:
            parser.error("--encrypt needs -o/--out")
        args.out.write_bytes(cipher)
        print(f"wrote {len(cipher)} encrypted bytes to {args.out}", file=sys.stderr)
        return 0

    plain = decrypt_slice(args.pack, args.offset, args.size, args.name, args.exe)
    if args.out:
        args.out.write_bytes(plain)
        print(f"wrote {len(plain)} bytes to {args.out}", file=sys.stderr)
    if args.hex or not args.out:
        for start in range(0, len(plain), 16):
            chunk = plain[start:start + 16]
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  {args.offset + start:08x}  {chunk.hex(' '):<47}  {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
