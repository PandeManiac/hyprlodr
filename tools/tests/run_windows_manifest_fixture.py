#!/usr/bin/env python3
"""Run the compiled native append-manifest validator under Wine."""
import copy, hashlib, json, pathlib, shutil, subprocess, sys

exe, artifact, manifest, work = map(pathlib.Path, sys.argv[1:5])
game_exe = pathlib.Path(sys.argv[5]) if len(sys.argv) > 5 else None
work.mkdir(parents=True, exist_ok=True)
wine_exe = subprocess.check_output(["winepath", "-w", str(game_exe or exe)], text=True).strip()
binary_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
base = json.loads(manifest.read_text())
if game_exe is None:
    base["executable_sha256"] = binary_hash
pin_hash = base["executable_sha256"]

def run(doc, mode, archive=artifact):
    path = work / f"{mode}.json"
    path.write_text(doc if isinstance(doc, str) else json.dumps(doc, separators=(",", ":")))
    win_manifest = subprocess.check_output(["winepath", "-w", str(path)], text=True).strip()
    win_archive = subprocess.check_output(["winepath", "-w", str(archive)], text=True).strip()
    subprocess.run(["wine", str(exe), win_manifest, win_archive, wine_exe, pin_hash, mode], check=True)

def run_paths(manifest_path, archive_path, mode):
    win_manifest = subprocess.check_output(["winepath", "-w", str(manifest_path)], text=True).strip()
    win_archive = subprocess.check_output(["winepath", "-w", str(archive_path)], text=True).strip()
    subprocess.run(["wine", str(exe), win_manifest, win_archive, wine_exe, pin_hash, mode], check=True)

run(base, "valid")
for name, expected, mutate in [
    ("wrong-executable", "executable_unsupported", lambda d: d.update(executable_sha256="0" * 64)),
    ("wrong-archive-hash", "archive_mismatch", lambda d: d["output_archive"].update(sha256="0" * 64)),
    ("wrong-record", "resource_identity_mismatch", lambda d: d["record"].update(original_size=21973)),
    ("wrong-resolver", "resource_identity_mismatch", lambda d: d["record"].update(resolver_table_id="tbl-wrong")),
    ("tag2-same-name", "resource_identity_mismatch", lambda d: d["record"].update(type_tag=2)),
    ("overflow", "manifest_malformed", lambda d: d["record"].update(replacement_offset=2**32)),
    ("unsupported-schema", "manifest_unsupported", lambda d: d.update(schema=2)),
    ("missing-field", "manifest_malformed", lambda d: d.pop("delivery")),
    ("wrong-cipher-hash", "payload_invalid", lambda d: d["record"].update(replacement_ciphertext_sha256="0" * 64)),
]:
    doc = copy.deepcopy(base); mutate(doc); run(doc, "reject-" + expected)

raw=json.dumps(base,separators=(",", ":"))
run(raw.replace('"schema":1', '"schema":1,"schema":1', 1), "reject-manifest_malformed")
run(raw[:-1] + ",}", "reject-manifest_malformed")
run('{"x":' + '[' * 40 + '0' + ']' * 40 + '}', "reject-manifest_malformed")

truncated = work / "truncated-archive"
with truncated.open("wb") as f:
    f.truncate(base["output_archive"]["size"] - 1)
run(base, "reject-archive_mismatch", truncated)
run_paths(work / "missing-manifest.json", artifact, "reject-manifest_missing")
run_paths(work / "valid.json", work / "missing-archive", "reject-archive_mismatch")
altered = work / "modified-ciphertext-archive"
shutil.copyfile(artifact, altered)
with altered.open("r+b") as f:
    f.seek(base["record"]["replacement_offset"] + 12)
    b=f.read(1); f.seek(-1,1); f.write(bytes([b[0]^1]))
modified_doc=copy.deepcopy(base)
digest=hashlib.sha256()
with altered.open("rb") as f:
    for block in iter(lambda:f.read(1<<20),b""): digest.update(block)
modified_doc["output_archive"]["sha256"]=digest.hexdigest()
run(modified_doc,"reject-payload_invalid",altered)
print("windows manifest fixture: valid real archive plus 16 fail-closed cases passed")
