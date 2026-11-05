#!/usr/bin/env bash
# Exercise the Windows x64 HYPRLODR artifacts under Wine.
#
# 1. hyprlodr-host.exe loads the five sample mod DLLs and produces the expected
#    initialization and shutdown order (same assertions as the native suite).
# 2. proxy_client.exe loads the OpenAL32 proxy, resolves forwarded exports and
#    waits for the bootstrap thread, proving the in-game attach path runs.
#
# Usage: tools/test_windows.sh [build-win-dir] [path-to-original-OpenAL32.dll]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build-win}"
ORIGINAL_OPENAL="${2:-}"
# Wine rejects relative WINEPREFIX paths and winepath needs absolute ones.
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
if [ -n "$ORIGINAL_OPENAL" ]; then
  ORIGINAL_OPENAL="$(cd "$(dirname "$ORIGINAL_OPENAL")" && pwd)/$(basename "$ORIGINAL_OPENAL")"
fi

export WINEPREFIX="$OUT/wineprefix"
export WINEDEBUG=-all
WORK="$OUT/wintest"
rm -rf "$WORK"
mkdir -p "$WORK/mods" "$WORK/proxy/mods"

status=0

echo "== wine: compiled CALL rel32 interception fixture =="
timeout 45 wine "$OUT/tests/callsite_hook_client.exe" || status=1

echo "== native hand dispatch readiness state tests =="
cc -std=c17 -Wall -Wextra -Werror "$ROOT/native/tests/hand_dispatch_test.c" \
  -o "$OUT/hand-dispatch-test"
"$OUT/hand-dispatch-test" || status=1

echo "== wine host: five sample mods =="
cp "$OUT"/mods/*.dll "$WORK/mods/"
timeout 60 wine "$OUT/hyprlodr-host.exe" \
  --log "$(winepath -w "$WORK/events.jsonl")" \
  "$(winepath -w "$WORK/mods")" || status=1
python3 - "$WORK" <<'PY' || status=1
import json, pathlib, sys
work = pathlib.Path(sys.argv[1])
summary = json.loads((work / "summary.json").read_text()) if (work / "summary.json").exists() else None
events = [json.loads(line) for line in (work / "events.jsonl").read_text().splitlines() if line.strip()]
init = [e["mod"] for e in events if e["event"] == "initialize_ok"]
stop = [e["mod"] for e in events if e["event"] == "shutdown"]
expected = ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"]
assert init == expected, f"init order {init}"
assert stop == list(reversed(expected)), f"shutdown order {stop}"
print("windows host: init/shutdown order OK")
PY

if [ -n "$ORIGINAL_OPENAL" ]; then
  echo "== wine proxy: forwarding + bootstrap =="
  cp "$OUT/OpenAL32.dll" "$WORK/proxy/OpenAL32.dll"
  cp "$ORIGINAL_OPENAL" "$WORK/proxy/OpenAL32_real.dll"
  cp "$OUT"/mods/*.dll "$WORK/proxy/mods/"
  # ABI 4 file redirection: registers a probe redirect the client exercises.
  cp "$OUT/tests/redirect_mod.dll" "$WORK/proxy/mods/"
  rm -f "$WORK/proxy/redirect-probe.dat"
  mkdir -p "$WORK/proxy/asset"
  printf 'hyprlodr original asset content' > "$WORK/proxy/asset/asset"

  CFG=(--target=x86_64-pc-windows-msvc -std=c17 -O2 -ffreestanding -nostdlibinc
       -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror
       -DHYPRLODR_MINIMAL_WIN32
       -I"$ROOT/native/win32/crt" -I"$ROOT/native/win32"
       -I"$ROOT/native/include" -I"$ROOT/native/src")
  echo "== real hand-hook mod: delayed dispatch, capture writer, and shutdown =="
  HAND_WORK="$OUT/hand-hook-fixture"
  mkdir -p "$HAND_WORK/state"
  CAPTURE_WIN="$(winepath -w "$HAND_WORK/hand2.capture")"
  printf 'capture\n%s\n' "$CAPTURE_WIN" > "$HAND_WORK/state/hand-mode.txt"
  clang "${CFG[@]}" -c "$ROOT/native/win32/tests/hand_hook_client.c" \
    -o "$OUT/obj/hand_hook_client.obj"
  lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
    "/out:$HAND_WORK/hand_hook_client.exe" \
    "$OUT/obj/hand_hook_client.obj" "$OUT/obj/entry.obj" "$OUT/obj/crt_impl.obj" \
    "$OUT/lib/kernel32.lib"
  timeout 20 wine "$HAND_WORK/hand_hook_client.exe" \
    "$(winepath -w "$OUT/examples/example.hand-hook.dll")" \
    "$(winepath -w "$HAND_WORK/state")" || status=1
  python3 - "$HAND_WORK" <<'PY' || status=1
import pathlib, sys
log = pathlib.Path(sys.argv[1]) / "hand2.capture"
assert not log.exists(), "fixture must not create a geometry snapshot without a hand draw"
print("hand-hook fixture: initialized and restored without a draw; no geometry file created")
PY
  clang "${CFG[@]}" -c "$ROOT/native/win32/tests/proxy_client.c" -o "$OUT/obj/proxy_client.obj"
  lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
    "/out:$WORK/proxy/proxy_client.exe" \
    "$OUT/obj/proxy_client.obj" "$OUT/obj/entry.obj" "$OUT/obj/crt_impl.obj" \
    "$OUT/lib/kernel32.lib"

  rm -f "$WORK/proxy/hyprlodr-config.json"
  ( cd "$WORK/proxy" && timeout 90 wine ./proxy_client.exe ) | tee "$WORK/proxy/client.log" || status=1

  python3 - "$WORK/proxy" 2500 <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
identity = json.loads((base.parent.parent / "hyprlodr-proxy-build.json").read_text())
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["proxy_build_revision"] == identity["source_revision"], attach
assert attach["proxy_capability_bits"] == identity["capability_bits"] == 3, attach
assert attach["proxy_capabilities"] == ",".join(identity["capabilities"]), attach
print(f"proxy identity: revision={attach['proxy_build_revision']} capabilities={attach['proxy_capabilities']}")
assert attach["stage"] in ("mods_loaded", "mods_failed"), attach
print(f"proxy attach: stage={attach['stage']} pid={attach['pid']} status_text={attach['status_text']}")
# The test host is proxy_client.exe, not hyperdemon.exe. The fingerprint must
# say so: an unknown build is reported, never assumed compatible.
assert attach["module_parsed"] is True, attach
assert attach["anchors_total"] > 0, attach
assert attach["build_matches"] is False, attach
assert attach["anchors_matched"] < attach["anchors_total"], attach
assert attach["experimental_record_route_requested"] is False, attach
assert attach["experimental_record_route_status"] == "disabled", attach
print(f"build fingerprint: {attach['anchors_matched']}/{attach['anchors_total']} anchors, "
      f"build_matches={attach['build_matches']}." )
# The stand-in host has no asset tables at those RVAs, so the dump must decline
# rather than read past the image.
assert attach["tables_dumped"] is False, attach
# No config file is present for this run, so the built-in delay applies.
assert attach["delay_ms"] == 2500, attach
# proxy_client.exe exits normally, so the crash filter must stay silent: it is
# chained into the process and must never fire without an unhandled exception.
assert not (base / "hyprlodr-crash.json").exists(), "crash filter fired on a clean exit"
events = [json.loads(line) for line in (base / "hyprlodr-events.jsonl").read_text().splitlines() if line.strip()]
init = [e["mod"] for e in events if e["event"] == "initialize_ok"]
samples = ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"]
assert [mod for mod in init if mod != "r.redirect"] == samples, init
assert "r.redirect" in init, init
assert attach["file_hook"] == "installed", attach
# The stand-in host imports CreateFileA from KERNEL32 and no CRT file API, so the
# CRT targets are reported absent instead of being assumed present.
assert "CreateFileA=patched" in attach["file_hook_detail"], attach
assert "fopen=absent" in attach["file_hook_detail"], attach
print(f"file hook detail: {attach['file_hook_detail']}")
assert attach["file_redirects"] == 2, attach
published = [e for e in events if e["event"] == "file_redirects_published"]
assert len(published) == 1 and "2 file redirect(s) active" in published[0]["message"], published
print("proxy bootstrap: mods initialized inside the host process")
print("file hook: installed, two redirects published")
print("crash filter: silent on a clean exit")
PY
  python3 - "$WORK/proxy" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
client = (base / "client.log").read_text()
assert "asset-open: hyprlodr redirect content" in client, client
opens = [json.loads(line) for line in (base / "hyprlodr-files.jsonl").read_text().splitlines() if line.strip()]
asset_records = [e for e in opens if e.get("event") == "file_open" and e.get("requested") == "asset/asset"]
assert asset_records and asset_records[0]["redirected"] is True, asset_records
assert asset_records[0]["ok"] is True and asset_records[0]["opened"].endswith("replacement.txt"), asset_records[0]
waits = [e for e in opens if e.get("event") == "redirect_wait" and e.get("requested") == "asset/asset"]
assert any(e.get("stage") == "begin" for e in waits), waits
ready = [e for e in waits if e.get("stage") == "ready"]
assert ready and ready[0]["waited_ms"] > 0, waits
print(f"early asset open: waited {ready[0]['waited_ms']} ms, redirected, and returned readable replacement bytes")
PY
  echo "== wine proxy: hyprlodr-config.json overrides the delay =="
  printf '{\n  "delay_ms": 0,\n  "experimental_record_route": true\n}\n' > "$WORK/proxy/hyprlodr-config.json"
  rm -f "$WORK/proxy/hyprlodr-attach.json" "$WORK/proxy/hyprlodr-events.jsonl"
  ( cd "$WORK/proxy" && timeout 90 wine ./proxy_client.exe ) | tee "$WORK/proxy/client.log" || status=1

  python3 - "$WORK/proxy" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["delay_ms"] == 0, attach
assert attach["stage"] == "mods_loaded", attach
assert attach["experimental_record_route_requested"] is True, attach
assert attach["experimental_record_route_status"] == "rejected_unsupported_executable", attach
events = [json.loads(line) for line in (base / "hyprlodr-events.jsonl").read_text().splitlines() if line.strip()]
init = [e["mod"] for e in events if e["event"] == "initialize_ok"]
samples = ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"]
assert [mod for mod in init if mod != "r.redirect"] == samples, init
assert "r.redirect" in init, init
print("config: delay_ms=0 honoured, route opt-in rejected on the unsupported Wine host")
assert attach["file_redirects"] == 2, attach

# The first CreateFileA call opens "asset/asset" before the client waits for
# bootstrap. The proxy must hold it until the mod publishes its redirect.
client = (base / "client.log").read_text()
assert "asset-open: hyprlodr redirect content" in client, client
assert "redirect-probe: hyprlodr redirect content" in client, client
print("file hook: redirected open in the stand-in host returned the mod's bytes")

opens = [json.loads(line) for line in (base / "hyprlodr-files.jsonl").read_text().splitlines() if line.strip()]
asset_records = [e for e in opens if e.get("event") == "file_open" and e.get("requested") == "asset/asset"]
assert asset_records, opens[:5]
assert asset_records[0]["redirected"] is True, asset_records[0]
assert asset_records[0]["ok"] is True, asset_records[0]
assert asset_records[0]["opened"].endswith("replacement.txt"), asset_records[0]
waits = [e for e in opens if e.get("event") == "redirect_wait" and e.get("requested") == "asset/asset"]
if waits:
    assert any(e.get("stage") == "begin" for e in waits), waits
    assert any(e.get("stage") in ("ready", "timeout") for e in waits), waits
print("asset open: redirected and returned readable replacement bytes")
records = [e for e in opens if e.get("requested") == "redirect-probe.dat"]
assert records, opens[:5]
assert records[0]["redirected"] is True, records[0]
assert records[0]["ok"] is True, records[0]
assert records[0]["opened"].endswith("replacement.txt"), records[0]
# Every record says when it happened and on which thread, so a long in-game
# capture can be read as a timeline instead of an unordered list.
assert "t_ms" in records[0] and "thread" in records[0], records[0]
print(f"file log: {len(opens)} opens observed, redirect recorded")
PY

  run_gate_case() { # case-name mod-dll proxy-dll client-exe delay-config
    local name="$1" mod="$2" proxy="$3" client="$4" delay="$5"
    local base="$WORK/$name"
    mkdir -p "$base/mods" "$base/asset"
    cp "$proxy" "$base/OpenAL32.dll"
    cp "$ORIGINAL_OPENAL" "$base/OpenAL32_real.dll"
    cp "$OUT/tests/$mod" "$base/mods/r.redirect.dll"
    cp "$client" "$base/proxy_client.exe"
    printf 'hyprlodr original asset content' > "$base/asset/asset"
    if [ "$delay" = "zero" ]; then
      printf '{\n  "delay_ms": 0\n}\n' > "$base/hyprlodr-config.json"
    fi
    ( cd "$base" && timeout 90 wine ./proxy_client.exe ) | tee "$base/client.log" || status=1
  }

  echo "== wine proxy: successful init without an asset redirect =="
  run_gate_case no-redirect redirect_mod_no_redirect.dll "$OUT/OpenAL32.dll" \
    "$WORK/proxy/proxy_client.exe" default
  python3 - "$WORK/no-redirect" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["stage"] == "mods_loaded" and attach["file_redirects"] == 0, attach
client = (base / "client.log").read_text()
assert "asset-open: hyprlodr original asset content" in client, client
opens = [json.loads(x) for x in (base / "hyprlodr-files.jsonl").read_text().splitlines() if x.strip()]
asset = next(x for x in opens if x.get("event") == "file_open" and x.get("requested") == "asset/asset")
assert asset["redirected"] is False and asset["ok"] is True and asset["opened"] == "asset/asset", asset
waits = [x for x in opens if x.get("event") == "redirect_wait" and x.get("requested") == "asset/asset"]
assert any(x.get("stage") == "begin" for x in waits) and any(x.get("stage") == "ready" for x in waits), waits
print("no redirect registered: early open waited for successful initialization, then used the original path")
PY

  echo "== wine proxy: initialization error releases the asset open =="
  run_gate_case init-failure redirect_mod_init_failure.dll "$OUT/OpenAL32.dll" \
    "$WORK/proxy/proxy_client.exe" default
  python3 - "$WORK/init-failure" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["stage"] == "mods_failed" and attach["file_redirects"] == 0, attach
events = [json.loads(x) for x in (base / "hyprlodr-events.jsonl").read_text().splitlines() if x.strip()]
assert any(x.get("event") == "error" and x.get("mod") == "r.redirect" for x in events), events
assert "asset-open: hyprlodr original asset content" in (base / "client.log").read_text()
opens = [json.loads(x) for x in (base / "hyprlodr-files.jsonl").read_text().splitlines() if x.strip()]
asset = next(x for x in opens if x.get("event") == "file_open" and x.get("requested") == "asset/asset")
assert asset["redirected"] is False and asset["ok"] is True and asset["opened"] == "asset/asset", asset
waits = [x for x in opens if x.get("event") == "redirect_wait" and x.get("requested") == "asset/asset"]
assert any(x.get("stage") == "begin" for x in waits) and any(x.get("stage") == "ready" for x in waits), waits
print("init error: redirect rollback completed and the waiting open used the original path")
PY

  echo "== wine proxy: bounded timeout uses the original asset =="
  clang "${CFG[@]}" -DHYPRLODR_REDIRECT_WAIT_LIMIT_MS=100 -c \
    "$ROOT/native/win32/file_hook.c" -o "$OUT/obj/file_hook_timeout.obj"
  TIMEOUT_PROXY="$WORK/proxy-timeout/OpenAL32.dll"
  mkdir -p "$WORK/proxy-timeout"
  lld-link /nologo /dll /nodefaultlib /entry:dll_main \
    "/out:$TIMEOUT_PROXY" "/def:$OUT/OpenAL32.def" \
    "$OUT/obj/proxy.obj" "$OUT/obj/gl_hook.obj" "$OUT/obj/asset_probe.obj" \
    "$OUT/obj/file_hook_timeout.obj" "$OUT/obj/loader.obj" "$OUT/obj/anchors.obj" \
    "$OUT/obj/crt_impl.obj" "$OUT/lib/kernel32.lib" "$OUT/lib/user32.lib" "$OUT/lib/gdi32.lib"
  clang "${CFG[@]}" -DHYPRLODR_TEST_ASSET_OPEN_DELAY_MS=0 \
    -c "$ROOT/native/win32/tests/proxy_client.c" -o "$OUT/obj/proxy_client_timeout.obj"
  lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
    "/out:$OUT/tests/proxy_client_timeout.exe" "$OUT/obj/proxy_client_timeout.obj" \
    "$OUT/obj/entry.obj" "$OUT/obj/crt_impl.obj" "$OUT/lib/kernel32.lib"
  run_gate_case redirect-timeout redirect_mod_delayed.dll "$TIMEOUT_PROXY" \
    "$OUT/tests/proxy_client_timeout.exe" default
  python3 - "$WORK/redirect-timeout" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["stage"] == "mods_loaded" and attach["file_redirects"] == 2, attach
assert "asset-open: hyprlodr original asset content" in (base / "client.log").read_text()
opens = [json.loads(x) for x in (base / "hyprlodr-files.jsonl").read_text().splitlines() if x.strip()]
asset = next(x for x in opens if x.get("event") == "file_open" and x.get("requested") == "asset/asset")
assert asset["redirected"] is False and asset["ok"] is True and asset["opened"] == "asset/asset", asset
waits = [x for x in opens if x.get("event") == "redirect_wait" and x.get("requested") == "asset/asset"]
timed_out = [x for x in waits if x.get("stage") == "timeout"]
assert timed_out and timed_out[0]["waited_ms"] >= 100, waits
print(f"timeout: waited {timed_out[0]['waited_ms']} ms, fell back to the original path, then bootstrap completed")
PY
else
  echo "skipped proxy runtime test: pass the original OpenAL32.dll path as the second argument"
fi

exit $status
