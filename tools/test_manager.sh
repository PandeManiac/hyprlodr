#!/usr/bin/env bash
# Build and run the headless manager self-test suite.
#
# The suite covers Steam discovery, loader install/uninstall safety, mod catalog
# and load-order planning, version constraints, event-log parsing, package
# validation and package import. It needs no display.
#
# Usage: tools/test_manager.sh [sample-mod-library]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAMPLE="${1:-$ROOT/build/mods/z.foundation.so}"

dotnet build "$ROOT/manager/HYPRLODR.SelfTest/HYPRLODR.SelfTest.csproj" \
  -v q --nologo >/dev/null
exec dotnet "$ROOT/manager/HYPRLODR.SelfTest/bin/Debug/net10.0/HYPRLODR.SelfTest.dll" "$SAMPLE"
