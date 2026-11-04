from pathlib import Path

import pytest

from tools.re.proton_input import (build_command, find_game_process,
                                   proton_root_from_wineserver, resolve_runtime,
                                   root_from_config_info, windows_basename)


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
              {"WINEPREFIX": str(app / "pfx"), "STEAM_COMPAT_DATA_PATH": str(app)},
              root / "files/bin/wine")
    result = resolve_runtime(steam, proc_root=proc)
    assert result["active_game"] is True
    assert result["game_pid"] == 101
    assert result["selection"] == "active HYPER DEMON Proton process"
    assert result["proton_root"] == str(root)


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
              {"WINEPREFIX": str(app / "pfx")}, active_root / "files/bin/wine")
    with pytest.raises(ValueError, match="belongs to"):
        resolve_runtime(steam, proton_override=requested_root, proc_root=proc)


def test_command_uses_proton_and_repository_tmp(tmp_path):
    root = create_proton(tmp_path / "Proton")
    runtime = {"proton": str(root / "proton"), "proton_root": str(root),
               "compat_data": str(tmp_path / "compatdata"),
               "wineprefix": str(tmp_path / "compatdata/pfx"),
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
               "wineprefix": str(tmp_path / "compatdata/pfx"),
               "steam_root": str(tmp_path / "Steam"), "app_id": "1743850"}
    command, _ = build_command(runtime, tmp_path / "probe.exe", ["click-enter"],
                               tmp_path / ".tmp", "HYPRLODR input fixture",
                               "hyprlodr_input_fixture.exe", "INPUT OK",
                               tmp_path / "events.log")
    assert command[-5:] == ["click-enter", "--title=HYPRLODR input fixture",
                            "--module=hyprlodr_input_fixture.exe",
                            "--expect-title=INPUT OK",
                            "--report=Z:" + str(tmp_path / "events.log").replace("/", "\\")]

