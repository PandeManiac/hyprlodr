#!/usr/bin/env python3
"""Stage and run a passive HYPER DEMON diagnostic in an offline Proton sandbox.

The copied game, compatibility prefix, HOME, temporary files, logs and captures
live below ``.tmp/first-load-diagnostic/isolated``. Bubblewrap exposes the
installed Proton runtime read-only while hiding the host HOME, /run and /tmp,
making the host root read-only, and removing network and host IPC access.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SANDBOX = (ROOT / ".tmp/first-load-diagnostic/isolated").resolve()
EXPECTED_EXE = "7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611"
EXPECTED_ARCHIVE = "2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7"
EXPECTED_OPENAL = "396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4"
APP_ID = "1743850"
IGNORED_NAMES = {"mods", "hyprlodr-state", "OST"}
RUNTIME_PREFIXES = ("hyprlodr-",)
RUNTIME_NAMES = {"berds.log", "steamuser.log"}


def inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def current_source_identity() -> tuple[str, bool]:
    revision = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                       text=True).strip()
    clean = not subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"],
                                         text=True).strip()
    return revision, clean


def inventory(root: Path) -> dict[str, dict[str, Any]]:
    rows = {}
    if not root.exists():
        return rows
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        info = path.lstat()
        if path.is_symlink():
            rows[relative] = {"type": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            rows[relative] = {"type": "file", "size": info.st_size,
                              "sha256": sha256(path), "links": info.st_nlink}
        elif path.is_dir():
            rows[relative] = {"type": "directory"}
        else:
            rows[relative] = {"type": "special"}
    return rows


def _ignore(directory: str, names: list[str]) -> set[str]:
    ignored = set()
    for name in names:
        if name in IGNORED_NAMES or name in RUNTIME_NAMES or name.startswith(RUNTIME_PREFIXES):
            ignored.add(name)
        elif name in {"OpenAL32.dll", "OpenAL32_real.dll"} or name.startswith("crash_"):
            ignored.add(name)
        elif name.endswith((".bmp", ".bin")) and name.startswith(("hyprlodr-", "crash_")):
            ignored.add(name)
    return ignored


def _validate_source_tree(source: Path) -> None:
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"source game tree contains a symlink: {path}")
        if not path.is_dir() and not path.is_file():
            raise ValueError(f"source game tree contains a special file: {path}")


def stage(source_game: Path, proton_root: Path, proxy: Path,
          original_openal: Path, run_root: Path, app_id: str = APP_ID,
          input_probe: Path | None = None) -> dict:
    """Create a byte-verified, mod-free runtime copy; never mutate source paths."""
    source_game, proton_root = source_game.resolve(), proton_root.resolve()
    proxy, original_openal = proxy.resolve(), original_openal.resolve()
    run_root = run_root.resolve()
    if run_root == SANDBOX or SANDBOX not in run_root.parents:
        raise ValueError(f"run directory must be a new child of {SANDBOX}")
    if run_root.exists():
        raise ValueError(f"refusing to overwrite an existing run directory: {run_root}")
    if app_id != APP_ID:
        raise ValueError(f"unexpected Steam app ID for supported HYPER DEMON: {app_id}")
    if not (source_game / "hyperdemon.exe").is_file() or \
            sha256(source_game / "hyperdemon.exe") != EXPECTED_EXE:
        raise ValueError("source game executable does not match the verified supported build")
    if not (source_game / "asset/asset").is_file():
        raise ValueError("source game archive is missing")
    if sha256(source_game / "asset/asset") != EXPECTED_ARCHIVE:
        raise ValueError("source game archive does not match the verified original archive")
    if sha256(original_openal) != EXPECTED_OPENAL:
        raise ValueError("original OpenAL runtime does not match the verified shipped library")
    for path, label in ((proton_root / "proton", "Proton launcher"),
                        (proxy, "HYPRLODR proxy"), (original_openal, "original OpenAL runtime")):
        if not path.is_file():
            raise ValueError(f"{label} is missing: {path}")
    identity_path = proxy.parent / "hyprlodr-proxy-build.json"
    try:
        proxy_identity = json.loads(identity_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("proxy build identity manifest is missing or malformed") from error
    revision, clean = current_source_identity()
    if proxy_identity.get("schema") != 1 or proxy_identity.get("source_revision") != revision or \
            proxy_identity.get("source_clean") is not True or not clean:
        raise ValueError("proxy must be built from the current clean committed source")
    if proxy_identity.get("proxy_sha256") != sha256(proxy):
        raise ValueError("proxy artifact hash does not match its build identity")
    required_caps = {"redirect-wait-v1", "build-identity-v1"}
    if not required_caps.issubset(set(proxy_identity.get("capabilities", []))):
        raise ValueError("proxy lacks required archive redirect and build identity capabilities")
    _validate_source_tree(source_game)
    proton_fixups = proton_root / "steampipe_fixups.json"
    proton_stamp = proton_root / "files/steampipe_fixups_mtime"
    if proton_fixups.exists() and proton_stamp.exists() and \
            proton_stamp.read_text(encoding="utf-8").strip() != str(proton_fixups.stat().st_mtime):
        raise ValueError("installed Proton has pending fixups; refusing to run a read-only runtime")

    run_root.parent.mkdir(parents=True, exist_ok=True)
    run_root.mkdir()
    game = run_root / "game"
    shutil.copytree(source_game, game, ignore=_ignore, copy_function=shutil.copy2)
    (game / "OpenAL32.dll").write_bytes(proxy.read_bytes())
    shutil.copy2(original_openal, game / "OpenAL32_real.dll")
    (game / "mods").mkdir(exist_ok=True)
    (game / "hyprlodr-config.json").write_text('{"delay_ms":0}\n', encoding="utf-8")
    if input_probe:
        probe = input_probe.resolve()
        if not probe.is_file():
            raise ValueError(f"Win32 input probe is missing: {probe}")
        shutil.copy2(probe, game / "hyprlodr-input-probe.exe")

    for relative in ("compatdata", "home", "xdg/config", "xdg/cache", "xdg/data",
                     "xdg/state", "runtime", "temp", "steam-client", "logs", "evidence"):
        (run_root / relative).mkdir(parents=True, exist_ok=True)
    (run_root / "proton").mkdir()
    exe_hash = sha256(source_game / "hyperdemon.exe")
    archive_hash = sha256(source_game / "asset/asset")
    if sha256(game / "hyperdemon.exe") != exe_hash or \
            sha256(game / "asset/asset") != archive_hash:
        raise RuntimeError("copied executable/archive verification failed")
    if sha256(game / "OpenAL32.dll") != sha256(proxy):
        raise RuntimeError("proxy copy verification failed")
    if sha256(game / "OpenAL32_real.dll") != sha256(original_openal):
        raise RuntimeError("original OpenAL copy verification failed")
    meta = {"schema": 1, "app_id": app_id, "source_game": str(source_game),
        "game_copy": str(game), "proton_root": str(proton_root),
        "proxy_build_manifest": str(identity_path),
        "source_executable_sha256": exe_hash, "source_archive_sha256": archive_hash,
        "copied_executable_sha256": sha256(game / "hyperdemon.exe"),
        "copied_archive_sha256": sha256(game / "asset/asset"),
        "proxy_sha256": sha256(game / "OpenAL32.dll"),
        "original_openal_sha256": sha256(game / "OpenAL32_real.dll"),
        "proxy_source_revision": proxy_identity["source_revision"],
        "replacement_routing": False, "mods_copied": False,
        "excluded_source_entries": sorted(IGNORED_NAMES | RUNTIME_NAMES)}
    (run_root / "stage.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return meta


def check_runtime(run_root: Path, source_game: Path, bwrap: str | Path | None = None,
                  proc_root: Path = Path("/proc"), allow_claimed: bool = False) -> dict:
    try:
        from .first_load_diagnostics import (
            live_steam_processes, probe_bubblewrap_isolation, reject_external_links)
    except ImportError:
        from first_load_diagnostics import (
            live_steam_processes, probe_bubblewrap_isolation, reject_external_links)

    run_root, source_game = run_root.resolve(), source_game.resolve()
    if run_root == SANDBOX or SANDBOX not in run_root.parents:
        raise ValueError(f"run directory must be a child of {SANDBOX}")
    meta_path = run_root / "stage.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    game = run_root / "game"
    proton = Path(meta["proton_root"])
    proxy_identity = json.loads(Path(meta["proxy_build_manifest"]).read_text(encoding="utf-8"))
    source_revision, source_clean = current_source_identity()
    roots = {name: run_root / name for name in ("game", "compatdata", "home", "temp", "steam-client")}
    reject_external_links(roots, run_root)
    reject_external_links({"complete_run_tree": run_root}, run_root)
    for path in roots.values():
        path.mkdir(parents=True, exist_ok=True)
    if source_game == game or source_game in game.parents or game in source_game.parents:
        raise ValueError("copied game path is not distinct from installed game")
    checks = {"source_executable_unchanged": sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE,
        "game_executable_matches_stage": sha256(game / "hyperdemon.exe") == meta["copied_executable_sha256"],
        "game_archive_matches_verified_original": sha256(game / "asset/asset") == EXPECTED_ARCHIVE and
            meta.get("copied_archive_sha256") == EXPECTED_ARCHIVE,
        "proxy_matches_build_identity": sha256(game / "OpenAL32.dll") == proxy_identity.get("proxy_sha256") and
            meta.get("proxy_sha256") == proxy_identity.get("proxy_sha256"),
        "proxy_built_from_current_clean_source": source_clean and
            proxy_identity.get("source_clean") is True and
            proxy_identity.get("source_revision") == source_revision == meta.get("proxy_source_revision"),
        "routing_disabled": meta.get("replacement_routing") is False,
        "proton_readable": (proton / "proton").is_file(),
        "fresh_compatdata_prefix": not any(roots["compatdata"].iterdir()),
        "run_not_previously_claimed": (
            not (run_root / "launch.claimed").exists() if not allow_claimed else
            (run_root / "launch.claimed").read_text(encoding="ascii").splitlines()[0] ==
            f"pid={os.getpid()}"),
        "run_directory_separate": game != source_game}
    original_archive_hash = sha256(source_game / "asset/asset")
    checks["source_archive_unchanged"] = original_archive_hash == meta["source_archive_sha256"]
    if not all(checks.values()):
        failed = [name for name, passed in checks.items() if not passed]
        raise ValueError("isolated Proton preflight failed: " + ", ".join(failed))
    isolation = probe_bubblewrap_isolation(run_root, roots["home"], bwrap)
    processes = live_steam_processes(proc_root)
    return {"schema": 1, "safe": True, "run_root": str(run_root),
        "checks": checks, "bubblewrap": isolation,
        "active_steam_pids_observed": processes,
        "active_steam_policy": "recorded; inaccessible via isolated home, runtime, mount and network namespaces",
        "cloud": "blocked by no client IPC and no network route",
        "authentication": "not supplied; game may reject offline direct Proton startup",
        "writable_host_roots": [str(run_root)],
        "game_process_will_be_network_isolated": True}


_SUPERVISOR = r'''import json, os, select, shutil, signal, subprocess, sys, time
root, proton, game, run_seconds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
evidence = os.path.join(root, "evidence")
logs = os.path.join(root, "logs")
for path in (evidence, logs): os.makedirs(path, exist_ok=True)
events_path = os.path.join(evidence, "runtime-events.jsonl")
sequence = 0
start = time.monotonic()
def event(name, **fields):
    global sequence
    sequence += 1
    row = {"seq": sequence, "t_ms": int((time.monotonic()-start)*1000), "event": name, **fields}
    with open(events_path, "a", encoding="utf-8") as stream: stream.write(json.dumps(row)+"\n")
    print(json.dumps(row), flush=True)
read_fd, write_fd = os.pipe()
xvfb_log = open(os.path.join(logs, "xvfb.log"), "wb")
xvfb = subprocess.Popen(["/usr/bin/Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1920x1080x24",
    "+extension", "GLX", "+extension", "RANDR", "+render", "-noreset", "-nolisten", "tcp", "-ac"],
    pass_fds=(write_fd,), stdout=xvfb_log, stderr=subprocess.STDOUT, close_fds=True)
os.close(write_fd)
display = None
ready_deadline = time.monotonic() + 10
buffer = b""
while time.monotonic() < ready_deadline and xvfb.poll() is None:
    ready, _, _ = select.select([read_fd], [], [], 0.1)
    if ready:
        buffer += os.read(read_fd, 64)
        if b"\n" in buffer:
            value = buffer.splitlines()[0].decode().strip()
            if value.isdigit(): display = ":" + value
            break
os.close(read_fd)
if not display:
    event("xvfb_start_failed", returncode=xvfb.poll())
    if xvfb.poll() is None: xvfb.terminate(); xvfb.wait(timeout=5)
    sys.exit(20)
env = os.environ.copy()
env.update({"DISPLAY": display, "XAUTHORITY": "/dev/null"})
env.pop("WAYLAND_DISPLAY", None)
for _ in range(100):
    probe = subprocess.run(["/usr/bin/xdpyinfo", "-display", display], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if probe.returncode == 0: break
    if xvfb.poll() is not None: break
    time.sleep(0.05)
if probe.returncode != 0:
    event("xvfb_readiness_failed", display=display)
    xvfb.terminate(); xvfb.wait(timeout=5); sys.exit(21)
event("xvfb_ready", display=display, pid=xvfb.pid, resolution="1920x1080x24", glx=True, randr=True)
wm = None
openbox = shutil.which("openbox")
if openbox:
    wm_log = open(os.path.join(logs, "window-manager.log"), "wb")
    wm = subprocess.Popen([openbox, "--sm-disable"], env=env, stdout=wm_log,
        stderr=subprocess.STDOUT, close_fds=True)
    event("window_manager_started", pid=wm.pid)
proton_log = open(os.path.join(logs, "proton-console.log"), "wb")
proc = subprocess.Popen([proton, "run", os.path.join(game, "hyperdemon.exe")], cwd=game,
    env=env, stdin=subprocess.DEVNULL, stdout=proton_log, stderr=subprocess.STDOUT,
    close_fds=True, start_new_session=True)
event("proton_game_started", pid=proc.pid, app_id=os.environ.get("STEAM_COMPAT_APP_ID"),
    game=game, compat_data=os.environ.get("STEAM_COMPAT_DATA_PATH"),
    steam_client_ipc="unavailable", network="isolated")
next_capture = time.monotonic() + 15
stop_at = time.monotonic() + run_seconds
stdin_open = True
try:
    while proc.poll() is None and time.monotonic() < stop_at:
        ready, _, _ = select.select([sys.stdin] if stdin_open else [], [], [], 0.25)
        if ready:
            command = sys.stdin.readline().strip().lower()
            if not command: stdin_open = False
            if command in ("snapshot", "enter", "click-enter"):
                image_path = os.path.join(evidence, "screen-" + str(int(time.monotonic()-start)) + ".xwd")
                xwd = shutil.which("xwd")
                if xwd:
                    shot = subprocess.run([xwd, "-display", display, "-root", "-silent", "-out", image_path],
                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                    event("screen_capture", path=image_path, ok=shot.returncode == 0)
                if command != "snapshot":
                    probe_path = os.path.join(game, "hyprlodr-input-probe.exe")
                    if not os.path.isfile(probe_path):
                        event("input_rejected", reason="existing Win32 probe not staged")
                    else:
                        action = "enter" if command == "enter" else "click-enter"
                        report = os.path.join(evidence, "input-report.jsonl")
                        report_win = "Z:" + report.replace("/", "\\")
                        args = [proton, "run", probe_path, action, "--title=hyperdemon",
                            "--module=hyperdemon.exe", "--report=" + report_win]
                        result = subprocess.run(args, cwd=game, env=env, stdout=proton_log,
                            stderr=subprocess.STDOUT, timeout=30, check=False)
                        event("win32_input_helper", action=action, returncode=result.returncode,
                            report=report)
            elif command in ("quit", "stop"):
                event("requested_stop")
                break
        if time.monotonic() >= next_capture:
            event("game_still_running", pid=proc.pid, elapsed_seconds=int(time.monotonic()-start))
            next_capture += 15
    if proc.poll() is None:
        event("game_stop", reason="runtime_limit_or_operator")
        os.killpg(proc.pid, signal.SIGTERM)
        try: proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
    event("game_exit", returncode=proc.returncode)
finally:
    try:
        subprocess.run([os.path.join(proton, "files/bin/wineserver"), "-k"], env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10, check=False)
    except Exception: pass
    if wm is not None and wm.poll() is None: wm.terminate()
    if wm is not None:
        try: wm.wait(timeout=5)
        except subprocess.TimeoutExpired: wm.kill(); wm.wait(timeout=2)
    if wm is not None: wm_log.close()
    if xvfb.poll() is None: xvfb.terminate()
    try: xvfb.wait(timeout=5)
    except subprocess.TimeoutExpired: xvfb.kill(); xvfb.wait(timeout=2)
    proton_log.close(); xvfb_log.close()
    event("owned_runtime_stopped", xvfb_returncode=xvfb.returncode)
'''


def launch(run_root: Path, source_game: Path, duration: int = 150,
           bwrap: str | Path | None = None) -> int:
    if run_root.is_symlink():
        raise ValueError("refusing a symlinked run directory")
    run_root, source_game = run_root.resolve(), source_game.resolve()
    guard = run_root / "launch.claimed"
    try:
        descriptor = os.open(guard, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as error:
        raise ValueError("this disposable run has already been claimed; stage a fresh run") from error
    with os.fdopen(descriptor, "w", encoding="ascii") as stream:
        stream.write(f"pid={os.getpid()}\nclaimed_ns={time.time_ns()}\n")
    try:
        return _launch_claimed(run_root, source_game, duration, bwrap)
    except Exception as error:
        evidence = run_root / "evidence"
        if not evidence.is_symlink() and inside(evidence, run_root):
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / "launch-failure.json").write_text(json.dumps({
                "schema": 1, "launch_prevented_or_failed": True,
                "reason": f"{type(error).__name__}: {error}",
                "run_claimed": True, "retry_requires_fresh_stage": True}, indent=2) + "\n",
                encoding="utf-8")
        raise


def _launch_claimed(run_root: Path, source_game: Path, duration: int,
                    bwrap: str | Path | None) -> int:
    report = check_runtime(run_root, source_game, bwrap, allow_claimed=True)
    bwrap_bin = str(bwrap or shutil.which("bwrap"))
    meta = json.loads((run_root / "stage.json").read_text(encoding="utf-8"))
    proton = Path(meta["proton_root"]).resolve()
    if duration <= 0 or duration > 600:
        raise ValueError("runtime duration must be between 1 and 600 seconds")
    if not (run_root / "steam-client").is_dir():
        raise ValueError("isolated Steam client placeholder is missing")
    current_home = Path.home().resolve()
    if not inside(current_home, Path("/home")):
        raise ValueError("HOME is not under /home; cannot safely mask the host profile")
    user_home_target = f"/home/{current_home.name}"
    args = [bwrap_bin, "--die-with-parent", "--new-session", "--unshare-net", "--unshare-pid",
        "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
        "--tmpfs", "/run", "--tmpfs", "/tmp", "--dir", "/tmp/hyprlodr",
        "--bind", str(run_root), "/tmp/hyprlodr",
        "--ro-bind", str(proton), "/tmp/hyprlodr/proton",
        "--bind", str(run_root / "home"), user_home_target,
        "--", sys.executable, "-c", _SUPERVISOR,
        "/tmp/hyprlodr", "/tmp/hyprlodr/proton/proton", "/tmp/hyprlodr/game", str(duration)]
    env = {"PATH": "/usr/bin:/bin", "HOME": "/tmp/hyprlodr/home",
        "USER": os.environ.get("USER", current_home.name),
        "LOGNAME": os.environ.get("LOGNAME", current_home.name),
        "LANG": os.environ.get("LANG", "C.UTF-8"),
        "TMPDIR": "/tmp/hyprlodr/temp", "TMP": "/tmp/hyprlodr/temp",
        "TEMP": "/tmp/hyprlodr/temp", "XDG_CONFIG_HOME": "/tmp/hyprlodr/xdg/config",
        "XDG_CACHE_HOME": "/tmp/hyprlodr/xdg/cache", "XDG_DATA_HOME": "/tmp/hyprlodr/xdg/data",
        "XDG_STATE_HOME": "/tmp/hyprlodr/xdg/state", "XDG_RUNTIME_DIR": "/tmp/hyprlodr/runtime",
        "STEAM_COMPAT_DATA_PATH": "/tmp/hyprlodr/compatdata",
        "WINEPREFIX": "/tmp/hyprlodr/compatdata/pfx",
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": "/tmp/hyprlodr/steam-client",
        "STEAM_COMPAT_APP_ID": meta["app_id"],
        "STEAM_COMPAT_INSTALL_PATH": "/tmp/hyprlodr/game",
        "STEAM_COMPAT_TOOL_PATHS": "/tmp/hyprlodr/proton",
        "HYPRLODR_EARLY_RECORD_HOOK": "1", "HYPRLODR_FIRST_LOAD_DIAGNOSTICS": "1",
        "PROTON_LOG": "1", "WINEDEBUG": "-all"}
    args.insert(args.index("--"), "--clearenv")
    for key, value in env.items():
        args[args.index("--") : args.index("--")] = ["--setenv", key, value]
    preflight_path = run_root / "evidence/preflight.json"
    preflight_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    before = inventory(run_root)
    (run_root / "evidence/prelaunch-inventory.json").write_text(
        json.dumps(before, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (run_root / "evidence/launch-environment.json").write_text(json.dumps({
        "cwd": str(run_root / "game"), "command": ["/tmp/hyprlodr/proton/proton", "run",
        "/tmp/hyprlodr/game/hyperdemon.exe"], "environment": env,
        "sandbox": report["bubblewrap"]}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"event": "isolated_launch_preflight_passed", "preflight": report}, indent=2), flush=True)
    log_path = run_root / "logs/sandbox-supervisor-host.log"
    with log_path.open("wb") as log:
            process = subprocess.Popen(args, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1, close_fds=True,
                start_new_session=True, env={
                    "PATH": "/usr/bin:/bin", "HOME": str(run_root / "home"),
                    "TMPDIR": str(run_root / "temp"), "TMP": str(run_root / "temp"),
                    "TEMP": str(run_root / "temp"), "LANG": "C.UTF-8"})
            try:
                for line in process.stdout:
                    log.write(line.encode(errors="replace"))
                    log.flush()
                    print(line, end="", flush=True)
                code = process.wait()
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGTERM)
                try: code = process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    code = process.wait(timeout=5)
    after = inventory(run_root)
    changed = {name: {"before": before.get(name), "after": after.get(name)}
               for name in sorted(set(before) | set(after)) if before.get(name) != after.get(name)}
    (run_root / "evidence/postlaunch-inventory.json").write_text(
        json.dumps({"schema": 1, "created_or_modified": changed,
                    "deleted": sorted(set(before) - set(after)),
                    "host_game_source_unchanged":
                        sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE and
                        sha256(source_game / "asset/asset") == meta["source_archive_sha256"]},
                   indent=2, sort_keys=True) + "\n", encoding="utf-8")
    attach_files = list((run_root / "game").glob("hyprlodr-attach-*.json"))
    if attach_files:
        attach = attach_files[-1]
        startup = run_root / "game" / f"hyprlodr-startup-{attach.stem.removeprefix('hyprlodr-attach-')}.jsonl"
        files = run_root / "game" / f"hyprlodr-files-{attach.stem.removeprefix('hyprlodr-attach-')}.jsonl"
        if startup.is_file() and files.is_file():
            try:
                from .first_load_diagnostics import build_coverage
            except ImportError:
                from first_load_diagnostics import build_coverage
            coverage = build_coverage(startup, files, attach, process_exited=True)
            (run_root / "evidence/coverage-report.json").write_text(
                json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"event": "isolated_launch_finished", "returncode": code,
        "run_root": str(run_root), "inventory": str(run_root / "evidence/postlaunch-inventory.json")}, indent=2))
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    staging = sub.add_parser("stage")
    staging.add_argument("--source-game", type=Path, required=True)
    staging.add_argument("--proton", type=Path, required=True)
    staging.add_argument("--proxy", type=Path, required=True)
    staging.add_argument("--original-openal", type=Path, required=True)
    staging.add_argument("--run-id", required=True)
    staging.add_argument("--app-id", default=APP_ID)
    staging.add_argument("--input-probe", type=Path)
    preflight = sub.add_parser("preflight")
    preflight.add_argument("--run", type=Path, required=True)
    preflight.add_argument("--source-game", type=Path, required=True)
    preflight.add_argument("--bwrap", type=Path)
    launch_parser = sub.add_parser("launch")
    launch_parser.add_argument("--run", type=Path, required=True)
    launch_parser.add_argument("--source-game", type=Path, required=True)
    launch_parser.add_argument("--bwrap", type=Path)
    launch_parser.add_argument("--seconds", type=int, default=150)
    args = parser.parse_args(argv)
    try:
        if args.command == "stage":
            run_root = SANDBOX / args.run_id
            result = stage(args.source_game, args.proton, args.proxy,
                           args.original_openal, run_root, args.app_id, args.input_probe)
            print(json.dumps(result, indent=2))
        elif args.command == "preflight":
            result = check_runtime(args.run, args.source_game, args.bwrap)
            print(json.dumps(result, indent=2))
        else:
            return launch(args.run, args.source_game, args.seconds, args.bwrap)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"isolated Proton {args.command} failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
