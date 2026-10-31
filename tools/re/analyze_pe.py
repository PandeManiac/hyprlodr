#!/usr/bin/env python3
"""Read-only static analysis of a HYPER DEMON build, producing runtime anchors.

What it does
------------
1. Parses the PE headers (sections, image base, timestamp, size of image).
2. Locates a small set of interesting string constants in the image.
3. Scans the code section for RIP-relative references to those strings, giving
   the instruction addresses that are the natural candidates for a future,
   fingerprinted hook.
4. Emits a compatibility manifest: RVA + length + FNV-1a 64 hash for each anchor.

No game bytes are written to the manifest or the generated header - only hashes -
so the repository never carries proprietary content. The full JSON report is
written wherever you ask (keep it out of version control).

Usage:
    analyze_pe.py <hyperdemon.exe> --json build-re/analysis.json \
                  --header native/win32/game_anchors.h
    analyze_pe.py <hyperdemon.exe> --verify native/win32/game_anchors.h
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF

# Short identifiers only; each becomes a hash anchor, never a copy of game data.
INTERESTING_STRINGS = [
    (b"asset/asset", "resource_path"),
    (b"Can't find asset ", "missing_resource_message"),
    (b"shaders", "shader_directory"),
    (b"skull_giga.vert", "shader_name"),
]

MACHINES = {0x8664: "x86-64", 0x14C: "x86"}


def fnv1a64(data: bytes) -> int:
    value = FNV_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & MASK64
    return value


class PEImage:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError("not a PE image (missing MZ header)")
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("not a PE image (missing PE signature)")

        coff = e_lfanew + 4
        machine, section_count, timestamp = struct.unpack_from("<HHI", self.data, coff)
        optional_size = struct.unpack_from("<H", self.data, coff + 16)[0]
        self.machine = MACHINES.get(machine, f"0x{machine:04x}")
        self.timestamp = timestamp

        optional = coff + 20
        magic = struct.unpack_from("<H", self.data, optional)[0]
        if magic == 0x20B:
            self.image_base = struct.unpack_from("<Q", self.data, optional + 24)[0]
            self.size_of_image = struct.unpack_from("<I", self.data, optional + 56)[0]
        elif magic == 0x10B:
            self.image_base = struct.unpack_from("<I", self.data, optional + 28)[0]
            self.size_of_image = struct.unpack_from("<I", self.data, optional + 56)[0]
        else:
            raise ValueError(f"unsupported optional header magic 0x{magic:04x}")

        self.sections = []
        for index in range(section_count):
            offset = optional + optional_size + index * 40
            name = self.data[offset:offset + 8].rstrip(b"\0").decode("latin-1")
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
                "<IIII", self.data, offset + 8)
            self.sections.append({
                "name": name,
                "virtual_address": virtual_address,
                "virtual_size": max(virtual_size, raw_size),
                "raw_size": raw_size,
                "raw_pointer": raw_pointer,
            })

    def section(self, name: str) -> dict | None:
        for section in self.sections:
            if section["name"] == name:
                return section
        return None

    def rva_to_offset(self, rva: int) -> int | None:
        for section in self.sections:
            start = section["virtual_address"]
            if start <= rva < start + section["virtual_size"]:
                delta = rva - start
                if delta >= section["raw_size"]:
                    return None
                return section["raw_pointer"] + delta
        return None

    def read_rva(self, rva: int, length: int) -> bytes | None:
        offset = self.rva_to_offset(rva)
        if offset is None or offset + length > len(self.data):
            return None
        return self.data[offset:offset + length]

    def find_string(self, needle: bytes) -> list[int]:
        """RVAs of every occurrence of needle inside a data section."""
        found = []
        for section in self.sections:
            if section["name"] not in (".rdata", ".data", "_RDATA"):
                continue
            data = self.data[section["raw_pointer"]:section["raw_pointer"] + section["raw_size"]]
            position = data.find(needle)
            while position >= 0:
                found.append(section["virtual_address"] + position)
                position = data.find(needle, position + 1)
        return found

    def code_xrefs(self, targets: set[int]) -> list[dict]:
        """RIP-relative lea/mov instructions whose target is one of `targets`.

        This is a conservative linear scan: it recognises the canonical
        `REX.W 8D/8B modrm=00 rm=101 disp32` encodings, which compilers use for
        string references. It is not a disassembler, so results are reported as
        candidates and confirmed by hash at runtime.
        """
        text = self.section(".text")
        if text is None:
            return []
        code = self.data[text["raw_pointer"]:text["raw_pointer"] + text["raw_size"]]
        base = text["virtual_address"]
        hits = []
        for index in range(len(code) - 7):
            if code[index] not in (0x48, 0x4C):
                continue
            if code[index + 1] not in (0x8D, 0x8B):
                continue
            modrm = code[index + 2]
            if (modrm & 0xC7) != 0x05:
                continue
            displacement = struct.unpack_from("<i", code, index + 3)[0]
            target = base + index + 7 + displacement
            if target in targets:
                hits.append({
                    "instruction_rva": base + index,
                    "target_rva": target,
                    "length": 7,
                    "bytes": code[index:index + 7].hex(),
                })
        return hits

    def function_start(self, rva: int, look_back: int = 4096) -> int | None:
        """Heuristic: the address after the last padding run before `rva`."""
        offset = self.rva_to_offset(rva)
        text = self.section(".text")
        if offset is None or text is None:
            return None
        start = max(text["raw_pointer"], offset - look_back)
        window = self.data[start:offset]
        best = None
        for index in range(len(window) - 4):
            if window[index] in (0xCC, 0x90) and window[index + 1] == window[index] and \
                    window[index + 2] == window[index] and window[index + 3] == window[index]:
                best = index + 4
        if best is None:
            return None
        # `best` indexes into `window`, which starts at file offset `start`.
        return text["virtual_address"] + (start - text["raw_pointer"]) + best


def build_report(image: PEImage) -> dict:
    anchors = []
    xrefs = []

    string_targets = {}
    for needle, label in INTERESTING_STRINGS:
        occurrences = image.find_string(needle)
        if not occurrences:
            continue
        rva = occurrences[0]
        payload = image.read_rva(rva, len(needle) + 1) or b""
        anchors.append({
            "name": f"string:{label}",
            "rva": rva,
            "length": len(payload),
            "fnv1a64": f"0x{fnv1a64(payload):016x}",
        })
        string_targets[rva] = label

    hits = image.code_xrefs(set(string_targets))
    for hit in hits:
        label = string_targets[hit["target_rva"]]
        payload = image.read_rva(hit["instruction_rva"], 16) or b""
        start = image.function_start(hit["instruction_rva"])
        anchors.append({
            "name": f"xref:{label}",
            "rva": hit["instruction_rva"],
            "length": len(payload),
            "fnv1a64": f"0x{fnv1a64(payload):016x}",
        })
        xrefs.append({
            "label": label,
            "instruction_rva": hit["instruction_rva"],
            "target_rva": hit["target_rva"],
            "bytes": hit["bytes"],
            "candidate_function_start_rva": start,
            "confidence": "observed (encoding scan); function start inferred",
        })

    return {
        "binary": {
            "name": image.path.name,
            "size": len(image.data),
            "sha256": hashlib.sha256(image.data).hexdigest(),
            "machine": image.machine,
            "image_base": f"0x{image.image_base:x}",
            "size_of_image": image.size_of_image,
            "timestamp": image.timestamp,
            "timestamp_hex": f"0x{image.timestamp:08x}",
        },
        "sections": image.sections,
        "anchors": anchors,
        "xrefs": xrefs,
        "notes": [
            "String locations and reference encodings are observed from the file.",
            "Function boundaries are inferred from padding runs, not from symbols.",
            "Anchors are hashes only; no game bytes are stored in the manifest.",
        ],
    }


def render_header(report: dict) -> str:
    binary = report["binary"]
    lines = [
        "/* Generated by tools/re/analyze_pe.py - do not edit by hand.",
        " *",
        f" * Build identity: {binary['name']} sha256={binary['sha256']}",
        f" * timestamp={binary['timestamp_hex']} size_of_image=0x{binary['size_of_image']:x}",
        " *",
        " * Anchors are FNV-1a 64 hashes of bytes at fixed RVAs, so this file",
        " * carries no proprietary content. The proxy verifies them at runtime and",
        " * reports mismatches instead of assuming a compatible build.",
        " */",
        "#ifndef HYPRLODR_GAME_ANCHORS_H",
        "#define HYPRLODR_GAME_ANCHORS_H",
        "",
        "#include \"hyprlodr/anchors.h\"",
        "",
        f"#define HYPRLODR_GAME_BUILD_TIMESTAMP 0x{binary['timestamp']:08x}u",
        f"#define HYPRLODR_GAME_SIZE_OF_IMAGE 0x{binary['size_of_image']:x}u",
        "",
        "/* Stream-cipher tables read by the asset loader (see docs/RESEARCH.md).",
        " * The proxy copies them out of the running image so the live bytes can be",
        " * compared with the file's .rdata copy. */",
        "#define HYPRLODR_GAME_TABLE_T0_RVA 0x361710u",
        "#define HYPRLODR_GAME_TABLE_T1_RVA 0x361810u",
        "#define HYPRLODR_GAME_TABLE_BYTES 256u",
        "",
        "static const hyprlodr_anchor hyprlodr_game_anchors[] = {",
    ]
    for anchor in report["anchors"]:
        lines.append(
            f"    {{\"{anchor['name']}\", 0x{anchor['rva']:08x}u, {anchor['length']}u, "
            f"{anchor['fnv1a64']}ull}},")
    lines += [
        "};",
        "",
        "#define HYPRLODR_GAME_ANCHOR_COUNT \\",
        "    (sizeof hyprlodr_game_anchors / sizeof hyprlodr_game_anchors[0])",
        "",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def verify(image: PEImage, header: Path) -> int:
    text = header.read_text()
    pattern = re.compile(
        r"\{\"([^\"]+)\", 0x([0-9a-fA-F]+)u, (\d+)u, 0x([0-9a-fA-F]+)ull\}")
    entries = pattern.findall(text)
    if not entries:
        print("no anchors found in header")
        return 1
    failures = 0
    for name, rva, length, expected in entries:
        payload = image.read_rva(int(rva, 16), int(length))
        actual = fnv1a64(payload) if payload is not None else None
        ok = actual == int(expected, 16)
        print(f"{'PASS' if ok else 'FAIL'} {name} rva=0x{int(rva, 16):x}")
        failures += 0 if ok else 1
    print(f"\n{len(entries) - failures}/{len(entries)} anchors verified against "
          f"{image.path.name}")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Anchor analysis for HYPER DEMON builds")
    parser.add_argument("executable", type=Path)
    parser.add_argument("--json", type=Path, help="write the full report here")
    parser.add_argument("--header", type=Path, help="write the C anchor header here")
    parser.add_argument("--verify", type=Path, help="verify an existing header against the binary")
    args = parser.parse_args(argv)

    image = PEImage(args.executable)
    if args.verify:
        return verify(image, args.verify)

    report = build_report(image)
    print(f"binary      : {report['binary']['name']} ({report['binary']['machine']})")
    print(f"sha256      : {report['binary']['sha256']}")
    print(f"timestamp   : {report['binary']['timestamp_hex']}")
    for anchor in report["anchors"]:
        print(f"anchor      : {anchor['name']:<34} rva=0x{anchor['rva']:06x} "
              f"len={anchor['length']:<3} {anchor['fnv1a64']}")
    for xref in report["xrefs"]:
        start = xref["candidate_function_start_rva"]
        print(f"xref        : {xref['label']:<28} at 0x{xref['instruction_rva']:06x} "
              f"-> 0x{xref['target_rva']:06x} "
              f"(candidate function 0x{start:06x})" if start else "no candidate")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2))
        print(f"report      : {args.json}")
    if args.header:
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text(render_header(report))
        print(f"header      : {args.header}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
