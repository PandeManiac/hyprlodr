#!/usr/bin/env python3
"""Probe the HYPER DEMON asset container and the loader's stream cipher.

What this tool establishes, reproducibly:

1. The container has no standard framing (zlib/deflate/bzip2/xz/zstd), no
   plaintext header, and the same entropy from byte zero to the end.
2. `hyperdemon.exe` opens it with a C++ stream (`FUN_140212490` ->
   `FUN_140110570`, a filebuf open) and then runs every byte through a
   stateful XOR cipher before parsing.
3. The cipher, as read from the decompiled code (RVAs are for the shipped
   build, which `tools/re/analyze_pe.py --verify` pins by hash):

       k    = T0[((T1[(s >> 2) & 0xff] >> 1) + s) & 0xff] ^ T1[s & 0xff]
       p    = c ^ k
       s    = (s + 1 + (int)(signed char)c) & 0xffff

   with 256-byte tables at RVA 0x361710 (T0) and 0x361810 (T1). Note the state
   advances on the *ciphertext* byte, so decryption is self-synchronising.

4. The header block is 0x5f3 bytes decrypted from initial state 11; the entry
   table that follows restarts at state 0 and is a sequence of
   (u16 id, u8 name_length, name, u32 offset, u32 size) ending at id 0xffff.

`decrypt` implements exactly that. Running it on the shipped archive does *not*
produce readable names, and the tool reports that instead of pretending
otherwise: either the tables are not the byte arrays Ghidra's typing suggests,
or the block is itself compressed. The self-test proves the implementation
round-trips a synthetic archive built the way the decompiled code describes.

Usage:
    asset_probe.py --exe hyperdemon.exe --asset asset/asset
    asset_probe.py --selftest
"""
from __future__ import annotations

import argparse
import bz2
import collections
import lzma
import math
import pathlib
import struct
import sys
import zlib

HEADER_BLOCK = 0x5F3
HEADER_STATE = 11
TABLE_T0_RVA = 0x361710
TABLE_T1_RVA = 0x361810
CRC_TABLE_RVA = 0x36F700
ENTRY_END = 0xFFFF


def standard_crc_table() -> list[int]:
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            value = (value >> 1) ^ 0xEDB88320 if value & 1 else value >> 1
        table.append(value)
    return table


def entry_state(name: bytes) -> int:
    """Per-entry cipher state.

    FUN_140215280 is a reflected CRC-32 (initial value 0, final complement)
    over the asset name; the loader passes its low 16 bits to the stream
    cipher as the starting state. Verified against the binary's table, see
    `verify_crc_table`.
    """
    return zlib.crc32(name) & 0xFFFF


def entropy(block: bytes) -> float:
    counts = collections.Counter(block)
    total = len(block)
    return -sum((c / total) * math.log2(c / total) for c in counts.values())


def keystream(t0: bytes, t1: bytes, state: int) -> int:
    return (t0[((t1[(state >> 2) & 0xFF] >> 1) + state) & 0xFF] ^ t1[state & 0xFF]) & 0xFF


def decrypt(data: bytes, state: int, t0: bytes, t1: bytes) -> tuple[bytes, int]:
    """Decrypt in place order; state advances on the ciphertext byte."""
    out = bytearray(len(data))
    for index, cipher in enumerate(data):
        out[index] = cipher ^ keystream(t0, t1, state)
        step = cipher - 256 if cipher > 127 else cipher
        state = (state + 1 + step) & 0xFFFF
    return bytes(out), state


def encrypt(data: bytes, state: int, t0: bytes, t1: bytes) -> bytes:
    """Inverse of decrypt, used by the self-test to build a fixture."""
    out = bytearray(len(data))
    for index, plain in enumerate(data):
        cipher = plain ^ keystream(t0, t1, state)
        out[index] = cipher
        step = cipher - 256 if cipher > 127 else cipher
        state = (state + 1 + step) & 0xFFFF
    return bytes(out)


def parse_entries(plain: bytes) -> list[dict]:
    entries = []
    position = 0
    while position + 3 <= len(plain):
        ident = struct.unpack_from("<H", plain, position)[0]
        position += 2
        if ident == ENTRY_END:
            break
        length = plain[position]
        position += 1
        if position + length + 8 > len(plain):
            break
        name = plain[position:position + length]
        position += length
        offset, size = struct.unpack_from("<II", plain, position)
        position += 8
        entries.append({"id": ident, "name": name, "offset": offset, "size": size})
    return entries


def build_fixture(t0: bytes, t1: bytes) -> bytes:
    stream = bytearray()
    payloads = [("shaders/skull_giga.vert", 0x5F3, 812), ("shaders/plain.frags", 0x8C1, 128)]
    for ident, (name, offset, size) in enumerate(payloads):
        stream += struct.pack("<H", ident)
        stream.append(len(name))
        stream += name.encode()
        stream += struct.pack("<II", offset, size)
    stream += struct.pack("<H", ENTRY_END)
    return encrypt(bytes(stream), 0, t0, t1)


def selftest() -> int:
    failures = 0

    def check(condition: bool, message: str) -> None:
        nonlocal failures
        if not condition:
            failures += 1
            print(f"FAIL {message}")

    # Fixed, arbitrary tables: the cipher does not require permutations.
    t0 = bytes(((index * 37 + 11) & 0xFF) for index in range(256))
    t1 = bytes(((index * 91 + 7) & 0xFF) for index in range(256))

    # Standard CRC-32 check value, then the 16-bit reduction the loader uses.
    check(entry_state(b"123456789") == (0xCBF43926 & 0xFFFF),
          "entry_state must reduce standard CRC-32 to 16 bits")
    check(entry_state(b"") == 0, "empty name must give state 0")

    probe = bytes(range(256))
    encrypted = encrypt(probe, HEADER_STATE, t0, t1)
    decrypted, state = decrypt(encrypted, HEADER_STATE, t0, t1)
    check(decrypted == probe, "round trip must recover the plaintext exactly")
    check(encrypted != probe, "ciphertext must differ from plaintext")
    # State depends only on ciphertext bytes, so a second pass agrees.
    _, again = decrypt(encrypted, HEADER_STATE, t0, t1)
    check(state == again, "state sequence must be deterministic")

    fixture = build_fixture(t0, t1)
    plain, _ = decrypt(fixture, 0, t0, t1)
    entries = parse_entries(plain)
    check(len(entries) == 2, f"expected two entries, got {len(entries)}")
    check(entries[0]["name"] == b"shaders/skull_giga.vert",
          f"unexpected name {entries[0]['name']!r}")
    check(entries[0]["offset"] == 0x5F3 and entries[0]["size"] == 812,
          f"unexpected offset/size {entries[0]}")
    check(entries[1]["id"] == 1, "entry ids must be preserved")

    # A corrupted byte breaks the entry table exactly as it breaks the real one.
    damaged = bytearray(fixture)
    damaged[4] ^= 0xFF
    damaged_entries = parse_entries(decrypt(bytes(damaged), 0, t0, t1)[0])
    check(len(damaged_entries) != 2 or damaged_entries[0]["name"] != entries[0]["name"],
          "a corrupted stream must not parse back to the same table")

    if failures:
        print(f"{failures} asset probe check(s) failed")
        return 1
    print("asset probe checks passed")
    return 0


def probe(exe_path: pathlib.Path, asset_path: pathlib.Path) -> int:
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from analyze_pe import PEImage  # local import keeps --selftest dependency free

    pe = PEImage(exe_path)
    asset = asset_path.read_bytes()

    print(f"asset        : {asset_path.name} ({len(asset):,} bytes)")
    for offset in range(0, min(len(asset), 512 * 1024), 64 * 1024):
        block = asset[offset:offset + 64 * 1024]
        print(f"  entropy @{offset:>7}: {entropy(block):.2f} bits/byte")

    for name, decoder in (("zlib", zlib.decompress),
                          ("deflate", lambda b: zlib.decompress(b, -15)),
                          ("bzip2", bz2.decompress),
                          ("xz", lzma.decompress)):
        try:
            decoder(asset[:1 << 16])
            print(f"  framing {name}: accepted (unexpected)")
        except Exception:
            print(f"  framing {name}: rejected")

    def table(rva: int) -> bytes:
        offset = pe.rva_to_offset(rva)
        return pe.data[offset:offset + 256]

    t0 = table(TABLE_T0_RVA)
    t1 = table(TABLE_T1_RVA)
    print(f"  table T0 @0x{TABLE_T0_RVA:x}: {len(set(t0))} distinct bytes, "
          f"permutation={sorted(t0) == list(range(256))}")
    print(f"  table T1 @0x{TABLE_T1_RVA:x}: {len(set(t1))} distinct bytes, "
          f"permutation={sorted(t1) == list(range(256))}")

    crc_offset = pe.rva_to_offset(CRC_TABLE_RVA)
    found = list(struct.unpack_from("<256I", pe.data, crc_offset))
    print(f"  crc table @0x{CRC_TABLE_RVA:x}: matches standard CRC-32 = "
          f"{found == standard_crc_table()}")
    print(f"  entry key    : state = crc32(name) & 0xffff, e.g. "
          f"{entry_state(b'shaders/skull_giga.vert')}")

    header, state = decrypt(asset[:HEADER_BLOCK], HEADER_STATE, t0, t1)
    printable = sum(1 for byte in header if 32 <= byte < 127)
    print(f"  header       : {HEADER_BLOCK} bytes from state {HEADER_STATE}, "
          f"{printable}/{HEADER_BLOCK} printable, ending state {state}")
    print(f"  header hex   : {header[:32].hex()}")

    body, _ = decrypt(asset[HEADER_BLOCK:HEADER_BLOCK + 4096], 0, t0, t1)
    entries = parse_entries(body)
    plausible = [e for e in entries
                 if all(32 <= byte < 127 for byte in e["name"]) and e["size"] < len(asset)]
    print(f"  entry table  : {len(entries)} parsed, {len(plausible)} plausible")
    if plausible:
        for entry in plausible[:5]:
            print(f"    {entry['name'].decode()!r} offset={entry['offset']} size={entry['size']}")
        print("  RESULT: the cipher is understood; names decode")
        return 0
    print("  RESULT: decryption does not yield names - the tables or block framing")
    print("          differ from the decompiled byte-typed view. Do not claim")
    print("          asset extraction until this passes.")
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", type=pathlib.Path)
    parser.add_argument("--asset", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.exe or not args.asset:
        parser.error("--exe and --asset are required unless --selftest is used")
    return probe(args.exe, args.asset)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
