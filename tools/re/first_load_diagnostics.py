#!/usr/bin/env python3
"""Fail-closed first-load trace coverage and disposable-runtime preflight."""
from __future__ import annotations

import argparse
import errno
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
ISOLATED_ROOT = (ROOT / ".tmp/first-load-diagnostic/isolated").resolve()


def _json_lines(path: Path) -> list[dict]:
    rows = []
    with path.open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{number}: invalid JSON: {error}") from error
            if not isinstance(row, dict):
                raise ValueError(f"{path}:{number}: event must be an object")
            rows.append(row)
    return rows


def build_coverage(startup: Path, files: Path, attach: Path,
                   process_exited: bool) -> dict:
    blockers = []
    if attach.is_file():
        metadata = json.loads(attach.read_text(encoding="utf-8"))
    else:
        metadata = {}
        blockers.append("attach_record_missing")
    run_id = metadata.get("run_id")
    pid = metadata.get("pid")
    if not isinstance(run_id, str) or not isinstance(pid, int):
        blockers.append("run_identity_unavailable")
    startup_rows = _json_lines(startup) if startup.is_file() else []
    file_rows = _json_lines(files) if files.is_file() else []
    if not startup.is_file():
        blockers.append("startup_event_log_missing")
    if not files.is_file():
        blockers.append("file_event_log_missing")
    rows = startup_rows + file_rows
    correlation_ok = bool(run_id and isinstance(pid, int))
    for row in rows:
        if row.get("run_id") != run_id or row.get("pid") != pid:
            if run_id and isinstance(pid, int):
                raise ValueError("mixed run IDs/PIDs in diagnostic inputs")
            correlation_ok = False
        if not isinstance(row.get("seq"), int):
            correlation_ok = False
            blockers.append("event_missing_shared_sequence")
    if rows and not correlation_ok:
        blockers.append("events_not_correlated_to_attach_record")
    rows.sort(key=lambda row: row.get("seq") if isinstance(row.get("seq"), int)
              else 0x100000000)
    sequences = [row["seq"] for row in rows if isinstance(row.get("seq"), int)]
    unique_sequences = len(sequences) == len(set(sequences))
    if not unique_sequences:
        blockers.append("duplicate_sequence_numbers_or_unmerged_legacy_logs")
    contiguous_sequences = (not sequences or
        sequences == list(range(min(sequences), max(sequences) + 1)))
    if not contiguous_sequences:
        blockers.append("shared_sequence_gap")

    event_names = {row.get("event") for row in rows}
    early_events = [row for row in rows if row.get("event") == "early_hook_install"]
    dropped = sum(int(row.get("count", 0)) for row in rows
                  if row.get("event") == "diagnostic_events_dropped")
    writer_running = any(row.get("event") == "diagnostic_writer_status" and
                         row.get("flags") == 2 for row in startup_rows)
    writer_failed = any(row.get("event") == "diagnostic_writer_status" and
                        row.get("flags") == 0 for row in startup_rows)
    no_drops = dropped == 0 and metadata.get("diagnostic_events_dropped", 0) == 0
    instrumentation_enabled = metadata.get("diagnostics_requested") is True
    complete = bool(process_exited and instrumentation_enabled and writer_running and
                     no_drops and startup.is_file() and files.is_file() and correlation_ok and
                     unique_sequences and contiguous_sequences)
    if not process_exited:
        blockers.append("process_not_confirmed_exited")
    if not instrumentation_enabled:
        blockers.append("diagnostics_not_confirmed_enabled")
    if not writer_running:
        blockers.append("writer_running_not_confirmed")
    if not no_drops:
        blockers.append("diagnostic_ring_overflow")
    if not correlation_ok:
        blockers.append("run_correlation_not_proven")
    callsite_seen = "first_loader_callsite" in event_names
    if "bootstrap_start" not in event_names:
        blockers.append("bootstrap_worker_started_not_observed")
    if not callsite_seen:
        blockers.append("callsite_execution_not_observed")
    hand2_rows = [row for row in rows if row.get("event") == "first_hand2_request"]
    target_rows = [row for row in rows if row.get("event") == "first_tag1_hand2_request"]
    target_calls = [row for row in rows if row.get("event") == "tag1_hand2_loader_call"]
    archive_rows = [row for row in rows if row.get("event") == "archive_open"]
    archive_rows += [row for row in file_rows if row.get("event") == "file_open" and
                     str(row.get("requested", "")).replace("\\", "/").lower().endswith("asset/asset")]
    archive_rows.sort(key=lambda row: row.get("seq", 0))
    route_rows = [row for row in rows if row.get("event") == "route_activation"]
    enabled_route_rows = [row for row in route_rows if row.get("flags", 0) & 1 and
                          row.get("text") in ("enabled", "route_enabled")]
    attach_hook_requested = metadata.get("early_hook_install_attempted",
                                         metadata.get("experimental_early_hook_requested"))
    attach_hook_status = metadata.get("experimental_early_hook_status")
    attach_hook_installed = metadata.get("early_hook_installed")
    if attach_hook_installed is None and attach_hook_status is not None:
        attach_hook_installed = attach_hook_status in ("installed_dormant", "already_installed")
    early_event_installed = bool(early_events and
        early_events[0].get("text") in ("installed_dormant", "already_installed"))
    hook_conflict = bool(early_events and attach_hook_status is not None and
        (early_event_installed != bool(attach_hook_installed) or
         (attach_hook_requested is False and early_event_installed)))
    return {
        "schema": 1,
        "run_id": run_id,
        "pid": pid,
        "process_exited": process_exited,
        "sequence_order": "shared_atomic_sequence",
        "sequence_count": len(rows),
        "sequence_min": min(sequences) if sequences else None,
        "sequence_max": max(sequences) if sequences else None,
        "dropped_events": dropped,
        "coverage_complete": complete,
        "coverage_blockers": sorted(set(blockers)),
        "instrumentation": {
            "diagnostics_requested": (instrumentation_enabled if "diagnostics_requested" in metadata else None),
            "writer_running_event": writer_running,
            "writer_failure_event": writer_failed,
            "early_hook_install_event": "early_hook_install" in event_names,
            "file_hook_install_event": "file_hook_install" in event_names,
            "bootstrap_worker_started": "bootstrap_start" in event_names,
            "first_archive_attempt_observable": "first_archive_attempt" in event_names,
            "archive_open_observable": bool(archive_rows),
            "first_intercepted_callsite": callsite_seen,
            "first_hand2_request": bool(hand2_rows),
            "first_verified_tag1_hand2_request": bool(target_rows),
            "route_activation_event": bool(route_rows),
        },
        "early_hook": {
            "requested": attach_hook_requested,
            "installed": attach_hook_installed,
            "status": attach_hook_status,
            "event_status": early_events[0].get("text") if early_events else None,
            "evidence_conflict": hook_conflict,
            "executed": callsite_seen,
        },
        "first_hand2": (hand2_rows[0] if hand2_rows else None),
        "first_tag1_hand2": (target_rows[0] if target_rows else None),
        "tag1_loader_call": (target_calls[0] if target_calls else None),
        "archive_open": (archive_rows[0] if archive_rows else None),
        "route_activation": (enabled_route_rows[-1] if enabled_route_rows else
                              route_rows[-1] if route_rows else None),
        "conclusions": {
            "hand2_request": ("observed" if hand2_rows else
                              "not_observed_during_instrumented_process" if complete and callsite_seen else
                              "unknown_incomplete_coverage"),
            "tag1_hand2_request": ("observed" if target_rows else
                                   "not_observed_during_instrumented_process" if complete and callsite_seen else
                                   "unknown_incomplete_coverage"),
            "first_tag1_request_after_archive_open": (
                target_rows[0]["seq"] > archive_rows[0]["seq"]
                if target_rows and archive_rows else None),
            "first_tag1_request_after_route_enable": (
                target_rows[0]["seq"] > enabled_route_rows[-1]["seq"]
                if target_rows and enabled_route_rows else None),
            "tag1_loader_used_replacement_extent": (
                bool(target_calls and target_calls[0].get("flags", 0) & 32)
                if target_calls else None),
            "cache_state_before_route_enable": "unknown_without_target_load_or_cache_observation",
            "replacement_timing_supported": bool(complete and target_rows and target_calls and
                enabled_route_rows and target_rows[0]["seq"] > enabled_route_rows[-1]["seq"] and
                target_calls[0]["seq"] > enabled_route_rows[-1]["seq"] and
                target_calls[0].get("flags", 0) & 32),
        },
        "observation_scope": {
            "process_exited": process_exited,
            "startup_event_stream_complete": complete,
            "intercepted_loader_execution_observed": callsite_seen,
            "intercepted_loader_observation_complete_for_this_process": bool(complete and callsite_seen),
            "gameplay_state_instrumented": False,
            "gameplay_trace_complete": False,
            "absence_conclusion_scope": "this instrumented process only; no inference about later gameplay or a fresh process",
        },
        "raw_event_timeline": rows,
    }


def live_steam_processes(proc_root: Path = Path("/proc")) -> list[int]:
    found = []
    try:
        entries = list(proc_root.iterdir())
    except OSError:
        return found
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text(errors="replace").strip().lower()
            cmdline = (entry / "cmdline").read_bytes().replace(b"\0", b" ").lower()
        except OSError:
            continue
        if comm == "steam" or b"steam.sh" in cmdline:
            found.append(int(entry.name))
    return sorted(found)


def probe_bubblewrap_isolation(sandbox: Path, home: Path,
                               bwrap: str | Path | None = None) -> dict:
    """Prove the exact namespace and mount controls used by the offline runner.

    This probe is intentionally active: it creates a private abstract Unix
    socket in the parent, then confirms a child in the isolated network
    namespace cannot connect to it. It also checks namespace identities,
    read-only host root, private /tmp and /run, the bound HOME, and that the
    sole persistent writable bind is the disposable sandbox.
    """
    executable = str(bwrap or shutil.which("bwrap") or "")
    if not executable or not Path(executable).is_file():
        raise ValueError("Bubblewrap is unavailable; cannot prove process and network isolation")
    sandbox, home = sandbox.resolve(), home.resolve()
    if not sandbox.is_dir() or not home.is_dir() or not inside(home, sandbox):
        raise ValueError("Bubblewrap probe requires an existing isolated sandbox and HOME")

    username = Path.home().name
    home_mount = f"/home/{username}"
    socket_name = "hyprlodr-probe-" + secrets.token_hex(12)
    host_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        host_socket.bind("\0" + socket_name)
        host_socket.listen(1)
        control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        control.connect("\0" + socket_name)
        accepted, _ = host_socket.accept()
        accepted.close()
        control.close()
    except OSError as error:
        host_socket.close()
        raise ValueError(f"cannot establish host IPC isolation probe: {error}") from error

    code = r'''import errno, json, os, socket, sys
expected_home = tuple(map(int, sys.argv[2:4]))
result = {}
result["network_namespace_private"] = os.stat("/proc/self/ns/net").st_ino != int(sys.argv[1])
result["pid_namespace_private"] = os.stat("/proc/self/ns/pid").st_ino != int(sys.argv[4])
mounts = open("/proc/self/mountinfo", encoding="utf-8").read().splitlines()
def mount_at(path):
    for line in mounts:
        fields = line.split()
        if len(fields) > 6 and fields[4] == path:
            return fields
    return None
root = mount_at("/")
result["host_root_read_only"] = bool(root and "ro" in root[5].split(","))
result["tmp_private_tmpfs"] = bool((m := mount_at("/tmp")) and "tmpfs" in m)
result["run_private_tmpfs"] = bool((m := mount_at("/run")) and "tmpfs" in m)
routes = open("/proc/net/route", encoding="ascii").read().splitlines()[1:]
result["no_default_network_route"] = not any(
    row.split()[1] == "00000000" for row in routes if len(row.split()) > 1)
allowed_rw_binds = {"/tmp/hyprlodr", sys.argv[5]}
ephemeral_types = {"tmpfs", "proc", "devpts", "mqueue", "devtmpfs"}
unexpected_rw = []
for line in mounts:
    fields = line.split()
    if len(fields) < 10 or "rw" not in fields[5].split(","):
        continue
    separator = fields.index("-")
    mountpoint, fstype = fields[4], fields[separator + 1]
    ephemeral_path = mountpoint in ("/dev", "/proc", "/run", "/tmp") or any(
        mountpoint.startswith(prefix + "/") for prefix in ("/dev", "/proc", "/run", "/tmp"))
    if mountpoint in allowed_rw_binds or (ephemeral_path and fstype in ephemeral_types):
        continue
    unexpected_rw.append(mountpoint)
result["persistent_writes_confined"] = not unexpected_rw
result["unexpected_writable_mounts"] = unexpected_rw
home_stat = os.stat(sys.argv[5])
bound_home_stat = os.stat(sys.argv[6])
result["home_bound_to_disposable"] = ((home_stat.st_dev, home_stat.st_ino) == expected_home and
    (bound_home_stat.st_dev, bound_home_stat.st_ino) == expected_home)
try:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(0.5)
    client.connect("\0" + sys.argv[7])
    result["host_abstract_ipc_unreachable"] = False
except OSError as error:
    result["host_abstract_ipc_unreachable"] = error.errno in (errno.ENOENT, errno.ECONNREFUSED)
try:
    with open("/var/tmp/.hyprlodr-readonly-probe", "xb"):
        pass
    os.unlink("/var/tmp/.hyprlodr-readonly-probe")
    result["host_filesystem_read_only"] = False
except OSError as error:
    result["host_filesystem_read_only"] = error.errno == errno.EROFS
probe = sys.argv[8]
with open(probe, "w", encoding="ascii") as stream:
    stream.write("sandbox writable\n")
result["sandbox_writable"] = open(probe, encoding="ascii").read().strip() == "sandbox writable"
print(json.dumps(result, sort_keys=True))
'''
    host_net = os.stat("/proc/self/ns/net").st_ino
    host_pid = os.stat("/proc/self/ns/pid").st_ino
    home_stat = home.stat()
    probe_file = sandbox / "isolation-probe.txt"
    bwrap_args = [executable, "--die-with-parent", "--new-session",
        "--unshare-net", "--unshare-pid", "--ro-bind", "/", "/",
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/run",
        "--tmpfs", "/tmp", "--dir", "/tmp/hyprlodr",
        "--bind", str(sandbox), "/tmp/hyprlodr",
        "--bind", str(home), home_mount, "--", sys.executable, "-c", code,
        str(host_net), str(home_stat.st_dev), str(home_stat.st_ino), str(host_pid),
        home_mount, "/tmp/hyprlodr/" + str(home.relative_to(sandbox)),
        socket_name, "/tmp/hyprlodr/isolation-probe.txt"]
    try:
        completed = subprocess.run(bwrap_args, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=15, close_fds=True, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"Bubblewrap isolation probe failed: {error}") from error
    finally:
        host_socket.close()
        probe_file.unlink(missing_ok=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ValueError(f"Bubblewrap isolation probe exited {completed.returncode}: {detail}")
    try:
        evidence = json.loads(completed.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError) as error:
        raise ValueError("Bubblewrap isolation probe returned no machine-readable evidence") from error
    required = ("network_namespace_private", "no_default_network_route",
        "pid_namespace_private", "host_root_read_only", "tmp_private_tmpfs",
        "run_private_tmpfs", "persistent_writes_confined", "home_bound_to_disposable",
        "host_abstract_ipc_unreachable", "host_filesystem_read_only", "sandbox_writable")
    failed = [name for name in required if evidence.get(name) is not True]
    if failed:
        raise ValueError("Bubblewrap isolation guarantees not proven: " + ", ".join(failed))
    return {"verified": True, "mechanism": "bubblewrap_private_pid_network_mount_namespaces",
            "checks": evidence}


def inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def reject_external_links(paths: dict[str, Path], sandbox: Path,
                          allowed_symlink_root: Path | None = None) -> None:
    roots = list(paths.items())
    for index, (left_name, left) in enumerate(roots):
        for right_name, right in roots[index + 1:]:
            if left == right or inside(left, right) or inside(right, left):
                raise ValueError(f"writable roots overlap: {left_name} and {right_name}")
        if left.is_symlink():
            raise ValueError(f"{left_name} must not be a symlink: {left}")
        for child in [left, *left.rglob("*")] if left.exists() else []:
            if child.is_symlink():
                if allowed_symlink_root is None or \
                        not inside(child, allowed_symlink_root) or \
                        not child.resolve().exists() or not inside(child.resolve(), sandbox):
                    raise ValueError(f"symlink in isolated writable tree: {child}")
            if child.is_file() and child.stat().st_nlink != 1:
                raise ValueError(f"hard-linked file in isolated writable tree: {child}")
            if child.exists() and not inside(child, sandbox):
                raise ValueError(f"writable path escapes isolated sandbox: {child}")


def validate_isolated_paths(sandbox: Path, game_root: Path, compat_data: Path,
                            home: Path, temp: Path, steam_state: Path,
                            source_game: Path,
                            proc_root: Path = Path("/proc"),
                            bwrap: str | Path | None = None) -> dict:
    sandbox_real = sandbox.resolve()
    expected = ISOLATED_ROOT
    if sandbox_real != expected:
        raise ValueError(f"sandbox must be the designated disposable root: {expected}")
    paths = {"game_root": game_root, "compat_data": compat_data,
             "home": home, "temp": temp, "steam_state": steam_state}
    resolved = {}
    for name, path in paths.items():
        absolute = path.resolve()
        if absolute == sandbox_real or sandbox_real not in absolute.parents:
            raise ValueError(f"{name} is outside the disposable sandbox: {absolute}")
        if path.is_symlink():
            raise ValueError(f"{name} must not be a symlink: {path}")
        if absolute.exists():
            for child in absolute.rglob("*"):
                if child.is_symlink():
                    if not inside(child.resolve(), sandbox_real):
                        raise ValueError(f"symlink in isolated writable tree escapes sandbox: {child}")
        resolved[name] = str(absolute)
    reject_external_links({name: Path(value) for name, value in resolved.items()}, sandbox_real)
    source = source_game.resolve()
    copied_game = Path(resolved["game_root"])
    if source == copied_game or source in copied_game.parents or copied_game in source.parents:
        raise ValueError("test game copy must be distinct from the installed game tree")
    if not (copied_game / "hyperdemon.exe").is_file():
        raise ValueError("isolated game root must contain hyperdemon.exe")
    steam = live_steam_processes(proc_root)
    isolation = probe_bubblewrap_isolation(sandbox_real, Path(resolved["home"]), bwrap)
    return {"schema": 1, "safe": True, "sandbox": str(sandbox_real),
            "paths": resolved, "source_game": str(source),
            "active_steam_pids": steam,
            "isolation": isolation,
            "steam_client_ipc": "blocked by private network namespace and private runtime mounts",
            "steam_cloud": "blocked: game process has no client IPC or network route"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    coverage = sub.add_parser("coverage")
    coverage.add_argument("--startup", type=Path, required=True)
    coverage.add_argument("--files", type=Path, required=True)
    coverage.add_argument("--attach", type=Path, required=True)
    coverage.add_argument("--output", type=Path, required=True)
    coverage.add_argument("--process-exited", action="store_true")
    preflight = sub.add_parser("preflight")
    for name in ("sandbox", "game-root", "compat-data", "home", "temp",
                 "steam-state", "source-game"):
        preflight.add_argument("--" + name, type=Path, required=True)
    preflight.add_argument("--proc-root", type=Path, default=Path("/proc"))
    preflight.add_argument("--bwrap", type=Path, default=Path(shutil.which("bwrap") or ""))
    preflight.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "coverage":
            result = build_coverage(args.startup, args.files, args.attach,
                                    args.process_exited)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        else:
            result = validate_isolated_paths(args.sandbox, args.game_root,
                args.compat_data, args.home, args.temp, args.steam_state,
                args.source_game,
                args.proc_root, args.bwrap)
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        if args.command == "preflight" and args.output:
            failure = {"schema": 1, "safe": False,
                "launch_prevented": True, "reason": str(error),
                "sandbox": str(args.sandbox.resolve()),
                "requested_paths": {"game_root": str(args.game_root.resolve()),
                    "compat_data": str(args.compat_data.resolve()),
                    "home": str(args.home.resolve()), "temp": str(args.temp.resolve()),
                    "steam_state": str(args.steam_state.resolve()),
                    "source_game": str(args.source_game.resolve())},
                "active_steam_pids": live_steam_processes(args.proc_root)}
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(failure, indent=2) + "\n",
                                   encoding="utf-8")
            print(json.dumps(failure, indent=2))
            return 2
        print(f"first-load preflight/report failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
