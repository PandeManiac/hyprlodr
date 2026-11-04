from pathlib import Path
import json
import os
import subprocess

import pytest

from tools.re.proton_input import (build_command, find_game_process,
                                   process_snapshot, proton_root_from_wineserver, resolve_runtime,
                                   root_from_config_info, verify_runtime_target,
                                   windows_basename)


def create_proton(root: Path, version: str = "fixture-proton") -> Path:
    (root / "files/bin").mkdir(parents=True)
    (root / "files/bin/wine").write_text("fixture")
    (root / "files/bin/wineserver").write_text("fixture")
    (root / "proton").write_text("fixture")
    (root / "version").write_text(version)
    return root


def make_proc(root: Path, pid: str, cmdline: list[str], env: dict[str, str], exe: Path) -> None:
    process = root / pid
    process.mkdir(parents=True)
    (process / "comm").write_text(windows_basename(cmdline[0]))
    (process / "cmdline").write_bytes(b"\0".join(item.encode() for item in cmdline) + b"\0")
    (process / "environ").write_bytes(b"\0".join(
        f"{key}={value}".encode() for key, value in env.items()) + b"\0")
    (process / "exe").symlink_to(exe)


def test_windows_style_path_matches_game_executable():
    assert windows_basename(r"S:\steamapps\common\hyperdemon\hyperdemon.exe") == "hyperdemon.exe"
    assert windows_basename(r"C:\windows\system32\winedevice.exe") != "hyperdemon.exe"


def test_only_game_process_with_expected_prefix_is_selected(tmp_path):
    compat = tmp_path / "compatdata/1743850"
    compat.mkdir(parents=True)
    records = [
        {"pid": 20, "cmdline": [r"C:\Games\hyperdemon.exe"],
         "env": {"WINEPREFIX": str(tmp_path / "other/pfx")}},
        {"pid": 21, "cmdline": [r"S:\games\HYPERDEMON.EXE"],
         "env": {"WINEPREFIX": str(compat / "pfx")}},
    ]
    assert find_game_process(records, compat)["pid"] == 21


def test_ambiguous_hyper_processes_are_rejected(tmp_path):
    compat = tmp_path / "compatdata/1743850"
    compat.mkdir(parents=True)
    record = {"cmdline": [r"C:\Games\hyperdemon.exe"],
              "env": {"WINEPREFIX": str(compat / "pfx")}}
    with pytest.raises(ValueError, match="multiple HYPER DEMON processes"):
        find_game_process([{**record, "pid": 21}, {**record, "pid": 22}], compat)


def test_actual_image_process_beats_proton_and_steam_wrappers(tmp_path):
    compat = tmp_path / "compatdata/1743850"
    compat.mkdir(parents=True)
    prefix = str(compat / "pfx")
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "1", ["proton", "run", "/game/hyperdemon.exe"],
              {"WINEPREFIX": prefix}, root / "files/bin/wine")
    make_proc(proc, "2", ["steam.exe", "/game/hyperdemon.exe"],
              {"WINEPREFIX": prefix}, root / "files/bin/wine")
    make_proc(proc, "3", [r"Z:\game\hyperdemon.exe"],
              {"WINEPREFIX": prefix}, root / "files/bin/wine")
    records = process_snapshot(proc)
    assert find_game_process(records, compat)["pid"] == 3


def test_wrapper_argv_without_live_game_image_is_not_selected(tmp_path):
    compat = tmp_path / "compatdata/1743850"
    compat.mkdir(parents=True)
    prefix = str(compat / "pfx")
    records = [
        {"pid": 1, "comm": "python3", "cmdline": ["proton", "run", "/game/hyperdemon.exe"],
         "env": {"WINEPREFIX": prefix}},
        {"pid": 2, "comm": "steam.exe", "cmdline": ["steam.exe", "/game/hyperdemon.exe"],
         "env": {"WINEPREFIX": prefix}},
    ]
    assert find_game_process(records, compat) is None


def test_process_snapshot_reads_kernel_image_name(tmp_path):
    compat = tmp_path / "compatdata/1743850"
    compat.mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"Z:\game\hyperdemon.exe"],
              {"WINEPREFIX": str(compat / "pfx")}, root / "files/bin/wine")
    assert process_snapshot(proc)[0]["comm"] == "hyperdemon.exe"


def test_proton_path_is_derived_from_wineserver(tmp_path):
    root = create_proton(tmp_path / "GE Proton")
    server = root / "files/bin/wineserver"
    assert proton_root_from_wineserver(server) == root
    assert proton_root_from_wineserver(tmp_path / "system/wineserver") is None


def test_default_runtime_comes_from_prefix_config_info(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    app.mkdir(parents=True)
    root = create_proton(steam / "steamapps/common/Proton - Experimental")
    (app / "pfx").mkdir()
    (app / "config_info").write_text(
        str(root / "files/share/default_pfx") + "/\n")
    result = resolve_runtime(steam, proc_root=tmp_path / "empty-proc")
    assert result["proton_root"] == str(root)
    assert result["selection"] == "compatdata default_pfx runtime"
    assert result["wineprefix"] == str(app / "pfx")


def test_matching_active_wineserver_controls_runtime(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "GE-Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "100", ["wineserver"], {"WINEPREFIX": str(app / "pfx")},
              root / "files/bin/wineserver")
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "STEAM_COMPAT_DATA_PATH": str(app),
               "DISPLAY": ":99"},
              root / "files/bin/wine")
    result = resolve_runtime(steam, proc_root=proc)
    assert result["active_game"] is True
    assert result["game_pid"] == 101
    assert result["selection"] == "active HYPER DEMON Proton process"
    assert result["proton_root"] == str(root)


def test_hyper_display_wins_over_shell_and_other_proton_game(tmp_path, monkeypatch):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    other = steam / "steamapps/compatdata/999"
    (other / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "GE-Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "100", ["wineserver"], {"WINEPREFIX": str(app / "pfx")},
              root / "files/bin/wineserver")
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "STEAM_COMPAT_DATA_PATH": str(app),
               "DISPLAY": ":99", "XAUTHORITY": str(tmp_path / "xauth-99"),
               "HOME": str(tmp_path / "game-home")}, root / "files/bin/wine")
    make_proc(proc, "200", [r"C:\games\othergame.exe"],
              {"WINEPREFIX": str(other / "pfx"), "DISPLAY": ":0"},
              root / "files/bin/wine")
    monkeypatch.setenv("DISPLAY", ":0")
    monkeypatch.setenv("XAUTHORITY", str(tmp_path / "wrong-shell-cookie"))

    result = resolve_runtime(steam, proc_root=proc)
    assert result["game_pid"] == 101
    assert result["display"] == ":99"
    assert result["xauthority"] == str(tmp_path / "xauth-99")
    assert result["home"] == str(tmp_path / "game-home")


def test_explicit_display_and_authority_override(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99",
               "XAUTHORITY": str(tmp_path / "game.xauth")}, root / "files/bin/wine")
    result = resolve_runtime(steam, proton_override=root, proc_root=proc,
                             display_override=":77",
                             xauthority_override=tmp_path / "override.xauth")
    assert result["display"] == ":77"
    assert result["display_selection"] == "explicit override"
    assert result["xauthority"] == str(tmp_path / "override.xauth")


def test_different_display_requires_matching_authority_override(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99",
               "XAUTHORITY": str(tmp_path / "game.xauth")}, root / "files/bin/wine")
    with pytest.raises(ValueError, match="different X server"):
        resolve_runtime(steam, proton_override=root, proc_root=proc,
                        display_override=":77")
    result = resolve_runtime(steam, proton_override=root, proc_root=proc,
                             display_override=":77", xauthority_override="none")
    assert result["xauthority"] == "/dev/null"


def test_missing_game_authority_does_not_inherit_shell_cookie(tmp_path, monkeypatch):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99"}, root / "files/bin/wine")
    monkeypatch.setenv("XAUTHORITY", str(tmp_path / "unrelated-cookie"))
    result = resolve_runtime(steam, proton_override=root, proc_root=proc)
    assert result["xauthority"] == "/dev/null"
    assert "shell value ignored" in result["xauthority_selection"]


def test_game_home_default_xauthority_is_recovered(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    home = tmp_path / "game-home"
    home.mkdir()
    authority = home / ".Xauthority"
    authority.write_text("fixture-cookie")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99", "HOME": str(home)},
              root / "files/bin/wine")
    result = resolve_runtime(steam, proton_override=root, proc_root=proc)
    assert result["xauthority"] == str(authority)


def test_stale_or_missing_game_process_cannot_be_targeted(tmp_path):
    runtime = {"game_pid": 201, "wineprefix": str(tmp_path / "pfx"),
               "compat_data": str(tmp_path / "compatdata")}
    with pytest.raises(ValueError, match="stale"):
        verify_runtime_target(runtime, tmp_path / "proc")


def test_game_without_display_requires_explicit_override(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx")}, root / "files/bin/wine")
    with pytest.raises(ValueError, match="has no DISPLAY"):
        resolve_runtime(steam, proton_override=root, proc_root=proc)
    result = resolve_runtime(steam, proton_override=root, proc_root=proc,
                             display_override=":99", xauthority_override="none")
    assert result["display"] == ":99"


def test_resolve_rejects_process_that_vanished_after_snapshot(tmp_path, monkeypatch):
    import tools.re.proton_input as module

    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    root = create_proton(tmp_path / "Proton")
    proc = tmp_path / "proc"
    record = {"pid": 201, "cmdline": [r"S:\games\hyperdemon.exe"],
              "env": {"WINEPREFIX": str(app / "pfx")},
              "executable": root / "files/bin/wine"}
    monkeypatch.setattr(module, "process_snapshot", lambda _root: [record])
    with pytest.raises(ValueError, match="stale"):
        resolve_runtime(steam, proton_override=root, proc_root=proc)


def test_active_game_without_runtime_mapping_does_not_guess_from_config(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    guessed = create_proton(steam / "steamapps/common/Proton - Experimental")
    (app / "config_info").write_text(
        str(guessed / "files/share/default_pfx") + "/\n")
    proc = tmp_path / "proc"
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99"},
              guessed / "files/bin/wine")
    with pytest.raises(ValueError, match="cannot resolve the active Proton runtime"):
        resolve_runtime(steam, proc_root=proc)


def test_rejects_runtime_mismatch_with_active_target_prefix(tmp_path):
    steam = tmp_path / "Steam"
    app = steam / "steamapps/compatdata/1743850"
    (app / "pfx").mkdir(parents=True)
    active_root = create_proton(tmp_path / "active")
    requested_root = create_proton(tmp_path / "requested")
    proc = tmp_path / "proc"
    make_proc(proc, "100", ["wineserver"], {"WINEPREFIX": str(app / "pfx")},
              active_root / "files/bin/wineserver")
    make_proc(proc, "101", [r"S:\games\hyperdemon.exe"],
              {"WINEPREFIX": str(app / "pfx"), "DISPLAY": ":99"},
              active_root / "files/bin/wine")
    with pytest.raises(ValueError, match="belongs to"):
        resolve_runtime(steam, proton_override=requested_root, proc_root=proc)


def test_command_uses_proton_and_repository_tmp(tmp_path):
    root = create_proton(tmp_path / "Proton")
    runtime = {"proton": str(root / "proton"), "proton_root": str(root),
               "compat_data": str(tmp_path / "compatdata"),
               "wineprefix": str(tmp_path / "compatdata/pfx"), "display": ":99",
               "xauthority": "/dev/null",
               "steam_root": str(tmp_path / "Steam"), "app_id": "1743850"}
    probe = tmp_path / "probe.exe"
    command, env = build_command(runtime, probe, ["play"], tmp_path / ".tmp/test")
    assert command[:3] == [str(root / "proton"), "run", str(probe)]
    assert command[-1] == "play"
    assert Path(env["TMPDIR"]) == (tmp_path / ".tmp/test").resolve()
    assert env["TMP"] == env["TEMP"] == env["TMPDIR"]
    assert env["WINEPREFIX"] == runtime["wineprefix"]
    assert Path(runtime["compat_data"]).is_dir()


def test_command_can_guard_and_verify_a_test_window(tmp_path):
    root = create_proton(tmp_path / "Proton")
    runtime = {"proton": str(root / "proton"), "proton_root": str(root),
               "compat_data": str(tmp_path / "compatdata"),
               "wineprefix": str(tmp_path / "compatdata/pfx"), "display": ":99",
               "xauthority": "/dev/null",
               "steam_root": str(tmp_path / "Steam"), "app_id": "1743850"}
    command, _ = build_command(runtime, tmp_path / "probe.exe", ["click-enter"],
                               tmp_path / ".tmp", "HYPRLODR input fixture",
                               "hyprlodr_input_fixture.exe", "INPUT OK",
                               tmp_path / "events.log")
    assert command[-5:] == ["click-enter", "--title=HYPRLODR input fixture",
                            "--module=hyprlodr_input_fixture.exe",
                            "--expect-title=INPUT OK",
                            "--report=Z:" + str(tmp_path / "events.log").replace("/", "\\")]


def test_proton_child_receives_selected_game_environment(tmp_path, monkeypatch):
    root = create_proton(tmp_path / "Proton")
    proton = root / "proton"
    proton.write_text("#!/usr/bin/env python3\n"
                      "import json, os\n"
                      "with open(os.environ['PROTON_ENV_CAPTURE'], 'w') as f:\n"
                      " json.dump({k: os.environ.get(k) for k in ('DISPLAY','XAUTHORITY','HOME','WINEPREFIX')}, f)\n")
    proton.chmod(0o755)
    capture = tmp_path / "child-env.json"
    monkeypatch.setenv("DISPLAY", ":0")
    monkeypatch.setenv("XAUTHORITY", str(tmp_path / "wrong-shell-cookie"))
    monkeypatch.setenv("PROTON_ENV_CAPTURE", str(capture))
    runtime = {"proton": str(proton), "proton_root": str(root),
               "compat_data": str(tmp_path / "compatdata"),
               "wineprefix": str(tmp_path / "compatdata/pfx"),
               "steam_root": str(tmp_path / "Steam"), "app_id": "1743850",
               "display": ":99", "xauthority": str(tmp_path / "game.xauth"),
               "home": str(tmp_path / "game-home")}
    command, environment = build_command(runtime, tmp_path / "probe.exe", ["enter"],
                                         tmp_path / ".tmp")
    completed = subprocess.run(command, env=environment, check=False)
    assert completed.returncode == 0
    observed = json.loads(capture.read_text())
    assert observed == {"DISPLAY": ":99", "XAUTHORITY": str(tmp_path / "game.xauth"),
                        "HOME": str(tmp_path / "game-home"),
                        "WINEPREFIX": runtime["wineprefix"]}

