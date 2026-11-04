#!/usr/bin/env python3
"""Build and validate an append-only variable-size replacement archive.

This tool does not patch a running game. It creates a byte-preserving archive
plus a manifest for a separately implemented, fingerprint-gated record override.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from asset_model import inspect_payload
from pack_crypt import decrypt, encrypt, load_tables, seed_for_name

UINT32_END = 1 << 32
MAX_LOADER_SIZE = 0x7FFFFFFF
CHUNK = 1 << 20


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_prefix(path: pathlib.Path, size: int) -> str:
    digest = hashlib.sha256()
    remaining = size
    with pathlib.Path(path).open("rb") as stream:
        while remaining:
            block = stream.read(min(CHUNK, remaining))
            if not block:
                raise ValueError("archive is shorter than its declared original prefix")
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def _record(index: dict, *, name: str, record_id: str, table_id: str,
            offset: int, size: int, tag: int) -> dict:
    matches = [r for r in index.get("records", []) if r.get("id") == record_id]
    if len(matches) != 1:
        raise ValueError("record id is missing or ambiguous in the asset index")
    row = matches[0]
    if row.get("association") != "verified" or row.get("record_evidence") != "resolver-linked":
        raise ValueError("target record name association is not resolver-verified")
    if (row.get("name"), row.get("offset"), row.get("size"), row.get("tag")) != (name, offset, size, tag):
        raise ValueError("indexed record metadata does not match the required target identity")
    mappings = [m for m in row.get("resolver_mappings", [])
                if m.get("name") == name and m.get("resolver_table_id") == table_id
                and m.get("tag") == tag and m.get("offset") == offset and m.get("size") == size]
    same_name = [m for r in index.get("records", []) for m in r.get("resolver_mappings", [])
                 if m.get("name") == name and m.get("resolver_table_id") == table_id and m.get("tag") == tag]
    if len(mappings) != 1 or len(same_name) != 1:
        raise ValueError("resolver table does not identify exactly one matching named record")
    if tag != 1:
        raise ValueError("variable-size model replacement currently supports tag-1 records only")
    return row


def build_append_archive(*, exe: pathlib.Path, pack: pathlib.Path, index_path: pathlib.Path,
                         payload_path: pathlib.Path, output: pathlib.Path,
                         manifest_path: pathlib.Path, name: str, record_id: str,
                         table_id: str, offset: int, size: int, tag: int,
                         expected_pack_sha256: str, expected_exe_sha256: str,
                         expected_record_cipher_sha256: str) -> dict:
    """Append encrypted plaintext and write a verifiable override manifest.

    Output starts at the exact original EOF; no alignment requirement has been
    established for the archive's seek-based reader. Existing records are never
    rewritten. The caller must still arrange a safe runtime metadata override.
    """
    exe, pack, index_path = map(pathlib.Path, (exe, pack, index_path))
    payload_path, output, manifest_path = map(pathlib.Path, (payload_path, output, manifest_path))
    if not pack.is_file() or not exe.is_file() or not index_path.is_file() or not payload_path.is_file():
        raise ValueError("executable, archive, index, and replacement payload must all exist")
    if output.resolve() in {pack.resolve(), exe.resolve(), payload_path.resolve(), index_path.resolve()}:
        raise ValueError("output must not alias the executable, source archive, or payload")
    if manifest_path.resolve() in {output.resolve(), pack.resolve(), exe.resolve(),
                                   payload_path.resolve(), index_path.resolve()}:
        raise ValueError("manifest path must be distinct from all inputs and archive output")
    if output.exists() or manifest_path.exists():
        raise ValueError("refusing to overwrite an existing output or manifest")
    partial = output.with_name(output.name + ".partial")
    manifest_partial = manifest_path.with_name(manifest_path.name + ".partial")
    if partial.exists() or manifest_partial.exists():
        raise ValueError("refusing to overwrite an existing partial output or manifest")
    actual_pack_hash, actual_exe_hash = sha256_file(pack), sha256_file(exe)
    if actual_pack_hash != expected_pack_sha256 or actual_exe_hash != expected_exe_sha256:
        raise ValueError("source archive or executable fingerprint mismatch")
    index = json.loads(index_path.read_text())
    source = index.get("source", {})
    if source.get("pack_sha256") != actual_pack_hash or source.get("exe_sha256") != actual_exe_hash:
        raise ValueError("asset index fingerprints do not match the current archive and executable")
    row = _record(index, name=name, record_id=record_id, table_id=table_id,
                  offset=offset, size=size, tag=tag)
    pack_size = pack.stat().st_size
    if offset < 0 or size <= 0 or offset > pack_size or size > pack_size - offset:
        raise ValueError("original record range exceeds source archive bounds")
    if row.get("encrypted_sha256") != expected_record_cipher_sha256:
        raise ValueError("indexed encrypted record hash does not match the required identity")
    with pack.open("rb") as stream:
        stream.seek(offset)
        original_cipher = stream.read(size)
    if len(original_cipher) != size or sha256_bytes(original_cipher) != expected_record_cipher_sha256:
        raise ValueError("original encrypted record bytes do not match the required hash")

    plain = payload_path.read_bytes()
    parsed = inspect_payload(plain)
    if len(plain) >= MAX_LOADER_SIZE:
        raise ValueError("replacement is too large for the loader's signed allocation size")
    new_offset = pack_size
    if new_offset > 0xFFFFFFFF or len(plain) > MAX_LOADER_SIZE or new_offset + len(plain) > UINT32_END:
        raise ValueError("appended replacement exceeds verified 32-bit record address limits")
    t0, t1, _ = load_tables(exe)
    seed = seed_for_name(name) & 0x3FF
    cipher = encrypt(plain, t0, t1, seed)
    if len(cipher) != len(plain) or decrypt(cipher, t0, t1, seed) != plain:
        raise ValueError("encrypted replacement failed local round-trip validation")

    output.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    partial_created = manifest_partial_created = output_published = manifest_published = False
    try:
        with pack.open("rb") as src, partial.open("xb") as dst:
            partial_created = True
            while True:
                block = src.read(CHUNK)
                if not block:
                    break
                dst.write(block)
            dst.write(cipher)
            dst.flush()
            os.fsync(dst.fileno())
        if partial.stat().st_size != pack_size + len(cipher):
            raise ValueError("output archive length does not match appended replacement")
        if sha256_prefix(partial, pack_size) != actual_pack_hash:
            raise ValueError("original archive prefix was not preserved exactly")
        output_hash = sha256_file(partial)
        manifest = {
            "schema": 1,
            "delivery": "append-only-record-override-required",
            "executable_sha256": actual_exe_hash,
            "source_archive": {"sha256": actual_pack_hash, "size": pack_size},
            "output_archive": {"sha256": output_hash, "size": partial.stat().st_size},
            "record": {"name": name, "record_id": record_id, "resolver_table_id": table_id,
                       "association": "verified", "original_offset": offset,
                       "original_size": size, "type_tag": tag,
                       "original_ciphertext_sha256": expected_record_cipher_sha256,
                       "replacement_offset": new_offset, "replacement_size": len(cipher),
                       "seed": seed, "replacement_ciphertext_sha256": sha256_bytes(cipher),
                       "replacement_plaintext_sha256": sha256_bytes(plain),
                       "mesh": {"vertex_count": parsed["vertex_count"],
                                "index_count": parsed["index_count"],
                                "vertex_stride": parsed["vertex_stride"]}},
            "runtime_override": {"implemented": False,
                                 "note": "Requires a separately validated, synchronized record metadata override."},
        }
        with manifest_partial.open("x", encoding="utf-8") as stream:
            manifest_partial_created = True
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        # Hard-link publication is atomic and refuses to replace a path that
        # appeared after the preflight check, preserving concurrent user files.
        os.link(partial, output)
        output_published = True
        partial.unlink()
        partial_created = False
        os.link(manifest_partial, manifest_path)
        manifest_published = True
        manifest_partial.unlink()
        manifest_partial_created = False
    except Exception:
        if partial_created:
            partial.unlink(missing_ok=True)
        if manifest_partial_created:
            manifest_partial.unlink(missing_ok=True)
        if output_published:
            output.unlink(missing_ok=True)
        if manifest_published:
            manifest_path.unlink(missing_ok=True)
        raise
    return manifest


def validate_override_read(*, exe: pathlib.Path, archive: pathlib.Path,
                           manifest_path: pathlib.Path, observed_record: dict) -> tuple[bytes, dict]:
    """Stand-in for selecting and reading the record override; no process writes."""
    manifest = json.loads(pathlib.Path(manifest_path).read_text())
    if manifest.get("schema") != 1 or manifest.get("delivery") != "append-only-record-override-required":
        raise ValueError("unsupported append manifest")
    if sha256_file(exe) != manifest["executable_sha256"]:
        raise ValueError("executable fingerprint mismatch")
    if sha256_file(archive) != manifest["output_archive"]["sha256"]:
        raise ValueError("replacement archive fingerprint mismatch")
    target = manifest["record"]
    if (target.get("association") != "verified" or target.get("type_tag") != 1
            or target.get("replacement_offset") != manifest["source_archive"]["size"]
            or target.get("replacement_size") != manifest["output_archive"]["size"]
            - manifest["source_archive"]["size"]):
        raise ValueError("manifest replacement append range is invalid or not a verified tag-1 record")
    if target["seed"] != seed_for_name(target["name"]) & 0x3FF:
        raise ValueError("manifest decryption seed does not match the logical name")
    expected_identity = (target["name"], target["record_id"], target["resolver_table_id"],
                         target["original_offset"], target["original_size"], target["type_tag"])
    observed_identity = (observed_record.get("name"), observed_record.get("record_id"),
                         observed_record.get("resolver_table_id"), observed_record.get("offset"),
                         observed_record.get("size"), observed_record.get("tag"))
    if observed_identity != expected_identity:
        raise ValueError("selected runtime record does not match the manifest identity and original metadata")
    replacement_offset, replacement_size = target["replacement_offset"], target["replacement_size"]
    archive_size = pathlib.Path(archive).stat().st_size
    if (replacement_offset < 0 or replacement_size <= 0 or replacement_offset > archive_size
            or replacement_size > archive_size - replacement_offset
            or replacement_offset + replacement_size > UINT32_END
            or replacement_size >= MAX_LOADER_SIZE):
        raise ValueError("replacement record range is invalid for archive or loader limits")
    with pathlib.Path(archive).open("rb") as stream:
        stream.seek(replacement_offset)
        cipher = stream.read(replacement_size)
    if len(cipher) != replacement_size or sha256_bytes(cipher) != target["replacement_ciphertext_sha256"]:
        raise ValueError("replacement bytes are missing or do not match the manifest")
    t0, t1, _ = load_tables(exe)
    plain = decrypt(cipher, t0, t1, target["seed"])
    if sha256_bytes(plain) != target["replacement_plaintext_sha256"]:
        raise ValueError("replacement plaintext hash mismatch")
    model = inspect_payload(plain)
    actual_counts = (model["vertex_count"], model["index_count"], model["vertex_stride"])
    expected_counts = (target["mesh"]["vertex_count"], target["mesh"]["index_count"],
                       target["mesh"]["vertex_stride"])
    if actual_counts != expected_counts:
        raise ValueError("decoded model metadata does not match manifest")
    return plain, model


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for arg in ("exe", "pack", "index", "payload", "out", "manifest"):
        parser.add_argument("--" + arg, required=True, type=pathlib.Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--record-id", required=True)
    parser.add_argument("--table-id", required=True)
    parser.add_argument("--offset", required=True, type=lambda s: int(s, 0))
    parser.add_argument("--size", required=True, type=lambda s: int(s, 0))
    parser.add_argument("--tag", required=True, type=lambda s: int(s, 0))
    parser.add_argument("--expected-pack-sha256", required=True)
    parser.add_argument("--expected-exe-sha256", required=True)
    parser.add_argument("--expected-record-cipher-sha256", required=True)
    args = parser.parse_args(argv)
    try:
        result = build_append_archive(exe=args.exe, pack=args.pack, index_path=args.index,
            payload_path=args.payload, output=args.out, manifest_path=args.manifest,
            name=args.name, record_id=args.record_id, table_id=args.table_id,
            offset=args.offset, size=args.size, tag=args.tag,
            expected_pack_sha256=args.expected_pack_sha256,
            expected_exe_sha256=args.expected_exe_sha256,
            expected_record_cipher_sha256=args.expected_record_cipher_sha256)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        parser.error(str(error))
    record = result["record"]
    print(f"appended {record['name']} tag {record['type_tag']} at {record['replacement_offset']} "
          f"({record['replacement_size']} bytes); runtime metadata override is still required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
