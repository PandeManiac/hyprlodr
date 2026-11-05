import hashlib
import json
from pathlib import Path

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
    source_revision = "fixture-source-revision"
    monkeypatch.setattr(isolated, "current_source_identity", lambda: (source_revision, True))
    (tmp_path / "hyprlodr-proxy-build.json").write_text(json.dumps({
        "schema": 1, "source_revision": source_revision, "source_clean": True,
        "proxy_sha256": hashlib.sha256(proxy.read_bytes()).hexdigest(),
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


def test_stages_mod_free_game_copy_and_proves_runtime_isolation(tmp_path, monkeypatch):
    if not __import__("shutil").which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox, source, proton, proxy, openal, run = fixture_tree(tmp_path, monkeypatch)
    manifest = isolated.stage(source, proton, proxy, openal, run)
    game = run / "game"
    assert manifest["replacement_routing"] is False
    assert Path(manifest["proton_root"]).is_relative_to(run)
    assert (Path(manifest["proton_root"]) / "proton-alias").is_symlink()
    assert (game / "hyperdemon.exe").read_bytes() == (source / "hyperdemon.exe").read_bytes()
    assert (game / "asset/asset").read_bytes() == (source / "asset/asset").read_bytes()
    assert (game / "OpenAL32.dll").read_bytes() == proxy.read_bytes()
    assert (game / "OpenAL32_real.dll").read_bytes() == openal.read_bytes()
    assert list((game / "mods").iterdir()) == []
    assert not (game / "OST").exists()
    report = isolated.check_runtime(run, source)
    assert report["safe"] is True
    assert report["game_process_will_be_network_isolated"] is True
    assert report["cloud"].startswith("blocked")
    assert report["authentication"] == "not supplied; game may reject offline direct Proton startup"
    assert report["active_steam_pids_observed"] == diagnostics.live_steam_processes()


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
