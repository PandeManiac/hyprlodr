#!/usr/bin/env bash
# Cross-build the Windows x64 HYPRLODR loader, synthetic host, sample mods and
# the OpenAL32 proxy DLL using only clang + lld + llvm-dlltool.
#
# No Windows SDK, mingw sysroot, Wine development package or C runtime is
# required: native/win32/crt_impl.c supplies the small libc subset the loader
# uses, and the kernel32 import library is generated from a .def file.
#
# Usage: tools/build_windows.sh [output-directory] [path-to-OpenAL32.dll]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build-win}"
ORIGINAL_OPENAL="${2:-}"

TARGET=x86_64-pc-windows-msvc
CC="${CC:-clang}"
CFLAGS=(
  "--target=$TARGET" -std=c17 -O2 -g
  -ffreestanding -nostdlibinc -fno-stack-protector -mno-stack-arg-probe
  -Wall -Wextra -Werror -DHYPRLODR_MINIMAL_WIN32
  -I"$ROOT/native/win32/crt"
  -I"$ROOT/native/win32"
  -I"$ROOT/native/include"
  -I"$ROOT/native/src"
)

mkdir -p "$OUT/obj" "$OUT/lib" "$OUT/mods"

echo "== import library =="
llvm-dlltool -m i386:x86-64 -d "$ROOT/native/win32/kernel32.def" -l "$OUT/lib/kernel32.lib"

compile() { # compile <source> <object> [extra flags...]
  local source="$1" object="$2"; shift 2
  echo "  CC $object"
  "$CC" "${CFLAGS[@]}" "$@" -c "$source" -o "$object"
}

compile "$ROOT/native/src/loader.c" "$OUT/obj/loader.obj"
compile "$ROOT/native/src/anchors.c" "$OUT/obj/anchors.obj"
compile "$ROOT/native/src/host.c" "$OUT/obj/host.obj"
compile "$ROOT/native/win32/proxy.c" "$OUT/obj/proxy.obj"
compile "$ROOT/native/win32/file_hook.c" "$OUT/obj/file_hook.obj"
# -fno-builtin keeps the runtime's own memcpy/memset implementations from
# turning into calls to themselves.
compile "$ROOT/native/win32/crt_impl.c" "$OUT/obj/crt_impl.obj" -fno-builtin
# Executable-only entry shim; the DLL uses dll_main instead.
compile "$ROOT/native/win32/entry.c" "$OUT/obj/entry.obj"

CRT_OBJ="$OUT/obj/crt_impl.obj"
K32="$OUT/lib/kernel32.lib"

echo "== host =="
lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
  "/out:$OUT/hyprlodr-host.exe" \
  "$OUT/obj/loader.obj" "$OUT/obj/host.obj" "$OUT/obj/entry.obj" "$CRT_OBJ" "$K32"

echo "== sample mods =="
link_mod() { # link_mod <source> <mod-id>
  local source="$1" id="$2"
  local object="$OUT/obj/mod_${id//[.\/]/_}.obj"
  echo "  MOD $id"
  "$CC" "${CFLAGS[@]}" -c "$source" -o "$object"
  lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/mods/$id.dll" \
    "$object" "$CRT_OBJ" "$K32"
}

link_mod "$ROOT/samples/foundation.c" "z.foundation"
link_mod "$ROOT/samples/palette.c" "a.palette"
link_mod "$ROOT/samples/audio.c" "b.audio"
link_mod "$ROOT/samples/overlay.c" "m.overlay"
link_mod "$ROOT/samples/final.c" "00.final"

echo "== wine-test redirect mod =="
mkdir -p "$OUT/tests"
"$CC" "${CFLAGS[@]}" -c "$ROOT/native/win32/tests/redirect_mod.c" -o "$OUT/obj/redirect_mod.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/tests/redirect_mod.dll" \
  "$OUT/obj/redirect_mod.obj" "$CRT_OBJ" "$K32"


echo "== example mod =="
mkdir -p "$OUT/examples"
"$CC" "${CFLAGS[@]}" -c "$ROOT/examples/uptime.c" -o "$OUT/obj/example_uptime.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/examples/example.uptime.dll" \
  "$OUT/obj/example_uptime.obj" "$CRT_OBJ" "$K32"
echo "  MOD example.uptime (in $OUT/examples, not copied into the game by default)"

# Stage the example as a real package, so the documented authoring path is the
# path that is actually exercised rather than described.
if [ -f "$ROOT/examples/package/manifest.json" ]; then
  rm -rf "$OUT/example-package"
  mkdir -p "$OUT/example-package/native/windows-x86_64"
  cp "$ROOT/examples/package/manifest.json" "$OUT/example-package/"
  cp "$OUT/examples/example.uptime.dll" "$OUT/example-package/native/windows-x86_64/"
  python3 "$ROOT/tools/hyprl_package.py" pack "$OUT/example-package" "$OUT/examples/example.uptime.zip"
fi

if [ -n "$ORIGINAL_OPENAL" ]; then
  echo "== OpenAL32 proxy =="
  python3 "$ROOT/tools/gen_proxy_def.py" "$ORIGINAL_OPENAL" "$OUT/OpenAL32.def" OpenAL32_real
  lld-link /nologo /dll /nodefaultlib /entry:dll_main "/out:$OUT/OpenAL32.dll" \
    /def:"$OUT/OpenAL32.def" \
    "$OUT/obj/proxy.obj" "$OUT/obj/file_hook.obj" "$OUT/obj/loader.obj" \
    "$OUT/obj/anchors.obj" "$CRT_OBJ" "$K32"
  echo "built $OUT/OpenAL32.dll (loads mods from <game>/mods)"
else
  echo "skipped proxy DLL: pass the game's OpenAL32.dll path as the second argument"
fi
