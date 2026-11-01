#!/usr/bin/env python3
"""Name the live asset records of a running HYPER DEMON.

The probe writes ``hyprlodr-assets.jsonl`` with two kinds of evidence:

* ``asset_record`` — ``(offset, size, tag)`` slots of the 0x58-byte record tables;
* ``key`` — the entries of the resolver's parallel ``std::string`` arrays.

Names are *not* stored next to the records, but the seed of a slice is
``zlib.crc32(name) & 0x3ff`` (confirmed on the game's eleven fonts), and the seed is
recoverable from the payload alone whenever the payload has readable content. So a record is
named by cracking it and looking the resulting key class up in the CRC map — never by pairing
a name array to a record array positionally, which is off by a shift in at least one array.

Only ``(offset, size, tag)`` triples and the recovered names are reported; payload bytes stay
in memory and are never written to disk by this tool.

Usage::

    python3 tools/re/name_records.py --game "$GAME" [--json out.json] [--text DIR]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from pack_crypt import (decrypt, keystream_bytes, crack_matrix, load_tables,
                        text_scores)

RUNS = re.compile(rb"[\x20-\x7e]{4,}")


def load_evidence(path: pathlib.Path):
    recs, keys = [], []
    for line in path.read_text(errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "asset_record":
            recs.append(event)
        elif event.get("event") == "key":
            keys.append(event)
    return recs, keys


def group_runs(recs):
    """Group records whose slices are contiguous in the pack (the resolver's arrays)."""
    runs = []
    for record in recs:
        if runs and record["offset"] == runs[-1][-1]["offset"] + runs[-1][-1]["size"]:
            runs[-1].append(record)
        else:
            runs.append([record])
    return runs


def strings(plain: bytes) -> list[str]:
    return [chunk.decode() for chunk in RUNS.findall(plain)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", required=True, type=pathlib.Path,
                        help="HYPER DEMON installation directory")
    parser.add_argument("--evidence", type=pathlib.Path,
                        help="defaults to <game>/hyprlodr-assets.jsonl")
    parser.add_argument("--limit", type=int, default=4096,
                        help="bytes per record used for the seed search (default 4096)")
    parser.add_argument("--full", action="store_true",
                        help="decrypt each named slice in full instead of the searched prefix")
    parser.add_argument("--json", type=pathlib.Path,
                        help="write the recovered name -> record map here")
    parser.add_argument("--text", type=pathlib.Path,
                        help="also write each named payload's readable strings here")
    parser.add_argument("--grep", action="append", default=[],
                        help="report records whose strings match this regex")
    parser.add_argument("--dump-all", type=pathlib.Path,
                        help="write every record's readable strings here, named by offset, plus "
                             "manifest.jsonl of (offset, size, tag, seed, score)")
    args = parser.parse_args(argv)

    evidence = args.evidence or args.game / "hyprlodr-assets.jsonl"
    recs, keys = load_evidence(evidence)
    runs = group_runs(recs)
    t0, t1, _ = load_tables(args.game / "hyperdemon.exe")
    ks = keystream_bytes(t0, t1)
    pack = (args.game / "asset" / "asset").read_bytes()

    # key class -> candidate names (CRC-32 collides in 10 bits, so keep every candidate)
    by_class: dict[int, list[str]] = {}
    for event in keys:
        by_class.setdefault(zlib.crc32(event["name"].encode()) & 0x3FF, []).append(event["name"])

    print(f"records {len(recs)} in {len(runs)} contiguous runs; "
          f"{len(keys)} names over {len({e['addr'] for e in keys})} arrays")

    named: dict[str, dict] = {}
    unmatched = 0
    dump_manifest = None
    if args.dump_all is not None:
        args.dump_all.mkdir(parents=True, exist_ok=True)
        dump_manifest = (args.dump_all / "manifest.jsonl").open("w")
    for record in recs:
        chunk = pack[record["offset"]:record["offset"] + min(record["size"], args.limit)]
        matrix = crack_matrix(chunk, t0, t1, ks)
        scores = text_scores(matrix)
        order = scores.argsort()[::-1]
        best, second = int(order[0]), int(order[1])
        plain = matrix[best].tobytes()
        if dump_manifest is not None:
            (args.dump_all / f"{record['offset']:09d}.txt").write_text("\n".join(strings(plain)))
            dump_manifest.write(json.dumps({
                "offset": record["offset"], "size": record["size"], "tag": record["tag"],
                "seed": f"0x{best:03x}", "score": float(scores[best]),
                "runner_up": float(scores[second])}) + "\n")
        if scores[best] < 24 or scores[best] < scores[second] * 1.3:
            unmatched += 1
            continue
        candidates = by_class.get(best, [])
        if len(candidates) != 1:
            unmatched += 1
            continue
        name = candidates[0]
        # the winning row of the matrix already holds this key class's plaintext; decrypting
        # the whole slice separately would repeat the work in Python for no gain
        if args.full and record["size"] > len(plain):
            plain = decrypt(pack[record["offset"]:record["offset"] + record["size"]],
                            t0, t1, best)
        named[name] = {**record, "seed": f"0x{best:03x}", "text": strings(plain)}
        if args.text:
            args.text.mkdir(parents=True, exist_ok=True)
            (args.text / (name.replace("/", "_") + ".txt")).write_text("\n".join(named[name]["text"]))

    print(f"named {len(named)} records, {unmatched} left unassigned "
          f"(no unique key class or ambiguous name)")
    if dump_manifest is not None:
        dump_manifest.close()
        print(f"dumped readable strings for all {len(recs)} records into {args.dump_all}")
    for name in sorted(named)[:20]:
        entry = named[name]
        print(f"  {name:<28} tag={entry['tag']:<5} size={entry['size']:<8} seed={entry['seed']}")
    for pattern in args.grep:
        regex = re.compile(pattern, re.I)
        hits = [n for n in named if regex.search(n)]
        print(f"\n{len(hits)} names match {pattern!r}: {sorted(hits)}")

    if args.json:
        args.json.write_text(json.dumps(
            {n: {k: v for k, v in e.items() if k != "text"} for n, e in named.items()}, indent=1))
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
