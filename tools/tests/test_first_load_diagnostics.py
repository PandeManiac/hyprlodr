import json
from pathlib import Path

import pytest

from tools.re import first_load_diagnostics as diag


def write_jsonl(path: Path, rows: list[dict]) -> None:
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))


def test_coverage_merges_shared_sequence_and_only_concludes_with_complete_trace(tmp_path):
    run_id = "0000000100000002"
    base = {"run_id": run_id, "pid": 1}
    startup = tmp_path / "startup.jsonl"
    files = tmp_path / "files.jsonl"
    attach = tmp_path / "attach.json"
    write_jsonl(startup, [
        {**base, "seq": 1, "event": "early_hook_install", "flags": 3,
         "text": "installed_dormant"},
        {**base, "seq": 2, "event": "bootstrap_start"},
        {**base, "seq": 3, "event": "diagnostic_writer_status", "flags": 2},
        {**base, "seq": 5, "event": "first_loader_callsite"},
        {**base, "seq": 6, "event": "first_hand2_request", "tag": 2},
        {**base, "seq": 7, "event": "tag1_hand2_loader_return", "tag": 1},
        {**base, "seq": 8, "event": "route_activation", "flags": 1,
         "text": "enabled"},
        {**base, "seq": 9, "event": "first_tag1_hand2_request", "tag": 1,
         "flags": 34},
        {**base, "seq": 10, "event": "tag1_hand2_loader_call", "tag": 1,
             "flags": 58},
    ])
    write_jsonl(files, [{**base, "seq": 4, "event": "file_open",
                         "requested": "asset/asset", "opened": "asset/asset"}])
    attach.write_text(json.dumps({**base, "diagnostics_requested": True,
        "diagnostic_events_dropped": 0, "early_hook_install_attempted": True,
        "early_hook_installed": True, "experimental_early_hook_status": "installed_dormant"}))
    report = diag.build_coverage(startup, files, attach, process_exited=True)
    assert [row["seq"] for row in report["raw_event_timeline"]] == list(range(1, 11))
    assert report["coverage_complete"] is True
    assert report["early_hook"]["executed"] is True
    assert report["conclusions"]["replacement_timing_supported"] is True


def test_missing_target_is_unknown_until_complete_callsite_coverage(tmp_path):
    base = {"run_id": "0000000100000002", "pid": 1}
    startup, files, attach = (tmp_path / "startup.jsonl", tmp_path / "files.jsonl",
                              tmp_path / "attach.json")
    write_jsonl(startup, [{**base, "seq": 1, "event": "early_hook_install"}])
    write_jsonl(files, [])
    attach.write_text(json.dumps({**base, "diagnostics_requested": True,
        "diagnostic_events_dropped": 0, "early_hook_install_attempted": True,
        "early_hook_installed": True}))
    report = diag.build_coverage(startup, files, attach, process_exited=True)
    assert report["conclusions"]["tag1_hand2_request"] == "unknown_incomplete_coverage"


def test_no_target_conclusion_is_limited_to_observed_process_not_later_gameplay(tmp_path):
    run_id = "0000000100000002"
    base = {"run_id": run_id, "pid": 1}
    startup, files, attach = (tmp_path / "startup.jsonl", tmp_path / "files.jsonl",
                              tmp_path / "attach.json")
    write_jsonl(startup, [
        {**base, "seq": 1, "event": "first_loader_callsite"},
        {**base, "seq": 2, "event": "bootstrap_start"},
        {**base, "seq": 3, "event": "diagnostic_writer_status", "flags": 2},
    ])
    write_jsonl(files, [])
    attach.write_text(json.dumps({**base, "diagnostics_requested": True,
        "diagnostic_events_dropped": 0}))
    report = diag.build_coverage(startup, files, attach, process_exited=True)
    assert report["conclusions"]["tag1_hand2_request"] == \
           "not_observed_during_instrumented_process"
    assert report["observation_scope"]["gameplay_trace_complete"] is False


def test_coverage_rejects_mixed_run_ids(tmp_path):
    startup, files, attach = (tmp_path / "startup.jsonl", tmp_path / "files.jsonl",
                              tmp_path / "attach.json")
    write_jsonl(startup, [{"seq": 1, "run_id": "a", "pid": 1, "event": "bootstrap_start"}])
    write_jsonl(files, [{"seq": 2, "run_id": "b", "pid": 1, "event": "file_open"}])
    attach.write_text(json.dumps({"run_id": "a", "pid": 1}))
    with pytest.raises(ValueError, match="mixed run IDs/PIDs"):
        diag.build_coverage(startup, files, attach, process_exited=True)


def test_coverage_reports_missing_bootstrap_outputs_as_unknown(tmp_path):
    report = diag.build_coverage(tmp_path / "missing-startup.jsonl",
        tmp_path / "missing-files.jsonl", tmp_path / "missing-attach.json",
        process_exited=True)
    assert report["coverage_complete"] is False
    assert report["conclusions"]["tag1_hand2_request"] == "unknown_incomplete_coverage"
    assert "bootstrap_worker_started_not_observed" in report["coverage_blockers"]


def test_coverage_rejects_gaps_in_the_shared_sequence(tmp_path):
    base = {"run_id": "0000000100000002", "pid": 1}
    startup, files, attach = (tmp_path / "startup.jsonl", tmp_path / "files.jsonl",
                              tmp_path / "attach.json")
    write_jsonl(startup, [
        {**base, "seq": 1, "event": "first_loader_callsite"},
        {**base, "seq": 3, "event": "bootstrap_start"},
        {**base, "seq": 4, "event": "diagnostic_writer_status", "flags": 2},
    ])
    write_jsonl(files, [])
    attach.write_text(json.dumps({**base, "diagnostics_requested": True,
        "diagnostic_events_dropped": 0}))
    report = diag.build_coverage(startup, files, attach, process_exited=True)
    assert report["coverage_complete"] is False
    assert "shared_sequence_gap" in report["coverage_blockers"]


def isolated_tree(tmp_path: Path):
    sandbox = tmp_path / "isolated"
    game = sandbox / "game"
    (game / "hyperdemon.exe").parent.mkdir(parents=True)
    (game / "hyperdemon.exe").write_bytes(b"fixture")
    roots = [sandbox / name for name in ("compatdata", "home", "temp", "steam")]
    for root in roots:
        root.mkdir(parents=True)
    return sandbox, game, *roots


def test_preflight_accepts_private_namespaces_while_recording_active_steam(tmp_path, monkeypatch):
    sandbox, game, compat, home, temp, steam_state = isolated_tree(tmp_path)
    source = tmp_path / "installed-game"
    source.mkdir()
    monkeypatch.setattr(diag, "ISOLATED_ROOT", sandbox.resolve())
    proc = tmp_path / "proc"
    proc.mkdir()
    (proc / "101").mkdir()
    (proc / "101/comm").write_text("unrelated\n")
    (proc / "101/cmdline").write_bytes(b"other\0")
    result = diag.validate_isolated_paths(sandbox, game, compat, home, temp,
                                          steam_state, source, proc)
    assert result["safe"] is True
    assert result["isolation"]["verified"] is True
    assert result["isolation"]["checks"]["host_abstract_ipc_unreachable"] is True
    (proc / "102").mkdir()
    (proc / "102/comm").write_text("steam\n")
    (proc / "102/cmdline").write_bytes(b"steam\0")
    result = diag.validate_isolated_paths(sandbox, game, compat, home, temp,
                                          steam_state, source, proc)
    assert result["safe"] is True
    assert result["active_steam_pids"] == [102]
    assert "blocked" in result["steam_cloud"]


def test_preflight_rejects_installed_game_tree_and_symlinked_writable_root(tmp_path, monkeypatch):
    sandbox, game, compat, home, temp, steam_state = isolated_tree(tmp_path)
    monkeypatch.setattr(diag, "ISOLATED_ROOT", sandbox.resolve())
    proc = tmp_path / "empty-proc"
    proc.mkdir()
    with pytest.raises(ValueError, match="outside the disposable sandbox"):
        diag.validate_isolated_paths(sandbox, game, compat, home,
            tmp_path / "not-isolated-temp", steam_state, tmp_path / "source", proc)
    link = sandbox / "linked-home"
    link.symlink_to(home, target_is_directory=True)
    with pytest.raises(ValueError, match="must not be a symlink"):
        diag.validate_isolated_paths(sandbox, game, compat, link, temp,
            steam_state, tmp_path / "source", proc)


def test_writable_hardlinks_are_rejected(tmp_path):
    sandbox = tmp_path / "sandbox"
    game, temp = sandbox / "game", sandbox / "temp"
    game.mkdir(parents=True)
    temp.mkdir()
    original = game / "original.bin"
    original.write_bytes(b"private")
    (temp / "linked.bin").hardlink_to(original)
    with pytest.raises(ValueError, match="hard-linked file"):
        diag.reject_external_links({"game": game, "temp": temp}, sandbox)


def test_bubblewrap_probe_proves_private_namespaces_and_host_ipc_isolation(tmp_path):
    import shutil
    if not shutil.which("bwrap"):
        pytest.skip("Bubblewrap is not installed")
    sandbox = tmp_path / "sandbox"
    home = sandbox / "home"
    home.mkdir(parents=True)
    report = diag.probe_bubblewrap_isolation(sandbox, home)
    assert report["verified"] is True
    assert all(value is True for key, value in report["checks"].items()
               if key != "unexpected_writable_mounts")
    assert report["checks"]["unexpected_writable_mounts"] == []
