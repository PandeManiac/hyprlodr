import importlib.util
import json
import os
import pathlib
import sys
import tempfile
from types import SimpleNamespace
from unittest import mock
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / "asset_trace.py"
SPEC = importlib.util.spec_from_file_location("asset_trace", MODULE)
trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trace)


def expected(digest="a" * 64):
    return {
        "record": {"name": "hand2", "archive_offset": trace.HAND2_OFFSET,
                   "encrypted_size": trace.HAND2_SIZE, "type_tag": trace.HAND2_TYPE,
                   "seed": trace.seed_for_name("hand2"),
                   "key_class": trace.seed_for_name("hand2") & 0x3ff},
        "payload_sha256": digest,
        "payload_size": trace.HAND2_SIZE,
        "mesh": {"vertex_count": 238, "index_count": 1188, "vertex_stride": 72,
                 "vertex_array_sha256": "b" * 64, "index_array_sha256": "c" * 64},
    }


def complete_events(payload_hash="a" * 64):
    record = expected()["record"]
    return [
        {"event": "process_identified", "identity_verified": True},
        {"event": "attach_succeeded"},
        {"event": "trace_ready", "signature_check": "passed",
         "hook_installation": "complete", "sha256_self_test": "passed"},
        {"event": "coverage_started", "process_start_coverage": False},
        {"event": "asset_lookup", "request_id": 1, "name": "hand2"},
        {"event": "asset_lookup_result", "request_id": 1, "name": "hand2", "found": True},
        {"event": "record_resolved", "request_id": 1, "name": "hand2",
         "archive_offset": record["archive_offset"], "encrypted_size": record["encrypted_size"],
         "type_tag": record["type_tag"]},
        {"event": "payload_decrypted", "request_id": 1, "name": "hand2",
         "size": trace.HAND2_SIZE, "seed": record["seed"], "key_class": record["key_class"],
         "payload_sha256": payload_hash},
        {"event": "tag1_parse_result", "request_id": 1, "name": "hand2", "success": True},
        {"event": "mesh_decode", "request_id": 1, "name": "hand2", "complete": True,
         "arrays_bounded": True, "vertex_count": 238, "index_count": 1188,
         "vertex_stride": 72, "vertex_array_sha256": "b" * 64,
         "index_array_sha256": "c" * 64},
    ]


class AssetTraceTests(unittest.TestCase):
    def setUp(self):
        self.original = expected("a" * 64)
        self.modified = expected("d" * 64)

    def test_confirms_modified_payload_through_mesh_decode(self):
        result = trace.validate_capture(complete_events("d" * 64), self.original, self.modified)
        self.assertTrue(result["trace_ready"])
        self.assertTrue(result["hand2_consumed"])
        self.assertTrue(result["modified_payload_decrypted"])
        self.assertTrue(result["hand2_mesh_decoded"])
        self.assertFalse(result["rendering_observed"])
        self.assertEqual(result["requests"][0]["variant"], "modified")
        self.assertEqual(result["requests"][0]["strongest_stage"], "mesh_decoded")

    def test_lookup_without_payload_is_not_reported_as_consumed(self):
        events = [{"event": "process_identified", "identity_verified": True},
                  {"event": "attach_succeeded"},
                  {"event": "trace_ready", "signature_check": "passed",
                   "hook_installation": "complete", "sha256_self_test": "passed"},
                  {"event": "coverage_started", "process_start_coverage": False},
                  {"event": "asset_lookup", "request_id": 7, "name": "hand2"}]
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertFalse(result["hand2_consumed"])
        self.assertFalse(result["process_start_coverage"])
        self.assertEqual(result["interpretation"],
                         "hand2-related activity reached lookup; a fully verified mesh decode was not established")
        self.assertEqual(result["requests"][0]["strongest_stage"], "lookup")

    def test_empty_late_capture_does_not_claim_asset_was_not_loaded(self):
        events = complete_events()[:4]
        events.insert(1, {"event": "process_identified", "identity_verified": True})
        events.insert(2, {"event": "attach_succeeded"})
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertTrue(result["trace_ready"])
        self.assertEqual(result["requests_observed"], 0)
        self.assertFalse(result["process_start_coverage"])
        self.assertIn("earlier activity may have been missed", result["interpretation"])

    def test_payload_hash_can_be_confirmed_without_claiming_mesh_decode(self):
        events = complete_events("d" * 64)[:8]
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertTrue(result["modified_payload_decrypted"])
        self.assertTrue(result["target_payload_decrypted"])
        self.assertFalse(result["hand2_consumed"])
        self.assertFalse(result["hand2_mesh_decoded"])

    def test_rejects_record_mismatch_and_unknown_payload_hash(self):
        events = complete_events("e" * 64)
        events[6]["archive_offset"] += 1
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertFalse(result["target_payload_decrypted"])
        self.assertFalse(result["hand2_consumed"])
        self.assertIn("record archive_offset mismatch: expected 295758784, got 295758785",
                      result["requests"][0]["errors"])
        self.assertIn("decrypted payload hash matches neither archive variant",
                      result["requests"][0]["errors"])

    def test_rejects_stage_reordering_and_incomplete_mesh(self):
        events = complete_events("d" * 64)
        events[7], events[8] = events[8], events[7]
        events[-1]["complete"] = False
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertTrue(result["target_payload_decrypted"])
        self.assertFalse(result["hand2_consumed"])
        errors = result["requests"][0]["errors"]
        self.assertIn("loader stage events are out of order", errors)
        self.assertIn("mesh decode completion or bounded arrays not confirmed", errors)

    def test_mesh_array_hash_mismatch_fails_closed(self):
        events = complete_events("d" * 64)
        events[-1]["vertex_array_sha256"] = "f" * 64
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertFalse(result["hand2_consumed"])
        self.assertIn("mesh vertex_array_sha256 mismatch", result["requests"][0]["errors"])

    def test_same_name_non_model_record_is_not_mistaken_for_hand2_mesh(self):
        events = [
            {"event": "trace_ready", "signature_check": "passed",
             "hook_installation": "complete", "sha256_self_test": "passed"},
            {"event": "asset_lookup", "request_id": 2, "name": "hand2", "kind": 2},
            {"event": "record_resolved", "request_id": 2, "name": "hand2",
             "archive_offset": 277743449, "encrypted_size": 87415, "type_tag": 2},
            {"event": "payload_decrypted", "request_id": 2, "name": "hand2",
             "size": 87415, "seed": 123, "payload_sha256": "9" * 64},
        ]
        result = trace.validate_capture(events, self.original, self.modified)
        request = result["requests"][0]
        self.assertFalse(result["hand2_consumed"])
        self.assertEqual(request["record_role"], "same-name-non-model-record")
        self.assertEqual(request["strongest_stage"], "decrypted")
        self.assertEqual(request["errors"], [])

    def test_readiness_requires_hook_installation_and_coverage_is_explicit(self):
        result = trace.validate_capture(
            [{"event": "trace_ready", "signature_check": "passed"}],
            self.original, self.modified)
        self.assertFalse(result["trace_ready"])
        self.assertFalse(result["hooks_active_during_capture"])
        self.assertIn("expected executable process was not identified", result["interpretation"])

        events = complete_events("d" * 64)
        events[3]["process_start_coverage"] = True
        result = trace.validate_capture(events, self.original, self.modified)
        self.assertTrue(result["process_start_coverage"])

    def test_capture_always_unloads_script_and_detaches_session(self):
        state = {"unloaded": False, "detached": False, "ready": False, "resumed": False}
        script = None

        class Script:
            callback = None
            def on(self, _event, callback): self.callback = callback
            def load(self):
                state["ready"] = True
                self.callback({"type": "send", "payload": {
                    "event": "trace_ready", "signature_check": "passed",
                    "hook_installation": "complete"}}, None)
            def unload(self): state["unloaded"] = True

        class Session:
            def create_script(self, _source): return script
            def detach(self): state["detached"] = True

        script = Script()

        class Device:
            def enumerate_processes(self):
                return [SimpleNamespace(pid=4321, name=r"S:\\game\\hyperdemon.exe")]
            def attach(self, _pid): return Session()

        fake_frida = SimpleNamespace(get_local_device=lambda: Device())
        with tempfile.TemporaryDirectory(dir=trace.REPO_ROOT / ".tmp") as directory:
            output = pathlib.Path(directory) / "capture.jsonl"
            with mock.patch.dict(sys.modules, {"frida": fake_frida}):
                trace.capture(4321, output, 0, launch_gated=True,
                              resume_target=lambda: state.update(resumed=state["ready"]))
            rows = trace.read_jsonl(output)
        self.assertTrue(state["unloaded"])
        self.assertTrue(state["detached"])
        self.assertTrue(state["resumed"])
        events = [row.get("event") for row in rows]
        self.assertIn("script_loaded", events)
        self.assertIn("trace_ready", events)
        self.assertIn("capture_closed", events)
        coverage = next(row for row in rows if row.get("event") == "coverage_started")
        self.assertTrue(coverage["process_start_coverage"])

    def test_capture_rejects_unrelated_pid_before_attach(self):
        class Device:
            attached = False
            def enumerate_processes(self):
                return [SimpleNamespace(pid=4321, name="unrelated.exe")]
            def attach(self, _pid): self.attached = True

        device = Device()
        fake_frida = SimpleNamespace(get_local_device=lambda: device)
        with tempfile.TemporaryDirectory(dir=trace.REPO_ROOT / ".tmp") as directory:
            output = pathlib.Path(directory) / "wrong-process.jsonl"
            with mock.patch.dict(sys.modules, {"frida": fake_frida}):
                with self.assertRaisesRegex(RuntimeError, "not hyperdemon.exe"):
                    trace.capture(4321, output, 0)
            rows = trace.read_jsonl(output)
        self.assertFalse(device.attached)
        self.assertEqual(rows[0]["event"], "process_identity_rejected")

    def test_capture_artifacts_must_stay_in_persistent_repository_scratch(self):
        self.assertTrue(trace._inside_persistent_tmp(trace.REPO_ROOT / ".tmp/logs/capture.jsonl"))
        self.assertFalse(trace._inside_persistent_tmp(pathlib.Path("/tmp/capture.jsonl")))
        trace._configure_persistent_temp()
        for variable in ("TMPDIR", "TMP", "TEMP"):
            self.assertEqual(pathlib.Path(os.environ[variable]),
                             trace.REPO_ROOT / ".tmp")

    def test_wait_process_matches_windows_path_returned_by_frida(self):
        self.assertTrue(trace._process_name_matches(
            r"S:\\steamapps\\common\\hyperdemon\\hyperdemon.exe", "hyperdemon.exe"))
        self.assertFalse(trace._process_name_matches(
            r"S:\\steamapps\\common\\other.exe", "hyperdemon.exe"))

    def test_spawn_path_match_requires_installed_game_path(self):
        self.assertTrue(trace._spawn_is_target_game(SimpleNamespace(
            identifier="S:\\steamapps\\common\\hyperdemon\\hyperdemon.exe", argv=[])))
        self.assertFalse(trace._spawn_is_target_game(SimpleNamespace(
            identifier="/opt/other/hyperdemon.exe", argv=[])))

    def test_spawn_gating_denial_is_persisted_before_any_launch(self):
        class Device:
            spawned = False
            def on(self, _event, _callback): pass
            def enable_spawn_gating(self): raise PermissionError("operation not permitted")
            def spawn(self, *_args, **_kwargs):
                self.spawned = True
                raise AssertionError("must not launch after spawn gating is denied")
            def off(self, _event, _callback): pass

        device = Device()
        fake_frida = SimpleNamespace(get_local_device=lambda: device)
        with tempfile.TemporaryDirectory(dir=trace.REPO_ROOT / ".tmp") as directory:
            output = pathlib.Path(directory) / "gated.jsonl"
            with mock.patch.dict(sys.modules, {"frida": fake_frida}):
                with self.assertRaisesRegex(PermissionError, "operation not permitted"):
                    trace.capture_proton_child(output, 1, 1)
            self.assertFalse(device.spawned)
            rows = trace.read_jsonl(output)
            status = json.loads(output.with_suffix(".launcher.json").read_text())
        self.assertEqual(rows[0]["stage"], "launch")
        self.assertIn("operation not permitted", rows[0]["message"])
        self.assertEqual(status["outcome"], "instrumentation_or_launch_failed")
        self.assertFalse(status["coverage_from_process_start"])

    def test_attach_failure_is_written_as_a_persistent_diagnostic(self):
        class Device:
            def enumerate_processes(self):
                return [SimpleNamespace(pid=4321, name="hyperdemon.exe")]
            def attach(self, _pid):
                raise OSError("ptrace blocked")

        fake_frida = SimpleNamespace(get_local_device=lambda: Device())
        with tempfile.TemporaryDirectory(dir=trace.REPO_ROOT / ".tmp") as directory:
            output = pathlib.Path(directory) / "failed-capture.jsonl"
            with mock.patch.dict(sys.modules, {"frida": fake_frida}):
                with self.assertRaisesRegex(RuntimeError, "could not attach"):
                    trace.capture(4321, output, 0.01)
            row = trace.read_jsonl(output)[0]
            rows = trace.read_jsonl(output)
        self.assertEqual(row["event"], "process_identified")
        self.assertEqual(rows[1]["event"], "capture_error")
        self.assertEqual(rows[1]["stage"], "attach")
        self.assertEqual(rows[1]["message"], "ptrace blocked")


if __name__ == "__main__":
    unittest.main()
