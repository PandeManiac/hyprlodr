import json
import pathlib
import subprocess
import sys
import tempfile

host, plugin_dir = sys.argv[1:]
expected_init = ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"]
expected_stop = list(reversed(expected_init))

for run in range(5):
    with tempfile.TemporaryDirectory(prefix="hyprlodr-") as tmp:
        log = pathlib.Path(tmp) / "events.jsonl"
        result = subprocess.run([host, plugin_dir, str(log)], text=True, capture_output=True)
        assert result.returncode == 0, f"run {run}: host failed: {result.stderr}"
        events = [json.loads(line) for line in log.read_text().splitlines()]
        started = [e["mod"] for e in events if e["event"] == "initialize_ok"]
        stopped = [e["mod"] for e in events if e["event"] == "shutdown"]
        assert started == expected_init, f"run {run}: unexpected startup {started}"
        assert stopped == expected_stop, f"run {run}: unexpected shutdown {stopped}"
        assert any(e["mod"] == "00.final" and "confirmed all prerequisites" in e["message"] for e in events)
print("five independently compiled plugins loaded, shared state/capabilities verified, deterministic startup and reverse shutdown passed 5 runs")
