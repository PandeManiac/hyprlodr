import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

from tools.re import first_load_diagnostics as diagnostics
from tools.re import isolated_proton as isolated


def fixture_tree(tmp_path: Path, monkeypatch):
    sandbox = tmp_path / "isolated"
    monkeypatch.setattr(isolated, "SANDBOX", sandbox.resolve())
    monkeypatch.setattr(diagnostics, "ISOLATED_ROOT", sandbox.resolve())
    source = tmp_path / "installed-game"
    (source / "asset").mkdir(parents=True)
    (source / "mods").mkdir()
    (source / "OST").mkdir()
    exe = b"verified test executable"
    (source / "hyperdemon.exe").write_bytes(exe)
    (source / "asset/asset").write_bytes(b"original archive")
    (source / "mods/user-mod.dll").write_bytes(b"must not stage")
    (source / "OST/track.mp3").write_bytes(b"not needed")
    (source / "OpenAL32.dll").write_bytes(b"old proxy")
    proxy, openal = tmp_path / "proxy.dll", tmp_path / "openal-real.dll"
    proxy.write_bytes(b"new verified proxy")
    openal.write_bytes(b"original OpenAL")
    diagnostic_mod = tmp_path / "mods/z.foundation.dll"
    diagnostic_mod.parent.mkdir()
    diagnostic_mod.write_bytes(b"verified repository diagnostic plugin")
    source_revision = "fixture-source-revision"
    monkeypatch.setattr(isolated, "current_source_identity", lambda: (source_revision, True))
    (tmp_path / "hyprlodr-proxy-build.json").write_text(json.dumps({
        "schema": 1, "source_revision": source_revision, "source_clean": True,
        "proxy_sha256": hashlib.sha256(proxy.read_bytes()).hexdigest(),
        "foundation_diagnostic_mod_sha256": hashlib.sha256(diagnostic_mod.read_bytes()).hexdigest(),
        "capabilities": ["redirect-wait-v1", "build-identity-v1"]}))
    proton = tmp_path / "Proton"
    proton.mkdir()
    (proton / "proton").write_text("#!/bin/sh\nexit 0\n")
    (proton / "proton").chmod(0o755)
    (proton / "proton-alias").symlink_to("proton")
    monkeypatch.setattr(isolated, "EXPECTED_EXE", hashlib.sha256(exe).hexdigest())
    monkeypatch.setattr(isolated, "EXPECTED_ARCHIVE", hashlib.sha256(b"original archive").hexdigest())
    monkeypatch.setattr(isolated, "EXPECTED_OPENAL", hashlib.sha256(b"original OpenAL").hexdigest())
    run = sandbox / "run-fixture"
    return sandbox, source, proton, proxy, openal, run


def test_stages_game_copy_without_user_mods_and_proves_runtime_isolation(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    manifest = isolated.stage(source, proton, proxy, openal, run)
    game = run / "game"
    assert manifest["replacement_routing"] is False
    assert manifest["user_mods_copied"] is False
    assert manifest["diagnostic_mod_id"] == "z.foundation"
    assert Path(manifest["proton_root"]).is_relative_to(run)
    assert (Path(manifest["proton_root"]) / "proton-alias").is_symlink()
    assert (game / "hyperdemon.exe").read_bytes() == (source / "hyperdemon.exe").read_bytes()
    assert (game / "asset/asset").read_bytes() == (source / "asset/asset").read_bytes()
    assert (game / "OpenAL32.dll").read_bytes() == proxy.read_bytes()
    assert (game / "OpenAL32_real.dll").read_bytes() == openal.read_bytes()
    assert [path.name for path in (game / "mods").iterdir()] == ["z.foundation.dll"]
    assert (game / "mods/z.foundation.dll").read_bytes() == \
        (proxy.parent / "mods/z.foundation.dll").read_bytes()
    assert not (game / "OST").exists()
    report = isolated.check_runtime(run, source)
    assert report["safe"] is True
    assert report["game_process_will_be_network_isolated"] is True
    assert report["cloud"].startswith("blocked")
    assert report["authentication"] == "not supplied; game may reject offline direct Proton startup"
    assert report["active_steam_pids_observed"] == diagnostics.live_steam_processes()


def test_vanilla_mode_stages_only_verified_original_game_and_openal(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    before = {path.relative_to(source): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in source.rglob("*") if path.is_file()}
    manifest = isolated.stage(source, proton, None, openal, run, mode="vanilla")
    game = run / "game"
    assert manifest["mode"] == "vanilla"
    assert manifest["hyprlodr_present"] is False
    assert manifest["replacement_routing"] is False
    assert manifest["user_mods_copied"] is False
    assert manifest["source_executable_sha256"] == isolated.EXPECTED_EXE
    assert manifest["source_archive_sha256"] == isolated.EXPECTED_ARCHIVE
    assert manifest["staged_openal_sha256"] == isolated.EXPECTED_OPENAL
    assert (game / "OpenAL32.dll").read_bytes() == openal.read_bytes()
    assert not (game / "OpenAL32_real.dll").exists()
    assert not (game / "mods").exists()
    assert not list(game.glob("hyprlodr-*"))
    assert not (game / "hyprlodr-config.json").exists()
    assert {path.relative_to(source): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in source.rglob("*") if path.is_file()} == before
    report = isolated.check_runtime(run, source)
    assert report["safe"] is True
    assert report["experiment_mode"] == "vanilla"
    assert report["hyprlodr_absent"] is True
    assert report["checks"]["vanilla_hyprlodr_artifacts_absent"] is True


def test_vanilla_preflight_rejects_any_hyprlodr_artifact(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    _, source, proton, _, openal, run = fixture_tree(tmp_path, monkeypatch)
    isolated.stage(source, proton, None, openal, run, mode="vanilla")
    (run / "game/mods").mkdir()
    with pytest.raises(ValueError, match="vanilla_hyprlodr_artifacts_absent"):
        isolated.check_runtime(run, source)


def test_vanilla_launch_has_no_loader_environment_and_uses_original_openal(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap") or not __import__("shutil").which("Xvfb"):
        pytest.skip("Bubblewrap and Xvfb are required")
    sandbox, source, proton, _, openal, run = fixture_tree(tmp_path, monkeypatch)
    _set_proton_fixture(proton, "import json,os,sys\n"
        "with open(os.path.join(os.environ['HOME'],'launch-env.json'),'w') as f:\n"
        " json.dump({k:v for k,v in os.environ.items() if k.startswith('HYPRLODR_') or k=='WINEDEBUG'},f)\n"
        "sys.exit(0)\n")
    isolated.stage(source, proton, None, openal, run, mode="vanilla")
    assert isolated.launch(run, source, duration=5) == 0
    captured = json.loads((run / "home/launch-env.json").read_text())
    assert captured == {"WINEDEBUG": "+file,+seh"}
    assert (run / "game/OpenAL32.dll").read_bytes() == openal.read_bytes()
    assert not (run / "game/mods").exists()
    report = json.loads((run / "evidence/preflight.json").read_text())
    assert report["experiment_mode"] == "vanilla"
    assert report["hyprlodr_absent"] is True
    inventory = json.loads((run / "evidence/postlaunch-inventory.json").read_text())
    assert inventory["host_game_source_unchanged"] is True
    assert inventory["host_game_openal_files_unchanged"] is True
    assert (run / "launch.claimed").is_file()
    with pytest.raises(ValueError, match="already been claimed"):
        isolated.launch(run, source, duration=5)


def test_preflight_rejects_existing_run_directory_and_bad_executable(tmp_path, monkeypatch):
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    run.mkdir(parents=True)
    with pytest.raises(ValueError, match="refusing to overwrite"):
        isolated.stage(source, proton, proxy, openal, run)
    run.rmdir()
    (source / "hyperdemon.exe").write_bytes(b"unsupported")
    with pytest.raises(ValueError, match="verified supported build"):
        isolated.stage(source, proton, proxy, openal, run)


def test_staging_rejects_proton_symlink_escape(tmp_path, monkeypatch):
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    (proton / "escape").symlink_to(openal)
    with pytest.raises(ValueError, match="symlink escapes or is broken"):
        isolated.stage(source, proton, proxy, openal, run)


def test_preflight_detects_mutated_copy_before_launch(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    isolated.stage(source, proton, proxy, openal, run)
    (run / "game/asset/asset").write_bytes(b"changed")
    with pytest.raises(ValueError, match="game_archive_matches_verified_original"):
        isolated.check_runtime(run, source)


def test_bubblewrap_runner_keeps_a_stub_proton_write_inside_run_root(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    (proton / "proton").write_text(
        "#!/bin/sh\nprintf '%s\\n' \"$HOME|$STEAM_COMPAT_DATA_PATH|$DISPLAY\" > \"$HOME/proton-write.txt\"\nprintf 'private runtime write\\n' > \"$(dirname \"$0\")/dist.lock\"\nexit 0\n")
    (proton / "proton").chmod(0o755)
    isolated.stage(source, proton, proxy, openal, run)
    assert isolated.launch(run, source, duration=5) == 0
    written = (run / "home/proton-write.txt").read_text().strip()
    assert written == "/tmp/hyprlodr/home|/tmp/hyprlodr/compatdata|:0"
    inventory = json.loads((run / "evidence/postlaunch-inventory.json").read_text())
    assert inventory["host_game_source_unchanged"] is True
    assert "home/proton-write.txt" in inventory["created_or_modified"]
    assert (run / "launch.claimed").is_file()
    assert (run / "proton/dist.lock").read_text().strip() == "private runtime write"
    assert not (proton / "dist.lock").exists()
    with pytest.raises(ValueError, match="already been claimed"):
        isolated.launch(run, source, duration=5)


def test_preflight_rejects_a_reused_compatdata_prefix(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    isolated.stage(source, proton, proxy, openal, run)
    (run / "compatdata/pfx").mkdir()
    with pytest.raises(ValueError, match="fresh_compatdata_prefix"):
        isolated.check_runtime(run, source)


def _auth_fixture(tmp_path, monkeypatch):
    sandbox = (tmp_path / "isolated").resolve()
    monkeypatch.setattr(isolated, "SANDBOX", sandbox)
    monkeypatch.setattr(isolated, "AUTH_SNAPSHOT_ROOT", sandbox / "authenticated-snapshots")
    source = tmp_path / "installed"
    (source / "asset").mkdir(parents=True)
    (source / "mods").mkdir()
    (source / "hyperdemon.exe").write_bytes(b"exe")
    (source / "asset/asset").write_bytes(b"archive")
    (source / "OpenAL32.dll").write_bytes(b"openal")
    candidate = sandbox / "candidate/game"
    (candidate / "asset").mkdir(parents=True)
    (candidate / "hyperdemon.exe").write_bytes(b"exe")
    (candidate / "asset/asset").write_bytes(b"archive")
    (candidate / "OpenAL32.dll").write_bytes(b"openal")
    proton = sandbox / "candidate/proton"
    proton.mkdir()
    (proton / "proton").write_text("stub")
    (sandbox / "candidate/stage.json").write_text(json.dumps({
        "mode": "vanilla", "app_id": isolated.APP_ID, "game_copy": str(candidate),
        "proton_root": str(proton),
        "source_executable_sha256": hashlib.sha256(b"exe").hexdigest(),
        "source_archive_sha256": hashlib.sha256(b"archive").hexdigest(),
        "proton_launcher_sha256": hashlib.sha256((proton / "proton").read_bytes()).hexdigest()}))
    steam = tmp_path / "Steam"
    prefix = sandbox / "candidate/compatdata"
    user_data = tmp_path / "original-prefix/pfx/drive_c/users/steamuser/AppData/Roaming/HyperDemon"
    user_data.mkdir(parents=True)
    (user_data / "config").write_bytes(b"private game config")
    openal = tmp_path / "shipped/OpenAL32.dll"
    openal.parent.mkdir()
    openal.write_bytes(b"openal")
    monkeypatch.setattr(isolated, "EXPECTED_EXE", hashlib.sha256(b"exe").hexdigest())
    monkeypatch.setattr(isolated, "EXPECTED_ARCHIVE", hashlib.sha256(b"archive").hexdigest())
    monkeypatch.setattr(isolated, "EXPECTED_OPENAL", hashlib.sha256(b"openal").hexdigest())
    return sandbox, source, candidate, proton, steam, prefix, openal, user_data


def test_authenticated_state_snapshot_is_private_and_detects_stale_data(tmp_path, monkeypatch):
    _, source, _, _, _, _, _, user_data = _auth_fixture(tmp_path, monkeypatch)
    (source / "OpenAL32.dll").write_bytes(b"installed proxy")
    (source / "hyprlodr-install-manifest.json").write_text('{"installed":true}')
    source_before = {p.relative_to(source): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in source.rglob("*") if p.is_file()}
    user_before = (user_data / "config").read_bytes()
    snapshot = isolated.snapshot_authenticated_state(source, user_data, "auth-fixture-1")
    manifest = Path(snapshot["manifest"])
    assert snapshot["source_modified"] is False
    assert snapshot["entries"] == 3
    assert manifest.stat().st_mode & 0o777 == 0o600
    assert manifest.parent.stat().st_mode & 0o777 == 0o700
    assert isolated.verify_authenticated_snapshot(source, user_data, manifest)["valid"] is True
    assert (user_data / "config").read_bytes() == user_before
    assert {p.relative_to(source): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in source.rglob("*") if p.is_file()} == source_before
    (user_data / "config").write_bytes(b"changed")
    assert isolated.verify_authenticated_snapshot(source, user_data, manifest)["valid"] is False


def test_authenticated_snapshot_rejects_links_malformed_and_overwrite(tmp_path, monkeypatch):
    _, source, _, _, _, _, _, user_data = _auth_fixture(tmp_path, monkeypatch)
    hardlink = user_data / "linked-config"
    os.link(user_data / "config", hardlink)
    with pytest.raises(ValueError, match="linked or unsupported"):
        isolated.snapshot_authenticated_state(source, user_data, "link-case")
    hardlink.unlink()
    linked_dir = user_data / "linked-dir"
    linked_dir.symlink_to(user_data, target_is_directory=True)
    with pytest.raises(ValueError, match="linked directory"):
        isolated.snapshot_authenticated_state(source, user_data, "symlink-case")
    linked_dir.unlink()
    snapshot = isolated.snapshot_authenticated_state(source, user_data, "valid-case")
    manifest = Path(snapshot["manifest"])
    manifest.write_text("not json")
    assert isolated.verify_authenticated_snapshot(source, user_data, manifest)["reason"] == \
        "snapshot_manifest_malformed"
    with pytest.raises(ValueError, match="refusing to overwrite"):
        isolated.snapshot_authenticated_state(source, user_data, "valid-case")


def test_authenticated_preflight_is_fail_closed_and_reports_separation(tmp_path, monkeypatch):
    sandbox, source, candidate, proton, steam, prefix, openal, user_data = \
        _auth_fixture(tmp_path, monkeypatch)
    snapshot = isolated.snapshot_authenticated_state(source, user_data, "preflight-case")
    proc_root = tmp_path / "empty-proc"
    proc_root.mkdir()
    report = isolated.authenticated_preflight(
        source, candidate, proton, steam, prefix, openal, user_data,
        Path(snapshot["manifest"]), proc_root=proc_root)
    assert report["safe_to_launch"] is False
    assert report["launch_attempted"] is False
    assert report["checks"]["supported_executable"] is True
    assert report["checks"]["original_archive"] is True
    assert report["checks"]["candidate_game_copy_is_vanilla"] is True
    assert report["checks"]["candidate_prefix_is_disposable"] is True
    assert report["checks"]["snapshot_matches_game_and_user_data"] is True
    assert "steam_cloud_behavior_not_proven_disabled_for_this_app" in report["blockers"]
    assert "authenticated_steam_handoff_to_disposable_game_and_prefix_unverified" in report["blockers"]
    assert not prefix.exists()
    default_prefix = steam / "steamapps/compatdata" / isolated.APP_ID
    bad = isolated.authenticated_preflight(
        source, candidate, proton, steam, default_prefix, openal, user_data,
        Path(snapshot["manifest"]), proc_root=proc_root)
    assert bad["checks"]["candidate_prefix_is_disposable"] is False
    default_prefix.mkdir(parents=True)
    (default_prefix / "user-state").write_text("must not touch")
    bad = isolated.authenticated_preflight(
        source, candidate, proton, steam, default_prefix, openal, user_data,
        Path(snapshot["manifest"]), proc_root=proc_root)
    assert (default_prefix / "user-state").read_text() == "must not touch"
    assert bad["candidate_compatdata"] == str(default_prefix)


def test_authenticated_preflight_rejects_modified_game_and_source_aliases(tmp_path, monkeypatch):
    sandbox, source, candidate, proton, steam, prefix, openal, user_data = \
        _auth_fixture(tmp_path, monkeypatch)
    snapshot = isolated.snapshot_authenticated_state(source, user_data, "identity-case")
    (candidate / "asset/asset").write_bytes(b"modified")
    proc_root = tmp_path / "empty-proc"
    proc_root.mkdir()
    report = isolated.authenticated_preflight(
        source, candidate, proton, steam, prefix, openal, user_data,
        Path(snapshot["manifest"]), proc_root=proc_root)
    assert report["checks"]["candidate_archive_matches"] is False
    assert report["safe_to_launch"] is False
    report = isolated.authenticated_preflight(
        source, source, proton, steam, prefix, openal, user_data,
        Path(snapshot["manifest"]), proc_root=proc_root)
    assert report["checks"]["game_copy_is_separate"] is False


def test_authenticated_preflight_cli_returns_blocked_without_launching(
        tmp_path, monkeypatch, capsys):
    _, source, candidate, proton, steam, prefix, openal, user_data = \
        _auth_fixture(tmp_path, monkeypatch)
    snapshot = isolated.snapshot_authenticated_state(source, user_data, "cli-case")
    proc_root = tmp_path / "empty-proc"
    proc_root.mkdir()
    monkeypatch.setattr(isolated, "live_wine_processes", lambda root: [])
    # The Steam process scanner is imported in the command implementation; no
    # host process state is needed to test its fail-closed result.
    from tools.re import first_load_diagnostics
    monkeypatch.setattr(first_load_diagnostics, "live_steam_processes", lambda root: [])
    result = isolated.main([
        "authenticated-preflight", "--source-game", str(source), "--game-copy", str(candidate),
        "--proton", str(proton), "--steam-root", str(steam), "--compatdata", str(prefix),
        "--original-openal", str(openal), "--user-data-root", str(user_data),
        "--snapshot-manifest", snapshot["manifest"]])
    captured = json.loads(capsys.readouterr().out)
    assert result == 3
    assert captured["launch_attempted"] is False
    assert captured["safe_to_launch"] is False
    assert not prefix.exists()


def _set_proton_fixture(proton_root: Path, body: str) -> None:
    launcher = proton_root / "proton"
    launcher.write_text("#!/usr/bin/python3\n" + body, encoding="utf-8")
    launcher.chmod(0o755)


def _run_supervisor_fixture(tmp_path, monkeypatch, body: str, seconds: int = 5,
                            window_fixture: Path | None = None):
    import shutil
    if not shutil.which("bwrap") or not shutil.which("Xvfb"):
        pytest.skip("Bubblewrap and Xvfb are required for supervisor lifecycle fixtures")
    case_root = tmp_path / ("fixture-%02d" % len(list(tmp_path.iterdir())))
    case_root.mkdir()
    sandbox, source, proton, proxy, openal, run = fixture_tree(case_root, monkeypatch)
    _set_proton_fixture(proton, body)
    isolated.stage(source, proton, proxy, openal, run)
    if window_fixture:
        shutil.copy2(window_fixture, run / "game/hyprlodr-supervisor-window-fixture.exe")
    return source, proton, run, isolated.launch(run, source, duration=seconds)


def _runtime_events(run: Path):
    return [json.loads(line) for line in
            (run / "evidence/runtime-events.jsonl").read_text().splitlines()]


def test_supervisor_waits_after_proton_exits_while_game_child_lives(tmp_path, monkeypatch):
    body = ("import subprocess,sys\n"
            "child_code=\"import ctypes,time; ctypes.CDLL(None).prctl(15,b'hyperdemon.exe',0,0,0); time.sleep(1.4)\"\n"
            "subprocess.Popen([sys.executable,'-c',child_code,sys.argv[2]])\n"
            "steam_code=\"import ctypes,time; ctypes.CDLL(None).prctl(15,b'steam.exe',0,0,0); time.sleep(.7)\"\n"
            "subprocess.Popen([sys.executable,'-c',steam_code,sys.argv[2]])\n"
            "sys.exit(0)\n")
    source, proton, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, body)
    assert result == 0
    events = _runtime_events(run)
    returned = next(e for e in events if e["event"] == "proton_wrapper_exit")
    outlived = next(e for e in events if e["event"] == "proton_wrapper_returned_before_game")
    disappeared = next(e for e in events if e["event"] == "process_disappeared" and
                       "hyperdemon" in e["roles"])
    assert outlived["t_ms"] >= returned["t_ms"]
    assert disappeared["t_ms"] > returned["t_ms"]
    assert disappeared["exit_code"] is None
    started = next(e for e in events if e["event"] == "process_started_observed" and
                   "hyperdemon" in e["roles"])
    assert started["comm"] == "hyperdemon.exe"
    steam_started = next(e for e in events if e["event"] == "process_started_observed" and
                         "steam_compat" in e["roles"])
    assert "hyperdemon" not in steam_started["roles"]
    assert steam_started["game_process"] is False
    assert not any(e.get("comm") == "bwrap" and "steam_compat" in e.get("roles", [])
                   for e in events)
    summary = json.loads((run / "evidence/process-lifecycle.json").read_text())
    assert summary["proton_outlived_by_game"] is True
    assert summary["supervisor_terminated_game"] is False
    assert (run / "logs/proton-console.log").is_file()


def test_supervisor_observes_game_child_exit_before_proton_parent(tmp_path, monkeypatch):
    body = ("import subprocess,sys,time\n"
            "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(.45)',sys.argv[2]])\n"
            "child.wait(); time.sleep(.35); sys.exit(0)\n")
    _, _, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, body)
    assert result == 0
    events = _runtime_events(run)
    disappeared = next(e for e in events if e["event"] == "process_disappeared" and
                       "hyperdemon" in e["roles"])
    returned = next(e for e in events if e["event"] == "proton_wrapper_exit")
    assert disappeared["t_ms"] < returned["t_ms"]
    assert disappeared["exit_observation"] == "not_available_from_procfs"


def test_supervisor_records_crashing_game_child_without_fabricating_status(tmp_path, monkeypatch):
    body = ("import subprocess,sys\n"
            "child_code=\"import ctypes,os,time; ctypes.CDLL(None).prctl(15,b'hyperdemon.exe',0,0,0); time.sleep(.4); os.abort()\"\n"
            "child=subprocess.Popen([sys.executable,'-c',child_code,sys.argv[2]])\n"
            "child.wait(); raise SystemExit(0)\n")
    _, _, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, body)
    assert result == 0
    events = _runtime_events(run)
    disappeared = next(e for e in events if e["event"] == "process_disappeared" and
                       "hyperdemon" in e["roles"])
    assert disappeared["exit_code"] is None
    assert disappeared["exit_signal"] is None
    assert disappeared["exit_observation"] == "not_available_from_procfs"
    assert (run / "logs/proton-console.log").is_file()


def test_supervisor_captures_and_tracks_wine_window_without_stdin(tmp_path, monkeypatch):
    fixture = os.environ.get("HYPRLODR_SUPERVISOR_WINDOW_FIXTURE")
    if not fixture or not Path(fixture).is_file():
        pytest.skip("compiled Wine window fixture is required")
    body = ("import subprocess,sys\n"
            "raise SystemExit(subprocess.run(['/usr/bin/wine',"
            "'/tmp/hyprlodr/game/hyprlodr-supervisor-window-fixture.exe']).returncode)\n")
    _, _, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, body, seconds=20,
                                                window_fixture=Path(fixture))
    assert result == 0
    events = _runtime_events(run)
    process_events = [json.loads(line) for line in
                      (run / "evidence/process-lifecycle.jsonl").read_text().splitlines()]
    created = [e for e in events if e["event"] == "window_created"]
    disappeared = [e for e in events if e["event"] == "window_disappeared"]
    candidates = [e["window"] for e in created if e["window"].get("game_candidate")]
    fixture_window = next((e["window"] for e in created
        if e["window"].get("game_candidate") and
        "hyprlodr-supervisor-window-fixture.exe" in e["window"].get("properties", "").lower()),
        None)
    assert fixture_window is not None, {
        "created_windows": [e.get("window") for e in created],
        "inventories": [e for e in events if e["event"].startswith("display_inventory")],
        "processes": process_events,
    }
    assert candidates == [fixture_window], candidates
    assert fixture_window["visible"] is True
    assert fixture_window["x_pid"] > 0
    assert any(p.get("pid") == fixture_window["x_pid"] and
               "hyprlodr-supervisor-window-fixture.exe" in p.get("cmdline", "").lower()
               for p in process_events if p["event"] == "process_started_observed"), {
        "window": fixture_window, "processes": process_events,
    }
    assert any(e["window"].get("id") == fixture_window["id"] and
               "hyprlodr-supervisor-window-fixture.exe" in
               e["window"].get("properties", "").lower() for e in disappeared)
    assert any(e["event"] == "screen_capture" and e["reason"] == "candidate_game_window"
               and e["ok"] for e in events)
    assert (run / "evidence/window-lifecycle.jsonl").is_file()


def test_supervisor_records_no_window_and_proton_crash(tmp_path, monkeypatch):
    no_window = ("import subprocess,sys\n"
                 "raise SystemExit(subprocess.run([sys.executable,'-c',"
                 "'import time; time.sleep(.3)',sys.argv[2]]).returncode)\n")
    _, _, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, no_window)
    assert result == 0
    assert not any(e["event"] == "window_created" and e["window"]["game_candidate"]
                   for e in _runtime_events(run))
    crash = "import os\nos.abort()\n"
    _, _, crash_run, result = _run_supervisor_fixture(tmp_path, monkeypatch, crash)
    assert result == 0
    crash_event = next(e for e in _runtime_events(crash_run)
                       if e["event"] == "proton_wrapper_exit")
    assert crash_event["returncode"] == -6 and crash_event["signal"] == 6
    assert (crash_run / "logs/proton-console.log").exists()


def test_supervisor_timeout_cleans_owned_game_and_preserves_host_process(tmp_path, monkeypatch):
    host_process = subprocess.Popen(["sleep", "20"])
    try:
        body = ("import subprocess,sys,time\n"
                "subprocess.Popen([sys.executable,'-c','import time; time.sleep(20)',sys.argv[2]])\n"
                "time.sleep(20)\n")
        _, _, run, result = _run_supervisor_fixture(tmp_path, monkeypatch, body, seconds=1)
        assert result == 0
        events = _runtime_events(run)
        assert any(e["event"] == "process_supervisor_signal" and
                   e.get("signal") == "SIGTERM" and "hyperdemon" in e.get("roles", [])
                   for e in events)
        summary = json.loads((run / "evidence/process-lifecycle.json").read_text())
        assert summary["supervisor_terminated_game"] is True
        assert host_process.poll() is None
        assert (run / "logs/proton-console.log").is_file()
    finally:
        host_process.terminate()
        host_process.wait(timeout=3)
