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


def _validate_proton_tree(source: Path) -> None:
    """Require Proton's links to remain within the copied runtime tree."""
    for path in source.rglob("*"):
        if path.is_symlink():
            target = path.resolve()
            if not target.exists() or not inside(target, source):
                raise ValueError(f"Proton runtime symlink escapes or is broken: {path}")
        elif not path.is_dir() and not path.is_file():
            raise ValueError(f"Proton runtime contains a special file: {path}")


def stage(source_game: Path, proton_root: Path, proxy: Path | None,
          original_openal: Path, run_root: Path, app_id: str = APP_ID,
          input_probe: Path | None = None, mode: str = "diagnostic") -> dict:
    """Create a byte-verified game/runtime copy without mutating source paths."""
    source_game, proton_root = source_game.resolve(), proton_root.resolve()
    proxy = proxy.resolve() if proxy is not None else None
    original_openal = original_openal.resolve()
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
    if mode not in {"diagnostic", "vanilla"}:
        raise ValueError(f"unsupported staging mode: {mode}")
    if mode == "vanilla" and (proxy is not None or input_probe is not None):
        raise ValueError("vanilla mode cannot include a HYPRLODR proxy or input probe")
    for path, label in ((proton_root / "proton", "Proton launcher"),
                        (original_openal, "original OpenAL runtime")):
        if not path.is_file():
            raise ValueError(f"{label} is missing: {path}")
    proxy_identity = None
    identity_path = None
    diagnostic_mod = None
    revision = None
    if mode == "diagnostic":
        if proxy is None:
            raise ValueError("diagnostic mode requires a HYPRLODR proxy")
        proxy = proxy.resolve()
        if not proxy.is_file():
            raise ValueError(f"HYPRLODR proxy is missing: {proxy}")
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
        diagnostic_mod = proxy.parent / "mods/z.foundation.dll"
        if not diagnostic_mod.is_file() or \
                sha256(diagnostic_mod) != proxy_identity.get("foundation_diagnostic_mod_sha256"):
            raise ValueError("repository diagnostic mod does not match the clean proxy build identity")
    _validate_source_tree(source_game)
    proton_fixups = proton_root / "steampipe_fixups.json"
    proton_stamp = proton_root / "files/steampipe_fixups_mtime"
    if proton_fixups.exists() and proton_stamp.exists() and \
            proton_stamp.read_text(encoding="utf-8").strip() != str(proton_fixups.stat().st_mtime):
        raise ValueError("installed Proton has pending fixups; refusing to run a read-only runtime")
    _validate_proton_tree(proton_root)

    run_root.parent.mkdir(parents=True, exist_ok=True)
    run_root.mkdir()
    game = run_root / "game"
    shutil.copytree(source_game, game, ignore=_ignore, copy_function=shutil.copy2)
    if mode == "diagnostic":
        (game / "OpenAL32.dll").write_bytes(proxy.read_bytes())
        shutil.copy2(original_openal, game / "OpenAL32_real.dll")
        (game / "mods").mkdir(exist_ok=True)
        shutil.copy2(diagnostic_mod, game / "mods/z.foundation.dll")
        (game / "hyprlodr-config.json").write_text('{"delay_ms":0}\n', encoding="utf-8")
    else:
        shutil.copy2(original_openal, game / "OpenAL32.dll")
    if input_probe:
        probe = input_probe.resolve()
        if not probe.is_file():
            raise ValueError(f"Win32 input probe is missing: {probe}")
        shutil.copy2(probe, game / "hyprlodr-input-probe.exe")

    for relative in ("compatdata", "home", "xdg/config", "xdg/cache", "xdg/data",
                     "xdg/state", "runtime", "temp", "steam-client", "logs", "evidence"):
        (run_root / relative).mkdir(parents=True, exist_ok=True)
    shutil.copytree(proton_root, run_root / "proton", symlinks=True,
                    copy_function=shutil.copy2,
                    ignore=shutil.ignore_patterns("user_settings.py"))
    copied_proton = run_root / "proton"
    _validate_proton_tree(copied_proton)
    exe_hash = sha256(source_game / "hyperdemon.exe")
    archive_hash = sha256(source_game / "asset/asset")
    if sha256(game / "hyperdemon.exe") != exe_hash or \
            sha256(game / "asset/asset") != archive_hash:
        raise RuntimeError("copied executable/archive verification failed")
    if mode == "diagnostic":
        if sha256(game / "OpenAL32.dll") != sha256(proxy):
            raise RuntimeError("proxy copy verification failed")
        if sha256(game / "OpenAL32_real.dll") != sha256(original_openal):
            raise RuntimeError("original OpenAL copy verification failed")
    elif sha256(game / "OpenAL32.dll") != EXPECTED_OPENAL:
        raise RuntimeError("vanilla OpenAL copy verification failed")
    meta = {"schema": 1, "mode": mode, "hyprlodr_present": mode == "diagnostic",
        "app_id": app_id, "source_game": str(source_game),
        "game_copy": str(game), "proton_root": str(copied_proton),
        "source_proton_root": str(proton_root),
        "proton_launcher_sha256": sha256(copied_proton / "proton"),
        "source_proton_launcher_sha256": sha256(proton_root / "proton"),
        "proton_runtime_isolated_copy": True,
        "source_executable_sha256": exe_hash, "source_archive_sha256": archive_hash,
        "source_installed_openal_sha256": sha256(source_game / "OpenAL32.dll"),
        "source_openal_runtime_path": str(original_openal),
        "source_openal_runtime_sha256": sha256(original_openal),
        "copied_executable_sha256": sha256(game / "hyperdemon.exe"),
        "copied_archive_sha256": sha256(game / "asset/asset"),
        "staged_openal_sha256": sha256(game / "OpenAL32.dll"),
        "replacement_routing": False, "user_mods_copied": False,
        "excluded_source_entries": sorted(IGNORED_NAMES | RUNTIME_NAMES)}
    if mode == "diagnostic":
        meta.update({"proxy_build_manifest": str(identity_path),
            "proxy_sha256": sha256(game / "OpenAL32.dll"),
            "diagnostic_mod_sha256": sha256(game / "mods/z.foundation.dll"),
            "diagnostic_mod_id": "z.foundation",
            "diagnostic_mod_source": "repository sample; host state/capability/log only",
            "original_openal_sha256": sha256(game / "OpenAL32_real.dll"),
            "proxy_source_revision": proxy_identity["source_revision"]})
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
    source_proton = Path(meta["source_proton_root"])
    mode = meta.get("mode", "diagnostic")
    if mode not in {"diagnostic", "vanilla"}:
        raise ValueError("stage manifest has an unknown launch mode")
    proxy_identity = None
    source_revision, source_clean = current_source_identity()
    if mode == "diagnostic":
        proxy_identity = json.loads(Path(meta["proxy_build_manifest"]).read_text(encoding="utf-8"))
    roots = {name: run_root / name for name in
             ("game", "compatdata", "home", "temp", "steam-client", "proton")}
    strict_roots = {name: path for name, path in roots.items() if name != "proton"}
    reject_external_links(strict_roots, run_root)
    reject_external_links({"proton": roots["proton"]}, run_root,
                          allowed_symlink_root=roots["proton"])
    reject_external_links({"complete_run_tree": run_root}, run_root,
                          allowed_symlink_root=roots["proton"])
    for path in roots.values():
        path.mkdir(parents=True, exist_ok=True)
    if source_game == game or source_game in game.parents or game in source_game.parents:
        raise ValueError("copied game path is not distinct from installed game")
    checks = {"source_executable_unchanged": sha256(source_game / "hyperdemon.exe") == EXPECTED_EXE,
        "game_executable_matches_stage": sha256(game / "hyperdemon.exe") == meta["copied_executable_sha256"],
        "game_archive_matches_verified_original": sha256(game / "asset/asset") == EXPECTED_ARCHIVE and
            meta.get("copied_archive_sha256") == EXPECTED_ARCHIVE,
        "stage_mode_is_explicit": meta.get("mode") == mode and
            meta.get("hyprlodr_present") is (mode == "diagnostic"),
        "routing_disabled": meta.get("replacement_routing") is False,
        "proton_readable": (proton / "proton").is_file(),
        "proton_is_disposable_copy": proton.resolve() == roots["proton"].resolve() and
            meta.get("proton_runtime_isolated_copy") is True,
        "proton_source_launcher_unchanged": sha256(source_proton / "proton") ==
            meta.get("source_proton_launcher_sha256"),
        "proton_copy_launcher_matches_stage": sha256(proton / "proton") ==
            meta.get("proton_launcher_sha256"),
        "fresh_compatdata_prefix": not any(roots["compatdata"].iterdir()),
        "run_not_previously_claimed": (
            not (run_root / "launch.claimed").exists() if not allow_claimed else
            (run_root / "launch.claimed").read_text(encoding="ascii").splitlines()[0] ==
            f"pid={os.getpid()}"),
        "run_directory_separate": game != source_game}
    if mode == "diagnostic":
        checks.update({
            "proxy_matches_build_identity": sha256(game / "OpenAL32.dll") ==
                proxy_identity.get("proxy_sha256") and
                meta.get("proxy_sha256") == proxy_identity.get("proxy_sha256"),
            "diagnostic_mod_matches_build_identity":
                sha256(game / "mods/z.foundation.dll") ==
                proxy_identity.get("foundation_diagnostic_mod_sha256") ==
                meta.get("diagnostic_mod_sha256") and meta.get("diagnostic_mod_id") == "z.foundation",
            "proxy_built_from_current_clean_source": source_clean and
                proxy_identity.get("source_clean") is True and
                proxy_identity.get("source_revision") == source_revision == meta.get("proxy_source_revision")})
    else:
        hyprlodr_artifacts = [path.relative_to(game).as_posix() for path in game.rglob("*")
                              if path.name == "mods" or path.name.startswith("hyprlodr-") or
                              path.name == "hyprlodr-config.json" or
                              path.name == "OpenAL32_real.dll"]
        checks.update({
            "vanilla_original_openal_staged": sha256(game / "OpenAL32.dll") == EXPECTED_OPENAL and
                meta.get("staged_openal_sha256") == EXPECTED_OPENAL,
            "vanilla_hyprlodr_artifacts_absent": not hyprlodr_artifacts,
            "vanilla_manifest_declares_absence": meta.get("hyprlodr_present") is False and
                meta.get("replacement_routing") is False and meta.get("user_mods_copied") is False})
    original_archive_hash = sha256(source_game / "asset/asset")
    checks["source_archive_unchanged"] = original_archive_hash == meta["source_archive_sha256"]
    checks["source_installed_openal_unchanged"] = sha256(source_game / "OpenAL32.dll") == \
        meta["source_installed_openal_sha256"]
    checks["source_openal_runtime_unchanged"] = sha256(Path(meta["source_openal_runtime_path"])) == \
        meta["source_openal_runtime_sha256"] == EXPECTED_OPENAL
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
        "experiment_mode": mode,
        "hyprlodr_absent": mode == "vanilla",
        "writable_host_roots": [str(run_root)],
        "game_process_will_be_network_isolated": True}


_SUPERVISOR = r'''import json, os, re, select, shutil, signal, subprocess, sys, time
root, proton, game, run_seconds, run_mode = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
evidence = os.path.join(root, "evidence")
logs = os.path.join(root, "logs")
for path in (evidence, logs): os.makedirs(path, exist_ok=True)
events_path = os.path.join(evidence, "runtime-events.jsonl")
process_path = os.path.join(evidence, "process-lifecycle.jsonl")
window_path = os.path.join(evidence, "window-lifecycle.jsonl")
sequence = 0
start = time.monotonic()
supervisor_run_id = "%x-%x" % (os.getpid(), time.monotonic_ns())
processes = {}
game_seen = set()
supervisor_signals = {}
def event(name, **fields):
    global sequence
    sequence += 1
    row = {"seq": sequence, "run_id": supervisor_run_id,
        "t_ms": int((time.monotonic()-start)*1000), "event": name, **fields}
    with open(events_path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(row)+"\n"); stream.flush()
    print(json.dumps(row), flush=True)
    if name.startswith("process_") or name in ("proton_wrapper_exit", "process_tree_complete"):
        with open(process_path, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row)+"\n"); stream.flush()
    if name.startswith("window_") or name.startswith("display_inventory") or name == "screen_capture":
        with open(window_path, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row)+"\n"); stream.flush()

ticks = os.sysconf("SC_CLK_TCK")
boot_time = 0
try:
    with open("/proc/stat", encoding="ascii") as stream:
        boot_time = int(next(line.split()[1] for line in stream if line.startswith("btime ")))
except Exception: pass
def scan_processes():
    found = {}
    try: entries = os.listdir("/proc")
    except OSError: return found
    for entry in entries:
        if not entry.isdigit(): continue
        pid = int(entry); base = "/proc/" + entry
        try:
            raw = open(base + "/stat", "rb").read().decode("utf-8", "replace")
            end = raw.rfind(")")
            fields = raw[end+2:].split()
            state, ppid, start_ticks = fields[0], int(fields[1]), int(fields[19])
            argv = open(base + "/cmdline", "rb").read().split(b"\0")
            argv = [item.decode("utf-8", "replace") for item in argv if item]
            cmdline = " ".join(argv)
            exe = os.readlink(base + "/exe")
            comm = open(base + "/comm", "rb").read().decode("utf-8", "replace").strip()
        except (OSError, ValueError, IndexError): continue
        role_args = argv if len(argv) < 2 or argv[1] != "-c" else argv[:1]
        roles = []
        proton_real = os.path.realpath(proton)
        is_proton = os.path.realpath(exe) == proton_real or any(
            os.path.realpath(arg) == proton_real for arg in argv[:3])
        steam_images = ("steam.exe", "steamservice", "steamservice.exe",
                        "steamclient", "steamclient.exe")
        image_names = {comm.lower(), os.path.basename(exe).lower()}
        image_names.update(os.path.basename(arg.replace("\\", "/")).lower() for arg in argv[:2])
        is_steam_compat = bool(image_names.intersection(steam_images))
        target_arg = any(arg.lower().replace("/", "\\").endswith("\\hyperdemon.exe") and
                         "\\game\\" in arg.lower().replace("/", "\\") for arg in argv)
        # Wine normally exposes the Windows image name through /proc/<pid>/comm
        # while /proc/<pid>/exe remains wine-preloader or a Unix helper. Keep
        # command-line matching as a fallback for Wine builds that preserve it.
        is_game = not is_proton and not is_steam_compat and \
            (comm.lower() == "hyperdemon.exe" or target_arg)
        if is_game: roles.append("hyperdemon")
        if any(arg.lower().replace("\\", "/").endswith("/xalia.exe") for arg in role_args): roles.append("xalia")
        if "wineserver" in os.path.basename(exe).lower(): roles.append("wineserver")
        if is_steam_compat: roles.append("steam_compat")
        if is_proton: roles.append("proton")
        if "wine" in os.path.basename(exe).lower() or "wine" in role_args[0].lower(): roles.append("wine")
        if not roles: continue
        key = str(pid) + ":" + str(start_ticks)
        found[key] = {"pid":pid,"ppid":ppid,"state":state,"start_ticks":start_ticks,
            "start_epoch_s":boot_time + start_ticks / ticks if boot_time else None,
            "exe":exe,"comm":comm,"cmdline":cmdline,"roles":sorted(set(roles)),"game_process":is_game}
    return found

def observe_processes(now_ms):
    current = scan_processes()
    for key, old in list(processes.items()):
        if key not in current:
            event("process_disappeared", identity=key, pid=old["pid"], ppid=old["ppid"],
                roles=old["roles"], last_state=old["state"], first_seen_ms=old["first_seen_ms"],
                last_seen_ms=old["last_seen_ms"], exit_code=None, exit_signal=None,
                exit_observation="not_available_from_procfs",
                supervisor_terminated=supervisor_signals.get(key, False))
            del processes[key]
    for key, sample in current.items():
        if key not in processes:
            sample["first_seen_ms"] = now_ms
            sample["last_seen_ms"] = now_ms
            processes[key] = sample
            event("process_started_observed", identity=key, **sample)
        else:
            tracked = processes[key]
            tracked.update({"state":sample["state"],"last_seen_ms":now_ms,
                            "ppid":sample["ppid"],"cmdline":sample["cmdline"],"exe":sample["exe"],
                            "comm":sample["comm"]})
        if sample["game_process"]: game_seen.add(key)
    active_games = {key: value for key,value in processes.items() if value["game_process"]}
    return current, active_games

def proc_identity_alive(key):
    try:
        pid_text, ticks_text = key.split(":", 1)
        raw = open("/proc/"+pid_text+"/stat", "rb").read().decode("utf-8", "replace")
        return int(raw[raw.rfind(")")+2:].split()[19]) == int(ticks_text)
    except (OSError, ValueError, IndexError): return False

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
probe = None
for _ in range(100):
    probe = subprocess.run(["/usr/bin/xdpyinfo", "-display", display], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if probe.returncode == 0: break
    if xvfb.poll() is not None: break
    time.sleep(0.05)
if probe is None or probe.returncode != 0:
    event("xvfb_readiness_failed", display=display)
    xvfb.terminate(); xvfb.wait(timeout=5); sys.exit(21)
event("xvfb_ready", display=display, pid=xvfb.pid, resolution="1920x1080x24", glx=True, randr=True)
wm = None
wm_log = None
openbox = shutil.which("openbox")
if openbox:
    wm_log = open(os.path.join(logs, "window-manager.log"), "wb")
    wm = subprocess.Popen([openbox, "--sm-disable"], env=env, stdout=wm_log,
        stderr=subprocess.STDOUT, close_fds=True)
    event("window_manager_started", pid=wm.pid)

def window_inventory():
    xwininfo = shutil.which("xwininfo")
    wmctrl = shutil.which("wmctrl")
    if not xwininfo: return None, []
    try:
        root_result = subprocess.run([xwininfo,"-display",display,"-root"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
        result = subprocess.run([wmctrl,"-l","-p","-G"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
    except subprocess.TimeoutExpired:
        return {"returncode":None,"stderr":"xwininfo_timeout"}, []
    rows=[]
    clients=[]
    if wmctrl and result.returncode == 0:
        for line in result.stdout.splitlines():
            match=re.match(r"^(0x[0-9a-fA-F]+)\s+(-?\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+\S+\s+(.*)$",line)
            if match:
                wid,pid,x,y,width,height,desktop,title=match.groups()
                clients.append((wid,int(pid) if pid.isdigit() else None,title,
                    int(x),int(y),int(width),int(height)))
    else:
        # xwininfo's root tree indents direct children by two spaces and
        # nested toolkit children more deeply; keep only actual root clients.
        tree=subprocess.run([xwininfo,"-display",display,"-root","-tree"],env=env,
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=3,check=False)
        for line in tree.stdout.splitlines():
            match=re.match(r"^  (0x[0-9a-fA-F]+)\s+(.+)$",line)
            if match: clients.append((match.group(1),None,match.group(2),None,None,None,None))
        result=tree
    for wid,wm_pid,wm_title,x,y,width,height in clients:
        try:
            prop=subprocess.run(["xprop","-display",display,"-id",wid,"WM_CLASS","_NET_WM_PID","_NET_WM_NAME","WM_NAME"],
                env=env,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,timeout=2,check=False)
        except subprocess.TimeoutExpired: continue
        info=prop.stdout.strip()
        try:
            detail=subprocess.run([xwininfo,"-display",display,"-id",wid],env=env,
                stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,timeout=2,check=False)
        except subprocess.TimeoutExpired: continue
        visible="IsViewable" in detail.stdout
        def xint(label):
            found=re.search(r"^\s*"+label+r":\s*(-?\d+)",detail.stdout,re.M)
            return int(found.group(1)) if found else None
        pid_match=re.search(r"_NET_WM_PID\(CARDINAL\)\s*=\s*(\d+)",info)
        x_pid=int(pid_match.group(1)) if pid_match else wm_pid
        title=(wm_title or "")
        rows.append({"id":wid,"title":title,"properties":info,"x_pid":x_pid,
            "visible":visible,"x":x if x is not None else xint("Absolute upper-left X"),
            "y":y if y is not None else xint("Absolute upper-left Y"),
            "width":width if width is not None else xint("Width"),
            "height":height if height is not None else xint("Height"),
            "game_candidate":("hyperdemon" in (title+" "+info).lower())})
    return {"returncode":result.returncode,"stderr":result.stderr.strip(),
        "root":root_result.stdout.strip()}, rows

def capture_screen(reason, max_captures=6):
    if capture_screen.count >= max_captures: return
    xwd=shutil.which("xwd")
    if not xwd:
        event("screen_capture",reason=reason,ok=False,error="xwd_unavailable"); return
    path=os.path.join(evidence,"screen-%02d.xwd"%capture_screen.count)
    shot=subprocess.run([xwd,"-display",display,"-root","-silent","-out",path],env=env,
        stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=5,check=False)
    capture_screen.count += 1
    event("screen_capture",reason=reason,path=path,ok=shot.returncode==0,
        error=shot.stderr.decode("utf-8","replace") if shot.returncode else "")
capture_screen.count=0

seen_windows={}
next_window_scan=0.0
fallback_capture_at=time.monotonic()+10
next_fallback_capture=fallback_capture_at
attach_paths=set()
def observe_attach():
    for path in sorted(__import__("glob").glob(os.path.join(game,"hyprlodr-attach-*.json"))):
        if path in attach_paths: continue
        attach_paths.add(path)
        try: row=json.load(open(path,encoding="utf-8"))
        except Exception as error:
            event("hyprlodr_attach_unreadable",path=path,error=str(error)); continue
        event("hyprlodr_attach_observed",path=path,hyprlodr_run_id=row.get("run_id"),
            windows_pid=row.get("pid"),stage=row.get("stage"),build_matches=row.get("build_matches"),
            anchors_total=row.get("anchors_total"),anchors_matched=row.get("anchors_matched"),
            early_hook=row.get("experimental_early_hook_status"),
            relevant_linux_processes=[p for p in processes.values() if p["game_process"]])

proton_log = open(os.path.join(logs, "proton-console.log"), "wb")
env["WINEDEBUG"] = "+file,+seh" if run_mode == "vanilla" else "+seh"
proc = subprocess.Popen([proton, "run", os.path.join(game, "hyperdemon.exe")], cwd=game,
    env=env, stdin=subprocess.DEVNULL, stdout=proton_log, stderr=subprocess.STDOUT,
    close_fds=True, start_new_session=True)
wrapper_started=time.monotonic()
event("proton_game_started", pid=proc.pid, ppid=os.getpid(), start_monotonic_ns=time.monotonic_ns(),
    app_id=os.environ.get("STEAM_COMPAT_APP_ID"),game=game,
    compat_data=os.environ.get("STEAM_COMPAT_DATA_PATH"),
    steam_client_ipc="unavailable",network="isolated",stdin="closed_noninteractive")
stop_at = wrapper_started + run_seconds
wrapper_exit_at=None
wrapper_exit_code=None
wrapper_outlived_game=False
window_identities=set()
termination_reason=None
scan_period=0.1
next_capture = wrapper_started + 15
last_window_report=None
try:
    while True:
        now=time.monotonic(); now_ms=int((now-start)*1000)
        current, active_games=observe_processes(now_ms)
        observe_attach()
        if now >= next_window_scan:
            display_result, rows=window_inventory()
            if display_result is not None:
                new={row["id"]:row for row in rows}
                for wid,row in new.items():
                    if wid not in seen_windows:
                        associated=[p for p in processes.values() if row["x_pid"] and p["pid"]==row["x_pid"]]
                        row["associated_processes"]=[{"pid":p["pid"],"roles":p["roles"],
                            "identity":str(p["pid"])+":"+str(p["start_ticks"])} for p in associated]
                        linked_game=any(p["game_process"] for p in associated)
                        row["game_process_associated"] = linked_game
                        event("window_created",window=row,display=display)
                        if row["game_candidate"] or linked_game: capture_screen("candidate_game_window")
                for wid,row in seen_windows.items():
                    if wid not in new: event("window_disappeared",window=row,display=display)
                seen_windows=new
                inventory_row={"display":display,"root":display_result,"windows":rows}
                if inventory_row != last_window_report:
                    event("display_inventory",**inventory_row)
                    last_window_report=inventory_row
            next_window_scan=now+0.5
        if now >= next_capture:
            event("supervisor_heartbeat",proton_pid=proc.pid,proton_running=proc.poll() is None,
                active_game_identities=sorted(active_games),elapsed_seconds=int(now-start))
            next_capture += 15
        if now >= next_fallback_capture:
            capture_screen("bounded_fallback"); next_fallback_capture += 30

        code=proc.poll()
        if code is not None and wrapper_exit_at is None:
            wrapper_exit_at=now; wrapper_exit_code=code
            event("proton_wrapper_exit",pid=proc.pid,returncode=code,
                exit_kind="signal" if code<0 else "status",signal=-code if code<0 else None,
                active_game_identities=sorted(active_games))
            wrapper_outlived_game=bool(active_games)
            if wrapper_outlived_game:
                event("proton_wrapper_returned_before_game",proton_pid=proc.pid,
                    active_game_identities=sorted(active_games))
        if wrapper_exit_at is not None and game_seen and not active_games:
            termination_reason="observed_game_process_exit"; break
        if wrapper_exit_at is not None and not game_seen and now-wrapper_exit_at>=5.0:
            termination_reason="no_game_process_observed_after_wrapper_grace"; break
        if now >= stop_at:
            termination_reason="run_limit"; break
        time.sleep(scan_period)

    # Preserve the final visible state before any cleanup can close Wine windows.
    capture_screen("pre_cleanup_final")
    display_result, rows=window_inventory()
    if display_result is not None:
        event("display_inventory_final",display=display,root=display_result,windows=rows)
    event("process_tree_complete",reason=termination_reason,proton_pid=proc.pid,
        proton_returncode=proc.poll(),game_processes_ever_observed=sorted(game_seen),
        game_process_active_at_wrapper_exit=wrapper_outlived_game,
        supervisor_termination_required=termination_reason=="run_limit")

    if termination_reason=="run_limit":
        current, active_games=observe_processes(int((time.monotonic()-start)*1000))
        for key,p in list(active_games.items()):
            if not proc_identity_alive(key): continue
            try:
                os.kill(p["pid"],signal.SIGTERM); supervisor_signals[key]=True
                event("process_supervisor_signal",identity=key,pid=p["pid"],roles=p["roles"],signal="SIGTERM")
            except ProcessLookupError: pass
        term_deadline=time.monotonic()+5
        while time.monotonic()<term_deadline:
            _,active_games=observe_processes(int((time.monotonic()-start)*1000))
            if not active_games: break
            time.sleep(scan_period)
        for key,p in list(active_games.items()):
            if proc_identity_alive(key):
                try:
                    os.kill(p["pid"],signal.SIGKILL); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],roles=p["roles"],signal="SIGKILL")
                except ProcessLookupError: pass
    elif proc.poll() is None:
        # This can occur only if the overall limit elapsed before Proton returned.
        event("process_supervisor_signal",identity=str(proc.pid),pid=proc.pid,signal="SIGTERM",role="proton")
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            event("process_supervisor_signal",identity=str(proc.pid),pid=proc.pid,signal="SIGKILL",role="proton")
            proc.kill(); proc.wait(timeout=2)
    event("game_exit_observation",wrapper_returncode=wrapper_exit_code,
        game_exit_code=None,game_exit_status="unknown_unless_waitable_child",
        reason=termination_reason,supervisor_terminated_game=termination_reason=="run_limit")
finally:
    # Wrapper return alone is never a cleanup condition while the game remains.
    # wineserver -k is scoped by the private WINEPREFIX and runs only after the
    # observed game process has exited, or after explicit run-limit cleanup.
    wineserver=os.path.join(proton,"files/bin/wineserver")
    if proc.poll() is None or any(p["game_process"] for p in processes.values()):
        try:
            _, active_games=observe_processes(int((time.monotonic()-start)*1000))
            for key,p in active_games.items():
                if proc_identity_alive(key):
                    os.kill(p["pid"],signal.SIGTERM); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],
                        roles=p["roles"],signal="SIGTERM",reason="supervisor_cleanup")
            grace=time.monotonic()+2
            while time.monotonic()<grace:
                _,active_games=observe_processes(int((time.monotonic()-start)*1000))
                if not active_games: break
                time.sleep(scan_period)
            for key,p in active_games.items():
                if proc_identity_alive(key):
                    os.kill(p["pid"],signal.SIGKILL); supervisor_signals[key]=True
                    event("process_supervisor_signal",identity=key,pid=p["pid"],
                        roles=p["roles"],signal="SIGKILL",reason="supervisor_cleanup")
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=2)
        except Exception as error: event("owned_process_cleanup_failed",error=str(error))
    if os.path.isfile(wineserver):
        try:
            result=subprocess.run([wineserver,"-k"],env=env,stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,timeout=10,check=False)
            event("wineserver_cleanup",returncode=result.returncode,after_game_observation=True)
        except Exception as error: event("wineserver_cleanup_failed",error=str(error))
    capture_screen("final_before_display_shutdown")
    display_result,rows=window_inventory()
    if display_result is not None: event("display_inventory_cleanup",display=display,root=display_result,windows=rows)
    if wm is not None and wm.poll() is None: wm.terminate()
    if wm is not None:
        try: wm.wait(timeout=5)
        except subprocess.TimeoutExpired: wm.kill(); wm.wait(timeout=2)
    if wm_log is not None: wm_log.close()
    if xvfb.poll() is None: xvfb.terminate()
    try: xvfb.wait(timeout=5)
    except subprocess.TimeoutExpired: xvfb.kill(); xvfb.wait(timeout=2)
    proton_log.close(); xvfb_log.close()
    event("owned_runtime_stopped",xvfb_returncode=xvfb.returncode,
        process_poll_interval_ms=int(scan_period*1000),supervisor_run_id=supervisor_run_id)
    summary={"schema":1,"supervisor_run_id":supervisor_run_id,"proton_pid":proc.pid,
        "proton_returncode":proc.poll(),"proton_outlived_by_game":wrapper_outlived_game,
        "game_processes_ever_observed":sorted(game_seen),"termination_reason":termination_reason,
        "game_exit_code":None,"game_exit_code_available":False,
        "supervisor_terminated_game":termination_reason=="run_limit",
        "process_poll_interval_ms":int(scan_period*1000),
        "window_inventory_path":window_path,"process_events_path":process_path}
    with open(os.path.join(evidence,"process-lifecycle.json"),"w",encoding="utf-8") as stream:
        json.dump(summary,stream,indent=2);stream.write("\n")
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
        "--bind", str(run_root / "home"), user_home_target,
        "--", sys.executable, "-c", _SUPERVISOR,
        "/tmp/hyprlodr", "/tmp/hyprlodr/proton/proton", "/tmp/hyprlodr/game",
        str(duration), meta.get("mode", "diagnostic")]
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
        "PROTON_LOG": "1",
        # The control has no in-process file hook. Request Wine's file channel
        # as supplemental evidence; missing channel output stays unknown.
        "WINEDEBUG": "+file" if meta.get("mode") == "vanilla" else "-all"}
    if meta.get("mode") == "diagnostic":
        env.update({"HYPRLODR_EARLY_RECORD_HOOK": "1",
                    "HYPRLODR_FIRST_LOAD_DIAGNOSTICS": "1"})
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
                        sha256(source_game / "asset/asset") == meta["source_archive_sha256"],
                    "host_game_openal_files_unchanged":
                        sha256(source_game / "OpenAL32.dll") == meta["source_installed_openal_sha256"] and
                        sha256(Path(meta["source_openal_runtime_path"])) ==
                        meta["source_openal_runtime_sha256"] == EXPECTED_OPENAL,
                    "source_proton_launcher_unchanged":
                        sha256(Path(meta["source_proton_root"]) / "proton") ==
                        meta["source_proton_launcher_sha256"]},
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
    staging.add_argument("--proxy", type=Path)
    staging.add_argument("--original-openal", type=Path, required=True)
    staging.add_argument("--mode", choices=("diagnostic", "vanilla"), default="diagnostic")
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
                           args.original_openal, run_root, args.app_id, args.input_probe,
                           args.mode)
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
