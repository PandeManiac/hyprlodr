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
llvm-dlltool -m i386:x86-64 -d "$ROOT/native/win32/user32.def" -l "$OUT/lib/user32.lib"
llvm-dlltool -m i386:x86-64 -d "$ROOT/native/win32/gdi32.def" -l "$OUT/lib/gdi32.lib"

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
compile "$ROOT/native/win32/gl_hook.c" "$OUT/obj/gl_hook.obj"
compile "$ROOT/native/win32/asset_probe.c" "$OUT/obj/asset_probe.obj"
# -fno-builtin keeps the runtime's own memcpy/memset implementations from
# turning into calls to themselves.
compile "$ROOT/native/win32/crt_impl.c" "$OUT/obj/crt_impl.obj" -fno-builtin
# Executable-only entry shim; the DLL uses dll_main instead.
compile "$ROOT/native/win32/entry.c" "$OUT/obj/entry.obj"

CRT_OBJ="$OUT/obj/crt_impl.obj"
K32="$OUT/lib/kernel32.lib"
U32="$OUT/lib/user32.lib"
G32="$OUT/lib/gdi32.lib"

echo "== host =="
lld-link /nologo /nodefaultlib /entry:hyprlodr_entry /subsystem:console \
  "/out:$OUT/hyprlodr-host.exe" \
  "$OUT/obj/loader.obj" "$OUT/obj/host.obj" "$OUT/obj/anchors.obj" "$OUT/obj/entry.obj" "$CRT_OBJ" "$K32"

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
# ABI 5 fixture: reads the running image header. Used for the real-game proof;
# it is not part of the Wine suite, whose host has no verified image.
"$CC" "${CFLAGS[@]}" -c "$ROOT/native/win32/tests/image_mod.c" -o "$OUT/obj/image_mod.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/tests/image_mod.dll" \
  "$OUT/obj/image_mod.obj" "$CRT_OBJ" "$K32"
# ABI 6 fixture: patches one verified byte in-game and restores it immediately.
"$CC" "${CFLAGS[@]}" -c "$ROOT/native/win32/tests/image_writer.c" -o "$OUT/obj/image_writer.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/tests/image_writer.dll" \
  "$OUT/obj/image_writer.obj" "$CRT_OBJ" "$K32"
# ABI 4 on the real asset path: redirect the asset container to an identical file.
"$CC" "${CFLAGS[@]}" -c "$ROOT/native/win32/tests/asset_redirect.c" -o "$OUT/obj/asset_redirect.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/tests/asset_redirect.dll" \
  "$OUT/obj/asset_redirect.obj" "$CRT_OBJ" "$K32"



echo "== example mod =="
mkdir -p "$OUT/examples"
"$CC" "${CFLAGS[@]}" -c "$ROOT/examples/uptime.c" -o "$OUT/obj/example_uptime.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/examples/example.uptime.dll" \
  "$OUT/obj/example_uptime.obj" "$CRT_OBJ" "$K32"
echo "  MOD example.uptime (in $OUT/examples, not copied into the game by default)"
"$CC" "${CFLAGS[@]}" -c "$ROOT/examples/game_info.c" -o "$OUT/obj/example_game_info.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/examples/example.game-info.dll" \
  "$OUT/obj/example_game_info.obj" "$CRT_OBJ" "$K32"
echo "  MOD example.game-info (in $OUT/examples, ABI 5 image reader)"
"$CC" "${CFLAGS[@]}" -c "$ROOT/examples/title_replacement.c" -o "$OUT/obj/example_title_replacement.obj"
lld-link /nologo /dll /noentry /nodefaultlib "/out:$OUT/examples/example.title-replacement.dll" \
  "$OUT/obj/example_title_replacement.obj" "$CRT_OBJ" "$K32"
echo "  MOD example.title-replacement (in $OUT/examples, ships the title mask as a package file)"

# Stage the example as a real package, so the documented authoring path is the
# path that is actually exercised rather than described.
if [ -f "$ROOT/examples/package/manifest.json" ]; then
  rm -rf "$OUT/example-package"
  mkdir -p "$OUT/example-package/native/windows-x86_64"
  cp "$ROOT/examples/package/manifest.json" "$OUT/example-package/"
  cp "$OUT/examples/example.uptime.dll" "$OUT/example-package/native/windows-x86_64/"
  python3 "$ROOT/tools/hyprl_package.py" pack "$OUT/example-package" "$OUT/examples/example.uptime.zip"
fi

# The title replacement is data plus a small mod, so its package is staged the same way.
# The mask is generated, never committed: render it with tools/title_mask_from_game.py first.
if [ -f "$ROOT/examples/package-title/manifest.json" ] && [ -f "$ROOT/assets/hyprlodr-title-mask.raw" ]; then
  rm -rf "$OUT/title-package"
  mkdir -p "$OUT/title-package/native/windows-x86_64" "$OUT/title-package/assets"
  cp "$ROOT/examples/package-title/manifest.json" "$OUT/title-package/"
  cp "$OUT/examples/example.title-replacement.dll" "$OUT/title-package/native/windows-x86_64/"
  cp "$ROOT/assets/hyprlodr-title-mask.raw" "$OUT/title-package/assets/title-mask.raw"
  python3 "$ROOT/tools/hyprl_package.py" pack "$OUT/title-package" "$OUT/examples/example.title-replacement.zip"
else
  echo "  skipped example.title-replacement package: generate the mask first (tools/title_mask_from_game.py)"
fi

if [ -n "$ORIGINAL_OPENAL" ]; then
  echo "== OpenAL32 proxy =="
  python3 "$ROOT/tools/gen_proxy_def.py" "$ORIGINAL_OPENAL" "$OUT/OpenAL32.def" OpenAL32_real
  lld-link /nologo /dll /nodefaultlib /entry:dll_main "/out:$OUT/OpenAL32.dll" \
    /def:"$OUT/OpenAL32.def" \
    "$OUT/obj/proxy.obj" "$OUT/obj/gl_hook.obj" "$OUT/obj/asset_probe.obj" "$OUT/obj/file_hook.obj" \
    "$OUT/obj/loader.obj" "$OUT/obj/anchors.obj" "$CRT_OBJ" "$K32" "$U32" "$G32"
  echo "built $OUT/OpenAL32.dll (loads mods from <game>/mods)"
else
  echo "skipped proxy DLL: pass the game's OpenAL32.dll path as the second argument"
fi
