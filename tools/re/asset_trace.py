#!/usr/bin/env python3
"""Capture and validate a focused runtime trace of the named ``hand2`` asset.

The capture uses read-only Frida Interceptor callbacks over functions whose
RVAs and signatures are checked by ``asset_trace_frida.js``. Captures and
reports are restricted to the repository's persistent ignored ``.tmp/`` tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys
import threading
import time
from typing import Any, Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(pathlib.Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from asset_model import inspect_payload
from pack_crypt import decrypt, load_tables, rva_to_offset, seed_for_name

GAME_DIR = pathlib.Path.home() / ".local/share/Steam/steamapps/common/hyperdemon"
STEAM_ROOT = GAME_DIR.parents[2]
STEAM_APP_ID = "1743850"
PROTON_RUNTIME = STEAM_ROOT / "steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"
PROTON_LAUNCHER = STEAM_ROOT / "steamapps/common/Proton - Experimental/proton"
COMPAT_DATA = STEAM_ROOT / f"steamapps/compatdata/{STEAM_APP_ID}"
HAND2_OFFSET = 295_758_784
HAND2_SIZE = 21_972
HAND2_TYPE = 1
GAME_EXE_SHA256 = "7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611"
HOOK_SIGNATURES = {
    0x211e70: bytes.fromhex("488bc44489482089"),
    0x212920: bytes.fromhex("4056574155415641"),
    0x213370: bytes.fromhex("48895c2408488974"),
    0x205ec0: bytes.fromhex("44894c2420448944"),
    0x21a710: bytes.fromhex("488bc45541564157"),
    # Exact five-byte call instruction in FUN_140211e70; the rel32 resolves to
    # FUN_140212920. Keep this independent of the function-entry signatures.
    0x211f78: bytes.fromhex("e8a3090000"),
}
TRACE_SCRIPT = pathlib.Path(__file__).with_name("asset_trace_frida.js")


def _configure_persistent_temp() -> None:
    persistent = str(REPO_ROOT / ".tmp")
    pathlib.Path(persistent).mkdir(parents=True, exist_ok=True)
    for variable in ("TMPDIR", "TMP", "TEMP"):
        os.environ[variable] = persistent


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def expected_hand2(archive: pathlib.Path, exe: pathlib.Path) -> dict[str, Any]:
    """Decrypt the verified record and hash its payload and decoded arrays."""
    exe_bytes = exe.read_bytes()
    if _sha256(exe_bytes) != GAME_EXE_SHA256:
        raise ValueError("executable hash does not match the Ghidra-verified game build")
    for rva, signature in HOOK_SIGNATURES.items():
        file_offset = rva_to_offset(exe_bytes, rva)
        if exe_bytes[file_offset:file_offset + len(signature)] != signature:
            raise ValueError(f"executable signature mismatch at RVA 0x{rva:x}")
    tables = load_tables(exe)
    with archive.open("rb") as handle:
        if archive.stat().st_size < HAND2_OFFSET + HAND2_SIZE:
            raise ValueError(f"archive is too short for hand2: {archive}")
        handle.seek(HAND2_OFFSET)
        encrypted = handle.read(HAND2_SIZE)
    if len(encrypted) != HAND2_SIZE:
        raise ValueError("short read while extracting the hand2 record")
    payload = decrypt(encrypted, tables[0], tables[1], seed_for_name("hand2"))
    model = inspect_payload(payload)
    vertex_start = model["vertex_offset"]
    vertex_size = model["vertex_count"] * model["vertex_stride"]
    index_start = model["index_offset"]
    index_size = model["index_count"] * model["index_width"]
    return {
        "archive": str(archive),
        "record": {
            "name": "hand2",
            "archive_offset": HAND2_OFFSET,
            "encrypted_size": HAND2_SIZE,
            "type_tag": HAND2_TYPE,
            "seed": seed_for_name("hand2"),
            "key_class": seed_for_name("hand2") & 0x3ff,
        },
        "payload_sha256": _sha256(payload),
        "payload_size": len(payload),
        "graph_version": model["graph_version"],
        "mesh": {
            "vertex_count": model["vertex_count"],
            "index_count": model["index_count"],
            "vertex_stride": model["vertex_stride"],
            "vertex_array_sha256": _sha256(payload[vertex_start:vertex_start + vertex_size]),
            "index_array_sha256": _sha256(payload[index_start:index_start + index_size]),
        },
    }


def validate_capture(events: Iterable[dict[str, Any]],
                     original: dict[str, Any],
                     modified: dict[str, Any]) -> dict[str, Any]:
    """Classify each hand2 request by the strongest observed loader stage."""
    rows = [event for event in events if isinstance(event, dict)]
    requests: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        if row.get("name") != "hand2" or row.get("request_id") is None:
            continue
        try:
            request_id = int(row["request_id"])
        except (TypeError, ValueError):
            continue
        requests.setdefault(request_id, []).append(row)

    results: list[dict[str, Any]] = []
    for request_id, trace in sorted(requests.items()):
        event_names = [row.get("event") for row in trace]
        result: dict[str, Any] = {
            "request_id": request_id,
            "stages": {"lookup": False, "record_resolved": False,
                       "decrypted": False, "parsed": False, "mesh_decoded": False},
            "variant": None,
            "errors": [],
        }
        positions: dict[str, int] = {}
        for event_name, stage in (
            ("asset_lookup", "lookup"),
            ("record_resolved", "record_resolved"),
            ("payload_decrypted", "decrypted"),
            ("tag1_parse_result", "parsed"),
            ("mesh_decode", "mesh_decoded"),
        ):
            if event_name in event_names:
                positions[stage] = event_names.index(event_name)
                result["stages"][stage] = True

        lookup = next((r for r in trace if r.get("event") == "asset_lookup"), None)
        if lookup and lookup.get("name") != "hand2":
            result["errors"].append("lookup name does not match hand2")
        lookup_result = next((r for r in trace if r.get("event") == "asset_lookup_result"), None)
        if lookup_result and not lookup_result.get("found"):
            result["errors"].append("named resolver returned no asset object")

        record = next((r for r in trace if r.get("event") == "record_resolved"), None)
        target_record = False
        if record:
            result["record_type_tag"] = record.get("type_tag")
            if record.get("type_tag") != HAND2_TYPE:
                result["record_role"] = "same-name-non-model-record"
            else:
                target_record = True
                result["record_role"] = "tag1-model-record"
                for key in ("archive_offset", "encrypted_size", "type_tag"):
                    expected_value = original["record"][key]
                    if record.get(key) != expected_value:
                        result["errors"].append(
                            f"record {key} mismatch: expected {expected_value}, got {record.get(key)}")
        result["target_record_match"] = target_record and not result["errors"]

        decrypted = next((r for r in trace if r.get("event") == "payload_decrypted"), None)
        if decrypted and target_record:
            if decrypted.get("size") != original["payload_size"]:
                result["errors"].append("decrypted payload size mismatch")
            if decrypted.get("seed") != original["record"]["seed"]:
                result["errors"].append("decryption seed mismatch")
            if decrypted.get("key_class") != original["record"]["key_class"]:
                result["errors"].append("decryption key class mismatch")
            digest = decrypted.get("payload_sha256")
            if digest == modified["payload_sha256"]:
                result["variant"] = "modified"
            elif digest == original["payload_sha256"]:
                result["variant"] = "original"
            else:
                result["variant"] = "unknown"
                result["errors"].append("decrypted payload hash matches neither archive variant")

        parsed = next((r for r in trace if r.get("event") == "tag1_parse_result"), None)
        if parsed and target_record and not parsed.get("success"):
            result["errors"].append("version-3 tag-1 parser did not report success")

        mesh = next((r for r in trace if r.get("event") == "mesh_decode"), None)
        if mesh and target_record:
            expected_mesh = modified["mesh"] if result["variant"] == "modified" else original["mesh"]
            for key in ("vertex_count", "index_count", "vertex_stride",
                        "vertex_array_sha256", "index_array_sha256"):
                if mesh.get(key) != expected_mesh[key]:
                    result["errors"].append(f"mesh {key} mismatch")
            if not mesh.get("complete") or not mesh.get("arrays_bounded"):
                result["errors"].append("mesh decode completion or bounded arrays not confirmed")

        ordered = [stage for stage in ("lookup", "record_resolved", "decrypted", "parsed", "mesh_decoded")
                   if stage in positions]
        if any(positions[a] >= positions[b] for a, b in zip(ordered, ordered[1:])):
            result["errors"].append("loader stage events are out of order")
        result["strongest_stage"] = next(
            (stage for stage in reversed(("lookup", "record_resolved", "decrypted", "parsed", "mesh_decoded"))
             if result["stages"][stage]), "not_requested")
        required_stages = all(result["stages"].values()) and result["target_record_match"]
        result["verified"] = bool(required_stages and not result["errors"])
        results.append(result)

    ready = any(row.get("event") == "trace_ready" and
                row.get("signature_check") == "passed" and
                row.get("hook_installation") == "complete" for row in rows)
    attached = any(row.get("event") == "attach_succeeded" for row in rows)
    process_identified = any(row.get("event") == "process_identified" and
                             row.get("identity_verified") for row in rows)
    signatures = any(row.get("event") == "trace_ready" and
                     row.get("signature_check") == "passed" for row in rows)
    coverage = next((row for row in rows if row.get("event") == "coverage_started"), None)
    capture_errors = [row for row in rows if row.get("event") == "capture_error"]
    if not process_identified:
        no_request_explanation = "the expected executable process was not identified"
    elif not attached:
        no_request_explanation = "the target process was identified, but Frida did not attach"
    elif not signatures:
        no_request_explanation = "the attached process did not pass executable signature checks"
    elif not ready:
        no_request_explanation = "function checks passed, but all trace hooks were not installed"
    elif not coverage:
        no_request_explanation = "hooks became ready, but the capture coverage window did not start"
    elif not coverage.get("process_start_coverage"):
        no_request_explanation = (
            "no hand2 lookup was observed after hooks became ready; earlier activity may have been missed")
    else:
        no_request_explanation = "no hand2 lookup was observed during process-start coverage"
    if any(r["verified"] for r in results):
        interpretation = "mesh decoded and copied into the game's mesh object; rendering was not traced"
    elif not results:
        interpretation = no_request_explanation
    else:
        strongest = max((r["strongest_stage"] for r in results),
                        key=lambda stage: ("not_requested", "lookup", "record_resolved",
                                           "decrypted", "parsed", "mesh_decoded").index(stage))
        interpretation = (f"hand2-related activity reached {strongest}; a fully verified mesh decode "
                          "was not established")
    return {
        "target": "hand2",
        "process_identified": process_identified,
        "attach_succeeded": attached,
        "signature_check_passed": signatures,
        "trace_ready": ready,
        "hooks_active_during_capture": bool(ready and coverage),
        "process_start_coverage": bool(coverage and coverage.get("process_start_coverage")),
        "instrumentation_errors": capture_errors,
        "requests_observed": len(results),
        "requests": results,
        "target_record_requested": any(r.get("target_record_match") for r in results),
        "target_payload_decrypted": any(
            r.get("target_record_match") and r.get("variant") in ("original", "modified") and
            not any("payload" in error or "decryption" in error for error in r["errors"])
            for r in results),
        "modified_payload_decrypted": any(
            r.get("target_record_match") and r.get("variant") == "modified" and
            not any("payload" in error or "decryption" in error for error in r["errors"])
            for r in results),
        "hand2_mesh_decoded": any(r["verified"] for r in results),
        "modified_mesh_decoded": any(r["verified"] and r.get("variant") == "modified"
                                      for r in results),
        "hand2_consumed": any(r["verified"] for r in results),
        "rendering_observed": False,
        "interpretation": interpretation,
    }


def read_jsonl(path: pathlib.Path) -> list[dict[str, Any]]:
    events = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"invalid JSON on line {number}: {error}") from error
        if isinstance(value, dict) and value.get("type") == "send" and isinstance(value.get("payload"), dict):
            value = value["payload"]
        events.append(value)
    return events


def _inside_persistent_tmp(path: pathlib.Path) -> bool:
    root = (REPO_ROOT / ".tmp").resolve()
    try:
        path.resolve().relative_to(root)
        return True
    except ValueError:
        return False


def _process_name_matches(observed: str, requested: str) -> bool:
    basename = observed.replace("\\", "/").rsplit("/", 1)[-1]
    return basename.casefold() == requested.replace("\\", "/").rsplit("/", 1)[-1].casefold()


def _spawn_is_target_game(spawn: Any) -> bool:
    """Require the game executable path, not a basename-only process match."""
    values = [getattr(spawn, "identifier", "")]
    values.extend(getattr(spawn, "argv", None) or [])
    for value in values:
        normalized = str(value).replace("\\", "/").casefold()
        if normalized.endswith("/steamapps/common/hyperdemon/hyperdemon.exe"):
            return True
    return False


def capture(pid: int, output: pathlib.Path, duration: float,
            *, launch_gated: bool = False, attached_session: Any = None,
            resume_target: Any = None) -> None:
    _configure_persistent_temp()
    if not _inside_persistent_tmp(output):
        raise ValueError(f"capture output must be inside {REPO_ROOT / '.tmp'}")
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        import frida  # type: ignore[import-not-found]
    except ImportError as error:
        output.write_text(json.dumps({"event": "capture_error", "stage": "frida_import",
                                      "message": str(error)}) + "\n", encoding="utf-8")
        raise RuntimeError("the Python frida package is required for capture") from error
    source = TRACE_SCRIPT.read_text(encoding="utf-8")
    with output.open("w", encoding="utf-8") as handle:
        session = None
        script = None
        ready = threading.Event()
        script_error: list[str] = []

        def controller_event(event: str, **fields: Any) -> None:
            row = {"event": event, "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                   "pid": pid, **fields}
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
            handle.flush()

        def on_message(message: dict[str, Any], _data: bytes | None) -> None:
            handle.write(json.dumps(message, separators=(",", ":")) + "\n")
            handle.flush()
            payload = message.get("payload") if message.get("type") == "send" else None
            if isinstance(payload, dict) and payload.get("event") == "trace_ready":
                ready.set()
            elif message.get("type") == "error":
                script_error.append(str(message.get("description", "script error")))

        try:
            try:
                device = frida.get_local_device()
                observed = next((p for p in device.enumerate_processes() if p.pid == pid), None)
                if observed is None:
                    raise RuntimeError(f"PID {pid} is no longer present on the local device")
                if not _process_name_matches(observed.name, "hyperdemon.exe"):
                    controller_event("process_identity_rejected", process_name=observed.name,
                                     identity_verified=False)
                    raise RuntimeError(f"PID {pid} is {observed.name!r}, not hyperdemon.exe")
                controller_event("process_identified", process_name=observed.name,
                                 process_parameters=getattr(observed, "parameters", None),
                                 identity_verified=True)
                session = attached_session if attached_session is not None else device.attach(pid)
                controller_event("attach_succeeded")
                script = session.create_script(source)
            except Exception as error:
                controller_event("capture_error", stage="attach", error_type=type(error).__name__,
                                 message=str(error))
                raise RuntimeError(f"could not attach Frida to PID {pid}: {error}") from error
            script.on("message", on_message)
            script.load()
            controller_event("script_loaded", process_start_coverage=bool(launch_gated))
            if not ready.wait(3.0):
                detail = script_error[0] if script_error else "no trace_ready event received"
                controller_event("capture_error", stage="instrumentation_preflight",
                                 message=detail)
                raise RuntimeError(f"trace preflight failed: {detail}")
            if resume_target is not None:
                resume_target()
            controller_event("coverage_started", process_start_coverage=bool(launch_gated))
            time.sleep(duration)
        except Exception as error:
            if not any(line for line in script_error):
                controller_event("capture_error", stage="capture", error_type=type(error).__name__,
                                 message=str(error))
            raise
        finally:
            if script is not None:
                try:
                    script.unload()
                except Exception:
                    pass
            if session is not None:
                try:
                    session.detach()
                except Exception:
                    pass
            controller_event("capture_closed")


def capture_proton_child(output: pathlib.Path, duration: float, wait_seconds: float) -> None:
    """Launch Proton under Frida spawn-gating and instrument the game before resume."""
    if not _inside_persistent_tmp(output):
        raise ValueError(f"capture output must be inside {REPO_ROOT / '.tmp'}")
    _configure_persistent_temp()
    for required in (PROTON_RUNTIME, PROTON_LAUNCHER, GAME_DIR / "hyperdemon.exe"):
        if not required.exists():
            raise RuntimeError(f"required local Proton/game path is missing: {required}")
    output.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({
        "TMPDIR": str(REPO_ROOT / ".tmp"),
        "TMP": str(REPO_ROOT / ".tmp"),
        "TEMP": str(REPO_ROOT / ".tmp"),
        "STEAM_COMPAT_DATA_PATH": str(COMPAT_DATA),
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": str(STEAM_ROOT),
        "STEAM_COMPAT_INSTALL_PATH": str(GAME_DIR),
        "STEAM_COMPAT_APP_ID": STEAM_APP_ID,
        "SteamAppId": STEAM_APP_ID,
        "SteamGameId": STEAM_APP_ID,
    })
    command = [str(PROTON_RUNTIME), "--verb=waitforexitandrun", "--",
               str(PROTON_LAUNCHER), "waitforexitandrun", str(GAME_DIR / "hyperdemon.exe")]
    game_pid = None
    root_pid = None
    session = None
    device = None
    on_spawn_added = None
    spawn_gating_enabled = False
    selected = threading.Event()
    selection: dict[str, Any] = {"candidates": [], "error": None}
    selection_lock = threading.Lock()
    launcher_status = output.with_suffix(".launcher.json")
    launch_data: dict[str, Any] = {"command": command, "wait_seconds": wait_seconds,
                                   "process_candidates": [], "coverage_from_process_start": False,
                                   "launch_mode": "Frida spawn-gated Proton child"}
    try:
        import frida  # type: ignore[import-not-found]
        device = frida.get_local_device()

        def on_spawn_added(spawn: Any) -> None:
            nonlocal game_pid, session
            spawn_row = {"pid": spawn.pid, "identifier": getattr(spawn, "identifier", ""),
                         "argv": getattr(spawn, "argv", None)}
            is_target = _spawn_is_target_game(spawn)
            with selection_lock:
                if len(launch_data["process_candidates"]) < 128:
                    launch_data["process_candidates"].append({**spawn_row, "target_path_match": is_target})
                if is_target and game_pid is None and selection["error"] is None:
                    try:
                        game_pid = spawn.pid
                        session = device.attach(spawn.pid)
                        selection["session"] = session
                        selection["spawn"] = spawn_row
                    except Exception as error:
                        selection["error"] = f"attach to spawn-gated target failed: {error}"
                    selected.set()
                    return
                if is_target and game_pid is not None:
                    selection["error"] = "multiple matching game processes spawned; refusing ambiguous run"
                    selected.set()
            try:
                device.resume(spawn.pid)
            except Exception as error:
                with selection_lock:
                    launch_data.setdefault("resume_errors", []).append({**spawn_row, "error": str(error)})

        device.on("spawn-added", on_spawn_added)
        device.enable_spawn_gating()
        spawn_gating_enabled = True
        launch_data["started_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        launcher_status.write_text(json.dumps(launch_data, indent=2) + "\n", encoding="utf-8")
        # Frida starts the Linux runtime suspended. The spawn gate then holds the
        # Wine game process before its entry point while non-target processes resume.
        root_pid = device.spawn(str(PROTON_RUNTIME), argv=command, env=env, cwd=str(GAME_DIR))
        device.resume(root_pid)
        if not selected.wait(wait_seconds):
            selection["error"] = "no game process with the expected installed executable path was spawned"
        if selection["error"]:
            raise RuntimeError(selection["error"])
        launch_data["game_pid"] = game_pid
        launch_data["target_spawn"] = selection.get("spawn")
        launch_data["outcome"] = "game_process_spawned_suspended"
        launcher_status.write_text(json.dumps(launch_data, indent=2) + "\n", encoding="utf-8")
        capture(game_pid, output, duration, launch_gated=True,
                attached_session=session, resume_target=lambda: device.resume(game_pid))
        launch_data["coverage_from_process_start"] = any(
            row.get("event") == "coverage_started" and row.get("process_start_coverage")
            for row in read_jsonl(output))
        launch_data["outcome"] = ("instrumented_capture_completed"
                                   if launch_data["coverage_from_process_start"]
                                   else "capture_completed_without_startup_coverage")
        launch_data["capture_completed"] = True
    except Exception as error:
        launch_data["outcome"] = "instrumentation_or_launch_failed"
        launch_data["error"] = f"{type(error).__name__}: {error}"
        if not output.exists():
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps({"event": "capture_error", "stage": "launch",
                                          "message": launch_data["error"]}) + "\n",
                              encoding="utf-8")
        if isinstance(error, (OSError, ValueError, RuntimeError)):
            raise
        raise RuntimeError(f"Frida/Proton launch failed: {type(error).__name__}: {error}") from error
    finally:
        if device is not None:
            if on_spawn_added is not None:
                try:
                    device.off("spawn-added", on_spawn_added)
                except Exception as error:
                    launch_data["spawn_callback_cleanup_error"] = str(error)
            if spawn_gating_enabled:
                try:
                    device.disable_spawn_gating()
                except Exception as error:
                    launch_data["spawn_gating_cleanup_error"] = str(error)
        if session is not None:
            try:
                session.detach()
            except Exception as error:
                launch_data["session_cleanup_error"] = str(error)
        for pid_to_kill in (game_pid, root_pid):
            if pid_to_kill is not None:
                try:
                    device.kill(pid_to_kill)
                except Exception:
                    pass
        launch_data["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        launcher_status.write_text(json.dumps(launch_data, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    cap = sub.add_parser("capture", help="attach the read-only Frida tracer to a running game")
    cap.add_argument("--pid", type=int, help="attach directly to this process")
    cap.add_argument("--wait-process", help="wait for this process name before attaching")
    cap.add_argument("--wait-seconds", type=float, default=45.0)
    cap.add_argument("--duration", type=float, default=60.0)
    cap.add_argument("--launch-proton-child", action="store_true",
                     help="launch this installed game as a child process for Yama-compatible attach")
    cap.add_argument("--out", type=pathlib.Path, required=True)
    val = sub.add_parser("validate", help="compare captured hashes with original and patched archives")
    val.add_argument("--events", type=pathlib.Path, required=True)
    val.add_argument("--original-pack", type=pathlib.Path, default=GAME_DIR / "asset/asset")
    val.add_argument("--modified-pack", type=pathlib.Path, required=True)
    val.add_argument("--exe", type=pathlib.Path, default=GAME_DIR / "hyperdemon.exe")
    val.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    try:
        if args.command == "capture":
            pid = args.pid
            if args.launch_proton_child:
                if pid is not None or args.wait_process is not None:
                    raise ValueError("--launch-proton-child cannot be combined with --pid or --wait-process")
                capture_proton_child(args.out, args.duration, args.wait_seconds)
                return 0
            if (pid is None) == (args.wait_process is None):
                raise ValueError("provide exactly one of --pid or --wait-process")
            if args.wait_process:
                try:
                    import frida  # type: ignore[import-not-found]
                except ImportError as error:
                    raise RuntimeError("the Python frida package is required for capture") from error
                device = frida.get_local_device()
                deadline = time.monotonic() + args.wait_seconds
                while time.monotonic() < deadline:
                    found = [p for p in device.enumerate_processes()
                             if _process_name_matches(p.name, args.wait_process)]
                    if len(found) == 1:
                        pid = found[0].pid
                        break
                    if len(found) > 1:
                        raise RuntimeError("multiple processes match " + repr(args.wait_process) +
                                           "; use --pid to select the intended process")
                    time.sleep(0.1)
                if pid is None:
                    raise RuntimeError(f"process {args.wait_process!r} did not appear in time")
            capture(pid, args.out, args.duration)
            return 0
        if not _inside_persistent_tmp(args.report):
            raise ValueError(f"report output must be inside {REPO_ROOT / '.tmp'}")
        original = expected_hand2(args.original_pack, args.exe)
        modified = expected_hand2(args.modified_pack, args.exe)
        report = validate_capture(read_jsonl(args.events), original, modified)
        report["expected_variants"] = {"original": original, "modified": modified}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in
                          ("trace_ready", "requests_observed", "target_record_requested",
                           "target_payload_decrypted", "modified_payload_decrypted",
                           "hand2_mesh_decoded", "modified_mesh_decoded", "hand2_consumed",
                           "rendering_observed", "interpretation")},
                         indent=2))
        return 0 if report["trace_ready"] and not any(
            request["errors"] for request in report["requests"]) else 2
    except (OSError, ValueError, RuntimeError) as error:
        print(f"asset trace: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
