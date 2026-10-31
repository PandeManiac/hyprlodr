#!/usr/bin/env python3
"""HYPRLODR synthetic-host loader test suite.

Every scenario copies real, independently compiled shared libraries into a
temporary mod directory and runs the hyprlodr-host executable. Assertions read
the JSON event log the loader itself produced, so nothing is simulated.

Usage: loader_suite.py <host> <samples-dir> <fixtures-dir>
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

HOST = pathlib.Path(sys.argv[1])
SAMPLES_DIR = pathlib.Path(sys.argv[2])
FIXTURES_DIR = pathlib.Path(sys.argv[3])

SAMPLE_ORDER = ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"]

RESULTS = []


class Runner:
    """Runs the host against a temporary mod directory and parses its output."""

    def __init__(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="hyprlodr-suite-"))
        self.mods = self.tmp / "mods"
        self.mods.mkdir()

    def add(self, *names, kind="fixture"):
        for name in names:
            source = (FIXTURES_DIR if kind == "fixture" else SAMPLES_DIR) / f"{name}.so"
            if not source.exists():
                raise AssertionError(f"missing build artifact: {source}")
            shutil.copy(source, self.mods / source.name)

    def add_raw(self, filename, payload: bytes):
        (self.mods / filename).write_bytes(payload)

    def write(self, name, text):
        path = self.tmp / name
        path.write_text(text)
        return path

    def run(self, seed=None, disable=(), disabled_file=None, state_root=None):
        log = self.tmp / "events.jsonl"
        if log.exists():
            log.unlink()
        args = [str(HOST), "--log", str(log), str(self.mods)]
        if state_root:
            args += ["--state-root", str(state_root)]
        if seed is not None:
            args += ["--seed", str(seed)]
        for mod_id in disable:
            args += ["--disable", mod_id]
        if disabled_file:
            args += ["--disabled-file", str(disabled_file)]
        proc = subprocess.run(args, capture_output=True, text=True)
        events = []
        if log.exists():
            events = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
        summary = {}
        for line in proc.stdout.splitlines():
            if line.startswith("{"):
                summary = json.loads(line)
        return proc, events, summary

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


def events_of(events, name):
    return [e["mod"] for e in events if e["event"] == name]


def errors_of(events):
    return [e["message"] for e in events if e["event"] == "error"]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_error(events, fragment):
    messages = errors_of(events)
    check(any(fragment in m for m in messages),
          f"expected an error containing {fragment!r}, saw {messages}")
    return messages


def scenario(name):
    def register(fn):
        RESULTS.append((name, fn))
        return fn
    return register


# --------------------------------------------------------------- happy paths

@scenario("five_mod_lifecycle")
def five_mod_lifecycle():
    runner = Runner()
    try:
        runner.add(*SAMPLE_ORDER, kind="samples")
        proc, events, summary = runner.run()
        check(proc.returncode == 0, f"host failed: {proc.stderr}")
        check(summary.get("ok") is True, f"summary not ok: {summary}")
        check(events_of(events, "initialize_ok") == SAMPLE_ORDER,
              f"init order {events_of(events, 'initialize_ok')}")
        check(events_of(events, "shutdown") == list(reversed(SAMPLE_ORDER)),
              f"shutdown order {events_of(events, 'shutdown')}")
        check(any(e["mod"] == "00.final" and "confirmed all prerequisites" in e["message"]
                  for e in events if e["event"] == "log"),
              "final mod did not confirm prerequisites")
        check(any(e["message"] == "test.overlay" for e in events
                  if e["event"] == "capability_registered"), "overlay capability missing")
    finally:
        runner.close()


@scenario("deterministic_startup")
def deterministic_startup():
    runner = Runner()
    try:
        runner.add(*SAMPLE_ORDER, kind="samples")
        orders = set()
        for seed in range(0, 12):
            proc, events, _ = runner.run(seed=seed)
            check(proc.returncode == 0, f"seed {seed} failed: {proc.stderr}")
            orders.add(tuple(events_of(events, "initialize_ok")))
        check(orders == {tuple(SAMPLE_ORDER)}, f"seeded runs diverged: {orders}")
    finally:
        runner.close()


@scenario("repeated_host_execution")
def repeated_host_execution():
    runner = Runner()
    try:
        runner.add(*SAMPLE_ORDER, kind="samples")
        for _ in range(10):
            proc, events, _ = runner.run()
            check(proc.returncode == 0, proc.stderr)
            check(events_of(events, "initialize_ok") == SAMPLE_ORDER, "unstable order")
    finally:
        runner.close()


@scenario("lexicographic_tie_break")
def lexicographic_tie_break():
    runner = Runner()
    try:
        runner.add("tie_beta", "tie_alpha", "tie_gamma")
        _, events, _ = runner.run(seed=7)
        check(events_of(events, "initialize_ok") == ["tie.alpha", "tie.beta", "tie.gamma"],
              f"tie-break order {events_of(events, 'initialize_ok')}")
    finally:
        runner.close()


@scenario("diamond_dependency")
def diamond_dependency():
    runner = Runner()
    try:
        runner.add("diamond_top", "diamond_left", "diamond_right", "diamond_bottom")
        order = None
        for seed in (0, 1, 2, 3):
            proc, events, _ = runner.run(seed=seed)
            check(proc.returncode == 0, proc.stderr)
            observed = events_of(events, "initialize_ok")
            check(observed[0] == "d.bottom", f"d.bottom not first: {observed}")
            check(observed[-1] == "d.top", f"d.top not last: {observed}")
            check(set(observed) == {"d.bottom", "d.left", "d.right", "d.top"}, observed)
            if order is None:
                order = observed
            else:
                check(observed == order, f"diamond order changed with seed {seed}")
    finally:
        runner.close()


@scenario("version_constraint_satisfied")
def version_constraint_satisfied():
    runner = Runner()
    try:
        runner.add("z.foundation", kind="samples")
        runner.add("ver_ok")
        proc, events, _ = runner.run()
        check(proc.returncode == 0, f"valid constraint rejected: {proc.stderr}")
        check("ver.ok" in events_of(events, "initialize_ok"), "ver.ok did not initialize")
    finally:
        runner.close()


@scenario("state_passed_between_mods")
def state_passed_between_mods():
    runner = Runner()
    try:
        runner.add("state_writer", "state_reader")
        proc, events, _ = runner.run()
        check(proc.returncode == 0, f"state handoff failed: {proc.stderr}")
        check(events_of(events, "initialize_ok") == ["k.writer", "k.reader"],
              events_of(events, "initialize_ok"))
    finally:
        runner.close()


# ------------------------------------------------------------ error handling

@scenario("missing_dependency")
def missing_dependency():
    runner = Runner()
    try:
        runner.add("missing_dep")
        proc, events, summary = runner.run()
        check(proc.returncode != 0, "host should fail on a missing dependency")
        expect_error(events, "missing dependency: not.there")
        check(events_of(events, "initialize_ok") == [], "no mod may initialize")
        check(summary.get("ok") is False, "summary should report failure")
    finally:
        runner.close()


@scenario("circular_dependency")
def circular_dependency():
    runner = Runner()
    try:
        runner.add("cycle_a", "cycle_b")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "cycle must fail")
        expect_error(events, "cycle among")
        check(events_of(events, "initialize_ok") == [], "cycle must not initialize anything")
    finally:
        runner.close()


@scenario("self_dependency")
def self_dependency():
    runner = Runner()
    try:
        runner.add("self_dep")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "self dependency must fail")
        expect_error(events, "cycle among")
    finally:
        runner.close()


@scenario("disabled_dependency")
def disabled_dependency():
    runner = Runner()
    try:
        runner.add("z.foundation", "a.palette", kind="samples")
        proc, events, _ = runner.run(disable=["z.foundation"])
        check(proc.returncode != 0, "dependent of a disabled mod must fail")
        expect_error(events, "dependency is disabled: z.foundation")
        check("a.palette" not in events_of(events, "initialize_ok"), "palette must not start")
    finally:
        runner.close()


@scenario("disabled_file_list")
def disabled_file_list():
    runner = Runner()
    try:
        runner.add("z.foundation", "b.audio", kind="samples")
        listing = runner.write("disabled.txt", "# comment line\n\nb.audio\n")
        proc, events, _ = runner.run(disabled_file=listing)
        check(proc.returncode == 0, f"disabled file broke the run: {proc.stderr}")
        check(events_of(events, "initialize_ok") == ["z.foundation"],
              f"disabled mod still initialized: {events_of(events, 'initialize_ok')}")
    finally:
        runner.close()


@scenario("duplicate_mod_id")
def duplicate_mod_id():
    runner = Runner()
    try:
        runner.add("dup_one", "dup_two")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "duplicate ids must be reported")
        expect_error(events, "duplicate mod id")
        check(len(events_of(events, "initialize_ok")) == 1,
              "exactly one copy of a duplicated id may initialize")
    finally:
        runner.close()


@scenario("version_incompatible")
def version_incompatible():
    runner = Runner()
    try:
        runner.add("z.foundation", kind="samples")
        runner.add("ver_old")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "unsatisfied constraint must fail")
        expect_error(events, "does not satisfy")
        check("ver.old" not in events_of(events, "initialize_ok"), "incompatible mod started")
    finally:
        runner.close()


@scenario("conflicting_mods")
def conflicting_mods():
    runner = Runner()
    try:
        runner.add("conflict_a", "conflict_b")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "declared conflict must fail")
        expect_error(events, "conflicts with loaded mod")
    finally:
        runner.close()


@scenario("native_abi_mismatch")
def native_abi_mismatch():
    runner = Runner()
    try:
        runner.add("abi_old")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "ABI 1 mod must be rejected")
        expect_error(events, "native ABI mismatch")
        check(events_of(events, "initialize_ok") == [], "rejected mod must not initialize")
    finally:
        runner.close()


@scenario("descriptor_too_small")
def descriptor_too_small():
    runner = Runner()
    try:
        runner.add("tiny_descriptor")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "truncated descriptor must be rejected")
        expect_error(events, "struct_size too small")
    finally:
        runner.close()


@scenario("null_descriptor")
def null_descriptor():
    runner = Runner()
    try:
        runner.add("bad_descriptor")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "NULL descriptor must be rejected")
        expect_error(events, "rejected host ABI version")
    finally:
        runner.close()


@scenario("missing_entry_export")
def missing_entry_export():
    runner = Runner()
    try:
        runner.add("no_entry")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "library without the entry symbol must be rejected")
        expect_error(events, "missing export hyprlodr_plugin_query")
    finally:
        runner.close()


@scenario("corrupt_library")
def corrupt_library():
    runner = Runner()
    try:
        runner.add("plain_a")
        runner.add_raw("garbage.so", b"this is not a shared object\n" * 32)
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "corrupt library must be reported")
        reported = [e for e in events if e["event"] == "error"
                    and "garbage.so" in e["mod"]]
        check(reported, f"corrupt file not reported as an error: {errors_of(events)}")
        check("invalid ELF" in reported[0]["message"] or reported[0]["message"],
              "corrupt file error carried no detail")
        check("plain.a" in events_of(events, "initialize_ok"),
              "a bad file must not stop valid mods from loading")
    finally:
        runner.close()


@scenario("empty_mod_directory")
def empty_mod_directory():
    runner = Runner()
    try:
        proc, _, summary = runner.run()
        check(proc.returncode != 0, "empty directory must fail")
        check(summary.get("mods") == 0, "no mods should be discovered")
    finally:
        runner.close()


# ------------------------------------------------------- failure and rollback

@scenario("initialization_failure_stops_dependents")
def initialization_failure_stops_dependents():
    runner = Runner()
    try:
        runner.add("early_ok", "failer", "dependent_of_failer")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "failing mod must fail the run")
        started = events_of(events, "initialize_begin")
        ok = events_of(events, "initialize_ok")
        check(ok == ["a.early"], f"only the pre-failure mod may finish: {ok}")
        check("a.dependent" not in started, "dependent of failed mod must never start")
        check(events_of(events, "shutdown") == ["a.early"],
              f"partial cleanup wrong: {events_of(events, 'shutdown')}")
        expect_error(events, "initialization failed")
        rollback = [e for e in events if e["event"] == "rollback"]
        check(rollback and "2 state/capability change(s)" in rollback[0]["message"],
              f"rollback journal not applied: {rollback}")
    finally:
        runner.close()


@scenario("rollback_leaves_no_residue")
def rollback_leaves_no_residue():
    runner = Runner()
    try:
        runner.add("early_ok", "failer")
        runner.add("residue_check")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "expected failure")
        check("z.residue" not in events_of(events, "initialize_ok"),
              "mod initialized after a failure")
    finally:
        runner.close()


@scenario("state_dirs_are_per_mod")
def state_dirs_are_per_mod():
    runner = Runner()
    try:
        runner.add("dir_writer_a", "dir_writer_b")
        root = runner.tmp / "state"
        proc, events, _ = runner.run(state_root=root)
        check(proc.returncode == 0, f"host failed: {proc.stderr}")
        first = root / "p.writer" / "probe.txt"
        second = root / "p.writer2" / "probe.txt"
        check(first.exists(), f"missing per-mod state file {first}")
        check(second.exists(), f"missing per-mod state file {second}")
        check(first.read_text() == "p.writer", f"unexpected content: {first.read_text()!r}")
        check(second.read_text() == "p.writer2", f"unexpected content: {second.read_text()!r}")
        announced = [e["message"] for e in events if e["event"] == "state_dir"]
        check(len(announced) == 2, f"expected two state_dir events, saw {announced}")
        check(announced[0] != announced[1], f"mods shared a state directory: {announced}")
    finally:
        runner.close()


@scenario("state_dir_persists_across_runs")
def state_dir_persists_across_runs():
    runner = Runner()
    try:
        runner.add("state_counter")
        root = runner.tmp / "state"
        first_proc, first_events, _ = runner.run(state_root=root)
        second_proc, second_events, _ = runner.run(state_root=root)
        check(first_proc.returncode == 0, f"first run failed: {first_proc.stderr}")
        check(second_proc.returncode == 0, f"second run failed: {second_proc.stderr}")

        def counter(events):
            # The loader prefixes mod log lines with the level, e.g. "info: ...".
            return [e["message"].rsplit("state counter ", 1)[1] for e in events
                    if e["event"] == "log" and "state counter " in e["message"]]

        check(counter(first_events) == ["1"],
              f"first run saw {counter(first_events)}")
        check(counter(second_events) == ["2"],
              f"second run saw {counter(second_events)}")
        stored = root / "p.counter" / "counter.txt"
        check(stored.exists(), f"missing {stored}")
        check(stored.read_text() == "2", f"stored value {stored.read_text()!r}")
    finally:
        runner.close()


@scenario("state_dir_absent_without_root")
def state_dir_absent_without_root():
    runner = Runner()
    try:
        runner.add("dir_writer_a")
        proc, events, _ = runner.run()
        check(proc.returncode != 0, "a mod requiring state_dir must fail without a root")
        messages = [e["message"] for e in events if e["event"] == "log"]
        check(any("state_dir unavailable" in m for m in messages),
              f"mod did not report the missing state dir: {messages}")
        check(not any(e["event"] == "state_dir" for e in events),
              "a state directory was announced without a root")
    finally:
        runner.close()


def main():
    failures = 0
    for name, fn in RESULTS:
        try:
            fn()
            print(f"PASS {name}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
        except Exception as exc:  # pragma: no cover - unexpected harness error
            failures += 1
            print(f"ERROR {name}: {type(exc).__name__}: {exc}")
    total = len(RESULTS)
    print(f"\n{total - failures}/{total} scenarios passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
