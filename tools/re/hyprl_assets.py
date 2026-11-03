#!/usr/bin/env python3
"""Index and extract records captured from HYPER DEMON's encrypted asset pack.

The live probe supplies structural records; this tool preserves uncertain names as
unknown and uses record offsets as stable IDs. It never writes into the game install.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pack_crypt import decrypt, load_tables, seed_for_name

CHUNK = 1024 * 1024
TAGS = {1: "subobject-graph", 2: "compact-struct", 4: "string-int", 8: "unknown-8",
        0x20: "text", 0x100: "audio", 0x1000: "truetype-font"}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def hash_range(stream, offset: int, size: int) -> str:
    digest = hashlib.sha256()
    stream.seek(offset)
    remaining = size
    while remaining:
        block = stream.read(min(CHUNK, remaining))
        if not block:
            raise ValueError("short read while hashing record")
        digest.update(block)
        remaining -= len(block)
    return digest.hexdigest()


def load_lines(path: pathlib.Path):
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        try:
            yield number, json.loads(line)
        except json.JSONDecodeError:
            continue


def read_names(path: pathlib.Path | None):
    if path is None:
        return {}
    raw = json.loads(path.read_text())
    # Existing name_records.py emits {name: {offset,size,tag,...}}. Keep only
    # unique, bounds-checkable rows; these are labeled inferred, never verified.
    names = {}
    for name, row in raw.items():
        key = (int(row["offset"]), int(row["size"]), int(row["tag"]))
        names.setdefault(key, []).append(name)
    return {key: values for key, values in names.items()}


def build_index(pack: pathlib.Path, exe: pathlib.Path, evidence: pathlib.Path,
                names_path: pathlib.Path | None):
    pack_size = pack.stat().st_size
    records = {}
    for line, event in load_lines(evidence):
        if event.get("event") != "asset_record":
            continue
        try:
            offset, size, tag = (int(event[k]) for k in ("offset", "size", "tag"))
        except (KeyError, TypeError, ValueError):
            continue
        # Python integers do not overflow, but use subtraction form to mirror
        # safe fixed-width validation and reject zero-length/out-of-file rows.
        if offset < 0 or size <= 0 or offset > pack_size or size > pack_size - offset:
            continue
        key = (offset, size, tag)
        records.setdefault(key, {"offset": offset, "size": size, "tag": tag,
                                  "evidence_lines": []})["evidence_lines"].append(line)

    name_map = read_names(names_path)
    cipher_tables = load_tables(exe)[:2] if name_map else None
    ordered = []
    with pack.open("rb") as stream:
        for (offset, size, tag), row in sorted(records.items()):
            stream.seek(offset)
            sample = stream.read(min(size, 4096))
            names = name_map.get((offset, size, tag), [])
            unique_names = sorted(set(names))
            name = unique_names[0] if len(unique_names) == 1 else None
            content_format = None
            payload_header = None
            if name and cipher_tables:
                t0, t1 = cipher_tables
                plain_sample = decrypt(sample, t0, t1, seed_for_name(name))
                payload_header = plain_sample[:24].hex()
                if tag == 0x1000 and plain_sample[0x18:0x1c] in (b"\x00\x01\x00\x00", b"OTTO"):
                    content_format = "sfnt/TrueType"
            row.update({
                "id": f"rec-{offset:09x}-{size:08x}-{tag:04x}",
                "name": name,
                "name_candidates": unique_names if len(unique_names) > 1 else [],
                "association": "inferred" if name else ("ambiguous" if unique_names else "unknown"),
                "seed": f"0x{seed_for_name(name) & 0x3ff:03x}" if name else None,
                "key_class": f"0x{seed_for_name(name) & 0x3ff:03x}" if name else None,
                "tag_name": TAGS.get(tag, "unsupported"),
                "content_format": content_format,
                "payload_header_hex": payload_header,
                "encrypted_sha256_sample": hashlib.sha256(sample).hexdigest(),
                "encrypted_sha256": hash_range(stream, offset, size),
            })
            ordered.append(row)
    return {
        "schema": 1,
        "source": {"pack_size": pack_size, "pack_sha256": sha256(pack),
                   "exe_size": exe.stat().st_size, "exe_sha256": sha256(exe)},
        "evidence": {"path": str(evidence), "sha256": sha256(evidence),
                     "unique_records": len(ordered),
                     "names_sha256": sha256(names_path) if names_path else None},
        "records": ordered,
    }


def cache_matches(index, pack, exe, evidence, names_path=None):
    source = index.get("source", {})
    ev = index.get("evidence", {})
    return (source.get("pack_sha256") == sha256(pack)
            and source.get("exe_sha256") == sha256(exe)
            and ev.get("sha256") == sha256(evidence)
            and ev.get("names_sha256") == (sha256(names_path) if names_path else None))


def load_index(path, pack=None, exe=None, evidence=None, names_path=None):
    index = json.loads(path.read_text())
    if pack and exe and evidence and not cache_matches(index, pack, exe, evidence, names_path):
        raise ValueError("index cache does not match the current archive, executable, or evidence; run scan")
    return index


def pick(index, query):
    exact = [r for r in index["records"] if r["id"] == query or r.get("name") == query]
    if not exact and query.isdecimal():
        exact = [r for r in index["records"] if r["offset"] == int(query)]
    if len(exact) != 1:
        raise ValueError(f"expected one record for {query!r}, found {len(exact)}; use record ID or offset")
    return exact[0]


def extract(record, pack, exe, output, name=None, key_class=None):
    offset, size = record["offset"], record["size"]
    if offset < 0 or size <= 0 or size > pack.stat().st_size - offset:
        raise ValueError("record bounds no longer fit the archive")
    if name is None and key_class is None:
        raise ValueError("decryption needs --name or an explicit --key-class; unknown records remain encrypted")
    if output.resolve() == pack.resolve() or (output.exists() and os.path.samefile(output, pack)):
        raise ValueError("output path aliases the source archive; refusing to modify the installation")
    seed = (seed_for_name(name) if name is not None else int(key_class, 0)) & 0x3ff
    t0, t1, _ = load_tables(exe)
    with pack.open("rb") as source:
        source.seek(offset)
        cipher = source.read(size)
    if len(cipher) != size:
        raise ValueError("short read while extracting record")
    plain = decrypt(cipher, t0, t1, seed)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(plain)
    return seed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("scan", "list", "search", "info", "extract", "verify"))
    parser.add_argument("query", nargs="?")
    parser.add_argument("--pack", type=pathlib.Path, required=True)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True,
                        help="JSONL emitted by the in-game asset probe")
    parser.add_argument("--index", type=pathlib.Path, default=pathlib.Path("hyprlodr-assets-index.json"))
    parser.add_argument("--names", type=pathlib.Path, help="optional JSON map from name_records.py")
    parser.add_argument("--out", type=pathlib.Path, help="extraction destination")
    parser.add_argument("--name", help="logical name used to derive the CRC-32 key")
    parser.add_argument("--key-class", help="explicit 10-bit key class (e.g. 0x3f2)")
    args = parser.parse_args(argv)
    try:
        if args.command == "scan":
            index = build_index(args.pack, args.exe, args.evidence, args.names)
            args.index.parent.mkdir(parents=True, exist_ok=True)
            temp = args.index.with_name(args.index.name + ".tmp")
            temp.write_text(json.dumps(index, indent=2) + "\n")
            os.replace(temp, args.index)
            print(f"indexed {len(index['records'])} unique records; cache {args.index}")
            return 0
        index = load_index(args.index, args.pack, args.exe, args.evidence, args.names)
        if args.command == "verify":
            bad = [r["id"] for r in index["records"] if r["offset"] < 0 or r["size"] <= 0
                   or r["size"] > index["source"]["pack_size"] - r["offset"]]
            print(f"records={len(index['records'])} invalid={len(bad)} source_hashes=match")
            return 1 if bad else 0
        if args.command == "list":
            rows = index["records"]
        elif args.command == "search":
            if not args.query:
                parser.error("search requires a query")
            rows = [r for r in index["records"] if args.query.lower() in
                    (r.get("name") or "").lower() or args.query.lower() in r["id"].lower()]
        else:
            if not args.query:
                parser.error(f"{args.command} requires a record name, ID, or offset")
            row = pick(index, args.query)
            rows = [row]
        if args.command == "extract":
            if not args.out:
                parser.error("extract requires --out")
            if args.name and rows[0].get("name") and args.name != rows[0]["name"]:
                raise ValueError("--name conflicts with the indexed name association")
            seed = extract(rows[0], args.pack, args.exe, args.out,
                           args.name or rows[0].get("name"), args.key_class)
            print(f"wrote {args.out} ({rows[0]['size']} bytes), key-class=0x{seed:03x}")
        elif args.command == "info":
            print(json.dumps(rows[0], indent=2))
        else:
            for row in rows:
                print(f"{row['id']}\t{row.get('name') or '<unknown>'}\ttag={row['tag']:#x}"
                      f"\toffset={row['offset']}\tsize={row['size']}\t{row['association']}")
            print(f"records={len(rows)}")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"hyprl-assets: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
