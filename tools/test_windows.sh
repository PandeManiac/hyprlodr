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

  CFG=(--target=x86_64-pc-windows-msvc -std=c17 -O2 -ffreestanding -nostdlibinc
       -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror
       -DHYPRLODR_MINIMAL_WIN32
       -I"$ROOT/native/win32/crt" -I"$ROOT/native/win32")
  clang "${CFG[@]}" -c "$ROOT/native/win32/tests/proxy_client.c" -o "$OUT/obj/proxy_client.obj"
  lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
    "/out:$WORK/proxy/proxy_client.exe" \
    "$OUT/obj/proxy_client.obj" "$OUT/obj/entry.obj" "$OUT/obj/crt_impl.obj" \
    "$OUT/lib/kernel32.lib"

  rm -f "$WORK/proxy/hyprlodr-config.json"
  ( cd "$WORK/proxy" && timeout 90 wine ./proxy_client.exe ) || status=1

  python3 - "$WORK/proxy" 2500 <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["stage"] in ("mods_loaded", "mods_failed"), attach
print(f"proxy attach: stage={attach['stage']} pid={attach['pid']} status_text={attach['status_text']}")
# The test host is proxy_client.exe, not hyperdemon.exe. The fingerprint must
# say so: an unknown build is reported, never assumed compatible.
assert attach["module_parsed"] is True, attach
assert attach["anchors_total"] > 0, attach
assert attach["build_matches"] is False, attach
assert attach["anchors_matched"] < attach["anchors_total"], attach
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
assert init == ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"], init
print("proxy bootstrap: mods initialized inside the host process")
print("crash filter: silent on a clean exit")
PY
  echo "== wine proxy: hyprlodr-config.json overrides the delay =="
  printf '{\n  "delay_ms": 0\n}\n' > "$WORK/proxy/hyprlodr-config.json"
  rm -f "$WORK/proxy/hyprlodr-attach.json" "$WORK/proxy/hyprlodr-events.jsonl"
  ( cd "$WORK/proxy" && timeout 90 wine ./proxy_client.exe ) || status=1

  python3 - "$WORK/proxy" <<'PY' || status=1
import json, pathlib, sys
base = pathlib.Path(sys.argv[1])
attach = json.loads((base / "hyprlodr-attach.json").read_text())
assert attach["delay_ms"] == 0, attach
assert attach["stage"] == "mods_loaded", attach
events = [json.loads(line) for line in (base / "hyprlodr-events.jsonl").read_text().splitlines() if line.strip()]
init = [e["mod"] for e in events if e["event"] == "initialize_ok"]
assert init == ["z.foundation", "a.palette", "b.audio", "m.overlay", "00.final"], init
print("config: delay_ms=0 honoured, all five mods still initialized")
PY
else
  echo "skipped proxy runtime test: pass the original OpenAL32.dll path as the second argument"
fi

exit $status
