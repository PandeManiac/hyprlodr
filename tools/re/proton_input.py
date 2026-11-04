#!/usr/bin/env python3
"""Run the Win32 input probe through HYPER DEMON's matching Proton runtime.

Never invoke the host ``wine`` executable: it can speak an incompatible client
protocol to a Proton wineserver. Runtime selection is tied to the game's
compatdata prefix or an explicitly supplied Proton installation.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "1743850"
GAME_EXE = "hyperdemon.exe"


def steam_root(explicit: Path | None = None) -> Path:
    if explicit:
        return explicit.expanduser().resolve()
    for candidate in (Path.home() / ".local/share/Steam", Path.home() / ".steam/steam"):
        if candidate.is_dir():
            return candidate.resolve()
    raise ValueError("Steam installation was not found; pass --steam-root")


def read_proc(proc_root: Path, pid: str) -> tuple[list[str], dict[str, str], Path | None]:
    base = proc_root / pid
    try:
        command = [part.decode(errors="replace") for part in
                   (base / "cmdline").read_bytes().split(b"\0") if part]
    except OSError:
        command = []
    try:
        environment = {}
        for entry in (base / "environ").read_bytes().split(b"\0"):
            if b"=" in entry:
                key, value = entry.split(b"=", 1)
                environment[key.decode(errors="replace")] = value.decode(errors="replace")
    except OSError:
        environment = {}
    try:
        executable = (base / "exe").resolve()
    except OSError:
        executable = None
    return command, environment, executable


def windows_basename(argument: str) -> str:
    return argument.replace("\\", "/").rsplit("/", 1)[-1].lower()


def process_snapshot(proc_root: Path = Path("/proc")) -> list[dict]:
    records = []
    try:
        pids = [entry.name for entry in proc_root.iterdir() if entry.name.isdigit()]
    except OSError:
        return records
    for pid in pids:
        command, environment, executable = read_proc(proc_root, pid)
        records.append({"pid": int(pid), "cmdline": command, "env": environment,
                        "executable": executable})
    return records


def proton_root_from_wineserver(path: Path | None) -> Path | None:
    if path is None or path.name.lower() != "wineserver":
        return None
    if len(path.parents) < 3 or path.parent.name != "bin" or path.parent.parent.name != "files":
        return None
    root = path.parent.parent.parent
    return root if (root / "proton").is_file() else None


def find_game_process(records: list[dict], app_prefix: Path) -> dict | None:
    expected_data = app_prefix.resolve()
    expected_prefix = (app_prefix / "pfx").resolve()
    matches = []
    for record in records:
        if not any(windows_basename(arg) == GAME_EXE for arg in record["cmdline"]):
            continue
        wineprefix = record["env"].get("WINEPREFIX")
        compat_data = record["env"].get("STEAM_COMPAT_DATA_PATH")
        if wineprefix and Path(wineprefix).resolve() == expected_prefix:
            matches.append(record)
        elif compat_data and Path(compat_data).resolve() == expected_data:
            matches.append(record)
    return max(matches, key=lambda item: len(item["cmdline"])) if matches else None


def root_from_config_info(app_prefix: Path) -> Path | None:
    try:
        text = (app_prefix / "config_info").read_text(errors="replace")
    except OSError:
        return None
    marker = "/files/share/default_pfx/"
    for line in text.splitlines():
        match = re.search(r"(/[^\r\n]*?" + re.escape(marker) + r")", line)
        if match:
            candidate = Path(match.group(1)[: -len(marker.rstrip("/"))])
            if (candidate / "proton").is_file():
                return candidate.resolve()
    return None


def resolve_runtime(steam: Path, app_id: str = APP_ID,
                    proton_override: Path | None = None,
                    compat_override: Path | None = None,
                    proc_root: Path = Path("/proc")) -> dict:
    app_prefix = compat_override.expanduser().resolve() if compat_override else \
        (steam / f"steamapps/compatdata/{app_id}").resolve()
    if not compat_override and not (app_prefix / "pfx").is_dir():
        raise ValueError(f"Steam compatdata prefix is missing: {app_prefix / 'pfx'}")
    records = process_snapshot(proc_root)
    game = find_game_process(records, app_prefix)
    selected_root = proton_override.expanduser().resolve() if proton_override else None
    selection = "explicit Proton path" if selected_root else None
    prefix = (app_prefix / "pfx").resolve()
    if game:
        prefix = Path(game["env"].get("WINEPREFIX", prefix)).resolve()
        if selected_root is None:
            for record in records:
                if record["env"].get("WINEPREFIX") and \
                        Path(record["env"]["WINEPREFIX"]).resolve() == prefix:
                    root = proton_root_from_wineserver(record["executable"])
                    if root:
                        selected_root = root
                        break
        if selected_root is None:
            tool_paths = game["env"].get("STEAM_COMPAT_TOOL_PATHS", "").split(":")
            selected_root = next((Path(item).resolve() for item in tool_paths
                                  if item and (Path(item) / "proton").is_file()), None)
        if selected_root:
            selection = "active HYPER DEMON Proton process"
    if selected_root is None:
        selected_root = root_from_config_info(app_prefix)
        if selected_root:
            selection = "compatdata default_pfx runtime"
    if selected_root is None:
        candidate = steam / "steamapps/common/Proton - Experimental"
        if (candidate / "proton").is_file():
            selected_root = candidate.resolve()
            selection = "Steam Proton Experimental fallback (no active app mapping)"
    if selected_root is None or not (selected_root / "proton").is_file():
        raise ValueError("matching Proton runtime not found; pass --proton explicitly")

    for record in records:
        executable = record["executable"]
        if not executable or executable.name.lower() != "wineserver":
            continue
        server_prefix = record["env"].get("WINEPREFIX")
        if server_prefix and Path(server_prefix).resolve() == prefix:
            server_root = proton_root_from_wineserver(executable)
            if server_root and server_root.resolve() != selected_root.resolve():
                raise ValueError(f"active wineserver PID {record['pid']} for {prefix} belongs to "
                                 f"{server_root}, selected runtime is {selected_root}")
    for required in (selected_root / "files/bin/wine", selected_root / "files/bin/wineserver"):
        if not required.is_file():
            raise ValueError(f"Proton runtime is incomplete: {required} is missing")
    version_file = selected_root / "version"
    return {"app_id": app_id, "steam_root": str(steam.resolve()),
            "compat_data": str(app_prefix), "wineprefix": str(prefix),
            "proton_root": str(selected_root.resolve()),
            "proton": str((selected_root / "proton").resolve()),
            "selection": selection, "game_pid": game["pid"] if game else None,
            "game_cmdline": game["cmdline"] if game else None,
            "active_game": bool(game),
            "runtime_version": version_file.read_text().strip() if version_file.is_file() else "unknown"}


def build_command(runtime: dict, probe: Path, arguments: list[str],
                  temp_root: Path | None = None,
                  target_title: str | None = None,
                  target_module: str | None = None,
                  expect_title: str | None = None,
                  report_path: Path | None = None) -> tuple[list[str], dict[str, str]]:
    temp_root = (temp_root or ROOT / ".tmp/proton-input/temp").resolve()
    temp_root.mkdir(parents=True, exist_ok=True)
    Path(runtime["compat_data"]).mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update({"STEAM_COMPAT_DATA_PATH": runtime["compat_data"],
                "STEAM_COMPAT_CLIENT_INSTALL_PATH": runtime["steam_root"],
                "STEAM_COMPAT_APP_ID": runtime["app_id"],
                "STEAM_COMPAT_TOOL_PATHS": runtime["proton_root"],
                "WINEPREFIX": runtime["wineprefix"],
                "TMPDIR": str(temp_root), "TMP": str(temp_root), "TEMP": str(temp_root)})
    probe_args = list(arguments)
    if target_title is not None:
        probe_args.append("--title=" + target_title)
    if target_module is not None:
        probe_args.append("--module=" + target_module)
    if expect_title is not None:
        probe_args.append("--expect-title=" + expect_title)
    if report_path is not None:
        probe_args.append("--report=Z:" + str(report_path.resolve()).replace("/", "\\"))
    return [runtime["proton"], "run", str(probe.resolve()), *probe_args], env


def run_self_test(runtime: dict, probe: Path, fixture: Path) -> int:
    """Verify mouse and keyboard delivery to a disposable Win32 fixture."""
    required = ("Xvfb", "xdpyinfo", "openbox")
    missing = [name for name in required if not shutil.which(name)]
    if missing:
        raise ValueError("input self-test requires: " + ", ".join(missing))
    if not probe.is_file() or not fixture.is_file():
        raise ValueError(f"build the probe and fixture first (missing {probe if not probe.is_file() else fixture})")

    output = ROOT / ".tmp/proton-input/selftest-run"
    output.mkdir(parents=True, exist_ok=True)
    (output / "probe-events.log").unlink(missing_ok=True)
    compat_data = output / "compatdata"
    compat_data.mkdir(parents=True, exist_ok=True)
    prefix = (compat_data / "pfx").resolve()
    test_runtime = {**runtime, "compat_data": str(compat_data.resolve()),
                    "wineprefix": str(prefix)}
    for record in process_snapshot():
        if record["executable"] and record["executable"].name.lower() == "wineserver" and \
                record["env"].get("WINEPREFIX") and \
                Path(record["env"]["WINEPREFIX"]).resolve() == prefix:
            raise ValueError("disposable self-test prefix already has a running wineserver")

    display = None
    xvfb = None
    window_manager = None
    read_fd, write_fd = os.pipe()
    os.set_inheritable(write_fd, True)
    xvfb_env = os.environ.copy()
    xvfb_env.update({"TMPDIR": str(output), "TMP": str(output), "TEMP": str(output)})
    with (output / "xvfb.log").open("w") as xvfb_log:
        xvfb = subprocess.Popen([shutil.which("Xvfb"), "-displayfd", str(write_fd),
                                 "-screen", "0", "800x600x24", "-nolisten", "tcp", "-ac"],
                                env=xvfb_env, pass_fds=(write_fd,), stdout=xvfb_log,
                                stderr=subprocess.STDOUT)
    os.close(write_fd)
    ready, _, _ = select.select([read_fd], [], [], 5.0)
    if ready:
        display_number = os.read(read_fd, 32).decode(errors="replace").strip()
        if display_number.isdigit():
            display = ":" + display_number
    os.close(read_fd)
    if display is None or xvfb.poll() is not None:
        if xvfb.poll() is None:
            xvfb.terminate()
            xvfb.wait(timeout=3)
        raise ValueError("Xvfb did not reserve a display with -displayfd")
    probe_env = os.environ.copy()
    probe_env.update({"DISPLAY": display, "TMPDIR": str(output),
                      "TMP": str(output), "TEMP": str(output)})
    probe_env.pop("WAYLAND_DISPLAY", None)
    probe_env.pop("XAUTHORITY", None)
    if subprocess.run([shutil.which("xdpyinfo")], env=probe_env,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
        xvfb.terminate()
        xvfb.wait(timeout=3)
        raise ValueError("Xvfb reserved a display but did not accept X11 connections")

    fixture_process = None
    result = 1
    try:
        env = os.environ.copy()
        env.update({"DISPLAY": display, "TMPDIR": str(output), "TMP": str(output),
                    "TEMP": str(output), "STEAM_COMPAT_DATA_PATH": test_runtime["compat_data"],
                    "STEAM_COMPAT_CLIENT_INSTALL_PATH": test_runtime["steam_root"],
                    "STEAM_COMPAT_APP_ID": test_runtime["app_id"],
                    "STEAM_COMPAT_TOOL_PATHS": test_runtime["proton_root"],
                    "WINEPREFIX": str(prefix)})
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("XAUTHORITY", None)
        env["HOME"] = str(output / "home")
        env["XDG_CONFIG_HOME"] = str(output / "xdg-config")
        env["XDG_CACHE_HOME"] = str(output / "xdg-cache")
        Path(env["HOME"]).mkdir(parents=True, exist_ok=True)
        window_manager = subprocess.Popen([shutil.which("openbox"), "--sm-disable"],
                                          cwd=ROOT, env=env,
                                          stdout=(output / "window-manager.log").open("w"),
                                          stderr=subprocess.STDOUT)
        time.sleep(0.4)
        with (output / "fixture.log").open("w") as fixture_log:
            fixture_process = subprocess.Popen([test_runtime["proton"], "run", str(fixture.resolve())],
                                               cwd=ROOT, env=env, stdout=fixture_log,
                                               stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 45
            fixture_seen = False
            while time.monotonic() < deadline:
                records = process_snapshot()
                fixture_seen = any(
                    windows_basename(arg) == fixture.name.lower() and
                    record["env"].get("WINEPREFIX") and
                    Path(record["env"]["WINEPREFIX"]).resolve() == prefix
                    for record in records for arg in record["cmdline"])
                if fixture_seen:
                    break
                if fixture_process.poll() is not None:
                    break
                time.sleep(0.1)
            if not fixture_seen:
                raise ValueError("Win32 fixture never appeared in the disposable Proton prefix")
            input_command, input_env = build_command(
                test_runtime, probe, ["click-enter"], output / "temp",
                "HYPRLODR input fixture", "hyprlodr_input_fixture.exe", "INPUT OK",
                output / "probe-events.log")
            input_env.update({"DISPLAY": display, "WINEPREFIX": str(prefix),
                              "STEAM_COMPAT_DATA_PATH": str(compat_data)})
            input_env.pop("WAYLAND_DISPLAY", None)
            input_env.pop("XAUTHORITY", None)
            with (output / "probe.log").open("w") as probe_log:
                completed = subprocess.run(input_command, cwd=ROOT, env=input_env,
                                           stdout=probe_log, stderr=subprocess.STDOUT,
                                           timeout=20)
            result = completed.returncode
            (output / "result.json").write_text(json.dumps({
                "runtime": runtime, "display": display,
                "fixture_seen": fixture_seen,
                "probe_exit_code": result,
                "input_confirmed": result == 0,
                "probe_log": str(output / "probe.log"),
                "fixture_log": str(output / "fixture.log")}, indent=2) + "\n")
    finally:
        if fixture_process and fixture_process.poll() is None:
            kill_env = os.environ.copy()
            kill_env.update({"WINEPREFIX": str(prefix), "TMPDIR": str(output),
                             "TMP": str(output), "TEMP": str(output)})
            subprocess.run([str(Path(test_runtime["proton_root"]) / "files/bin/wineserver"), "-k"],
                           env=kill_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                fixture_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                fixture_process.terminate()
        if xvfb and xvfb.poll() is None:
            xvfb.terminate()
            xvfb.wait(timeout=3)
        if window_manager and window_manager.poll() is None:
            window_manager.terminate()
            window_manager.wait(timeout=3)
    print((output / "result.json").read_text() if (output / "result.json").exists()
          else json.dumps({"input_confirmed": False, "output": str(output)}, indent=2))
    return 0 if result == 0 else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-id", default=APP_ID)
    parser.add_argument("--steam-root", type=Path)
    parser.add_argument("--compat-data", type=Path,
                        help="compatdata root; pass a disposable directory for test runs")
    parser.add_argument("--proton", type=Path, help="override the selected Proton installation")
    parser.add_argument("--probe", type=Path,
                        default=ROOT / ".tmp/build-win/tools/win_input_probe.exe")
    parser.add_argument("--target-title", help="required title substring; defaults to HYPER DEMON")
    parser.add_argument("--target-module", help="required owning module; defaults to hyperdemon.exe")
    parser.add_argument("--expect-title", help="require this target title after the input")
    parser.add_argument("--report", type=Path, help="write per-window and input stages to this file")
    parser.add_argument("--self-test", action="store_true",
                        help="deliver mouse and keyboard input to a disposable Win32 fixture")
    parser.add_argument("--fixture", type=Path,
                        help="self-test fixture executable; defaults beside --probe")
    parser.add_argument("--inspect", action="store_true", help="report runtime without launching")
    parser.add_argument("--dry-run", action="store_true", help="print command without launching")
    parser.add_argument("action", nargs="?", default="play",
                        choices=("play", "center", "enter", "click-enter", "screen-test"))
    args = parser.parse_args(argv)
    try:
        steam = steam_root(args.steam_root)
        runtime = resolve_runtime(steam, args.app_id, args.proton, args.compat_data)
        if not args.inspect and not args.dry_run and not args.probe.is_file():
            raise ValueError(f"input probe executable is missing: {args.probe}")
    except (OSError, ValueError) as error:
        print(f"input preflight failed: {error}", file=sys.stderr)
        return 2
    if args.inspect:
        print(json.dumps(runtime, indent=2))
        return 0
    if args.self_test:
        fixture = args.fixture or args.probe.parent / "hyprlodr_input_fixture.exe"
        return run_self_test(runtime, args.probe, fixture)
    report_path = args.report or ROOT / ".tmp/proton-input/live-report.log"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.unlink(missing_ok=True)
    command, environment = build_command(runtime, args.probe, [args.action],
                                         target_title=args.target_title,
                                         target_module=args.target_module,
                                         expect_title=args.expect_title,
                                         report_path=report_path)
    print(json.dumps({"runtime": runtime, "command": command,
                      "tmpdir": environment["TMPDIR"]}, indent=2))
    if args.dry_run:
        return 0
    result = subprocess.run(command, env=environment, cwd=ROOT).returncode
    print(json.dumps({"probe_exit_code": result, "report_path": str(report_path),
                      "report": report_path.read_text() if report_path.is_file() else ""}, indent=2))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
