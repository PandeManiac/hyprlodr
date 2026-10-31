#!/usr/bin/env python3
"""HYPRLODR read-only installation diagnostics.

Locates Steam libraries and the HYPER DEMON installation, identifies the
executable's PE architecture and imported libraries, hashes the shipped
binaries, detects Proton, and checks for an existing HYPRLODR installation.

Nothing is modified and nothing is uploaded. Only metadata is collected; the
optional --full report embeds hashes and PE metadata, never game file contents.

Usage:
    python3 tools/hyprl_diagnose.py                # human summary
    python3 tools/hyprl_diagnose.py --json out.json
    python3 tools/hyprl_diagnose.py --game-dir /path/to/hyperdemon
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import struct
import sys
from pathlib import Path

APP_ID = "1743850"
EXE_NAME = "hyperdemon.exe"
TOOL_VERSION = "0.2.0"


# --------------------------------------------------------------------- Steam


def _steam_roots() -> list[Path]:
    """Candidate Steam roots, platform aware, never hardcoded to one install."""
    home = Path.home()
    if sys.platform == "win32":
        candidates = [
            Path(os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")) / "Steam",
            Path(os.environ.get("PROGRAMFILES", r"C:\Program Files")) / "Steam",
            Path(os.environ.get("STEAM_PATH", "")) if os.environ.get("STEAM_PATH") else None,
        ]
    elif sys.platform == "darwin":
        candidates = [home / "Library" / "Application Support" / "Steam"]
    else:
        candidates = [
            home / ".steam" / "steam",
            home / ".steam" / "root",
            home / ".local" / "share" / "Steam",
            home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
            home / "snap" / "steam" / "common" / ".local" / "share" / "Steam",
        ]
    out: list[Path] = []
    for candidate in candidates:
        if candidate and candidate.is_dir():
            resolved = candidate.resolve()
            if resolved not in out:
                out.append(resolved)
    return out


def _vdf_library_paths(root: Path) -> list[Path]:
    """Read library folders from libraryfolders.vdf (tolerant, regex based)."""
    paths: list[Path] = [root]
    for vdf in (root / "steamapps" / "libraryfolders.vdf", root / "config" / "libraryfolders.vdf"):
        if not vdf.is_file():
            continue
        try:
            text = vdf.read_text(errors="replace")
        except OSError:
            continue
        for match in re.finditer(r'"path"\s+"([^"]+)"', text):
            raw = match.group(1).replace("\\\\", "\\")
            candidate = Path(raw)
            if candidate.is_dir() and candidate.resolve() not in paths:
                paths.append(candidate.resolve())
        for match in re.finditer(r'^\s*"\d+"\s+"([^"]+)"\s*$', text, re.M):
            raw = match.group(1).replace("\\\\", "\\")
            candidate = Path(raw)
            if (candidate / "steamapps").is_dir() and candidate.resolve() not in paths:
                paths.append(candidate.resolve())
    return paths


def parse_acf(path: Path) -> dict[str, str]:
    data: dict[str, str] = {}
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return data
    for key, value in re.findall(r'"([A-Za-z0-9_]+)"\s+"([^"]*)"', text):
        data.setdefault(key, value)
    return data


def find_installations(explicit: str | None) -> list[dict]:
    installs: list[dict] = []
    if explicit:
        game = Path(explicit).expanduser().resolve()
        installs.append({"library": str(game.parent), "path": str(game), "source": "manual"})
        return installs

    for root in _steam_roots():
        for library in _vdf_library_paths(root):
            manifest = library / "steamapps" / f"appmanifest_{APP_ID}.acf"
            info = parse_acf(manifest) if manifest.is_file() else {}
            installdir = info.get("installdir", "hyperdemon")
            game = library / "steamapps" / "common" / installdir
            if (game / EXE_NAME).is_file():
                installs.append({
                    "library": str(library),
                    "path": str(game),
                    "source": "steam",
                    "steam_root": str(root),
                    "appmanifest": str(manifest) if manifest.is_file() else None,
                    "buildid": info.get("buildid"),
                    "state_flags": info.get("StateFlags"),
                    "size_on_disk": info.get("SizeOnDisk"),
                    "name": info.get("name"),
                })
    return installs


# ------------------------------------------------------------------ PE reader


class PEInfo:
    """Minimal read-only PE parser: architecture, subsystem, imports, exports."""

    MACHINES = {0x8664: "x86-64", 0x14C: "x86", 0xAA64: "arm64"}
    SUBSYSTEMS = {2: "windows-gui", 3: "console"}

    def __init__(self, path: Path):
        self.path = path
        self.error: str | None = None
        self.machine = "unknown"
        self.subsystem = "unknown"
        self.is_pe = False
        self.imports: list[str] = []
        self.exports: list[str] = []
        self.forwarders: list[dict[str, str]] = []
        self.image_base = 0
        self.timestamp = None
        self.characteristics: list[str] = []
        try:
            self._data = path.read_bytes()
        except OSError as exc:
            self.error = f"cannot read: {exc}"
            self._data = b""
            return
        try:
            self._parse()
        except Exception as exc:  # malformed binaries must not crash diagnostics
            self.error = f"malformed PE: {exc}"

    def _parse(self) -> None:
        data = self._data
        if len(data) < 0x40 or data[:2] != b"MZ":
            self.error = "not a PE image (missing MZ header)"
            return
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            self.error = "not a PE image (missing PE signature)"
            return
        self.is_pe = True
        coff = e_lfanew + 4
        machine, nsections, timestamp = struct.unpack_from("<HHI", data, coff)
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        self.machine = self.MACHINES.get(machine, f"0x{machine:04x}")
        self.timestamp = timestamp
        characteristics = struct.unpack_from("<H", data, coff + 18)[0]
        if characteristics & 0x0020:
            self.characteristics.append("large-address-aware")
        if characteristics & 0x2000:
            self.characteristics.append("dll")

        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic == 0x20B:
            self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
            dd = opt + 112
        elif magic == 0x10B:
            self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
            dd = opt + 96
        else:
            self.error = f"unsupported optional header magic 0x{magic:04x}"
            return
        self.subsystem = self.SUBSYSTEMS.get(struct.unpack_from("<H", data, opt + 68)[0], "other")

        sections = []
        for index in range(nsections):
            offset = opt + opt_size + index * 40
            name = data[offset:offset + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, offset + 8)
            sections.append((name, vaddr, max(vsize, rawsize), rawptr))

        def rva_to_offset(rva: int) -> int | None:
            for _, vaddr, size, rawptr in sections:
                if vaddr <= rva < vaddr + size:
                    return rawptr + (rva - vaddr)
            return None

        def read_cstr(rva: int) -> str:
            offset = rva_to_offset(rva)
            if offset is None:
                return ""
            end = data.find(b"\0", offset)
            return data[offset:end if end >= 0 else offset + 256].decode("latin-1", "replace")

        import_rva = struct.unpack_from("<I", data, dd + 8)[0]
        offset = rva_to_offset(import_rva) if import_rva else None
        while offset is not None and offset + 20 <= len(data):
            desc = struct.unpack_from("<IIIII", data, offset)
            if not any(desc):
                break
            name = read_cstr(desc[3])
            if name:
                self.imports.append(name)
            offset += 20

        export_rva = struct.unpack_from("<I", data, dd)[0]
        if export_rva:
            exp = rva_to_offset(export_rva)
            if exp is not None:
                (_, _, _, _, _, base, nfuncs, nnames,
                 addr_funcs, addr_names, addr_ordinals) = struct.unpack_from("<IIHHIIIIIII", data, exp)
                names_off = rva_to_offset(addr_names)
                ords_off = rva_to_offset(addr_ordinals)
                funcs_off = rva_to_offset(addr_funcs)
                for index in range(min(nnames, 4096)):
                    if names_off is None or ords_off is None:
                        break
                    name_rva = struct.unpack_from("<I", data, names_off + index * 4)[0]
                    ordinal = struct.unpack_from("<H", data, ords_off + index * 2)[0]
                    symbol = read_cstr(name_rva)
                    target = ""
                    if funcs_off is not None and ordinal < nfuncs and ordinal < 4096:
                        target_rva = struct.unpack_from("<I", data, funcs_off + ordinal * 4)[0]
                        if export_rva <= target_rva < export_rva + 0x10000:
                            target = read_cstr(target_rva)
                    self.exports.append(symbol)
                    if target:
                        self.forwarders.append({"export": symbol, "forward": target})
                if not self.exports and nfuncs:
                    self.exports = [f"#{base + i}" for i in range(min(nfuncs, 64))]

    def as_dict(self) -> dict:
        return {
            "file": str(self.path),
            "is_pe": self.is_pe,
            "architecture": self.machine,
            "subsystem": self.subsystem,
            "image_base": hex(self.image_base) if self.image_base else None,
            "characteristics": self.characteristics,
            "imports": sorted(self.imports),
            "import_count": len(self.imports),
            "export_count": len(self.exports),
            "exports_sample": self.exports[:8],
            "exports": self.exports,
            "forwarders": self.forwarders,
            "error": self.error,
        }


# ---------------------------------------------------------------- file facts


def sha256(path: Path, chunk: int = 1 << 20) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(chunk), b""):
            digest.update(block)
    return digest.hexdigest()


def file_facts(path: Path) -> dict:
    try:
        stat = path.stat()
    except OSError as exc:
        return {"file": str(path), "error": str(exc)}
    return {
        "file": str(path),
        "name": path.name,
        "size": stat.st_size,
        "sha256": sha256(path),
    }


# ------------------------------------------------------- HYPRLODR presence


def hyprlodr_state(game: Path) -> dict:
    mods = game / "mods"
    markers = {
        "proxy_installed": (game / "OpenAL32.dll").is_file()
        and (game / "OpenAL32_real.dll").is_file(),
        "original_backup": (game / "OpenAL32.dll.orig").is_file(),
        "mods_directory": mods.is_dir(),
        "installed_mods": sorted(p.name for p in mods.iterdir()
                                 if p.suffix.lower() in (".dll", ".so")) if mods.is_dir() else [],
        "disabled_list": (mods / "disabled.txt").is_file(),
        "event_logs": sorted(p.name for p in game.glob("hyprlodr*.jsonl")),
        "loader_directory": (game / "hyprlodr").is_dir(),
    }
    return markers


def proton_state(library: Path) -> dict:
    result: dict = {"compatdata": None, "proton_versions": [], "prefix": None, "wine_overrides": None}
    compatdata = library / "steamapps" / "compatdata" / APP_ID
    result["compatdata"] = str(compatdata) if compatdata.is_dir() else None
    prefix = compatdata / "pfx"
    result["prefix"] = str(prefix) if prefix.is_dir() else None
    common = library / "steamapps" / "common"
    if common.is_dir():
        result["proton_versions"] = sorted(p.name for p in common.iterdir()
                                           if p.is_dir() and p.name.lower().startswith("proton"))
    config = compatdata / "config_info"
    if config.is_file():
        try:
            text = config.read_text(errors="replace")
        except OSError:
            text = ""
        for line in text.splitlines():
            if "WINEDLLOVERRIDES" in line:
                result["wine_overrides"] = line.strip()
    return result


def build_report(explicit: str | None) -> dict:
    report: dict = {
        "tool": "hyprlodr-diagnose",
        "tool_version": TOOL_VERSION,
        "app_id": APP_ID,
        "host": {
            "platform": platform.platform(),
            "system": sys.platform,
            "python": platform.python_version(),
        },
        "steam_roots": [str(p) for p in _steam_roots()],
        "installations": [],
        "notes": [],
    }
    installs = find_installations(explicit)
    if not installs:
        report["notes"].append(
            "No HYPER DEMON installation found. Pass --game-dir with the folder that "
            "contains hyperdemon.exe."
        )
        return report

    for install in installs:
        game = Path(install["path"])
        library = Path(install["library"])
        entry = dict(install)
        entry["files"] = []
        entry["binaries"] = {}
        for name in (EXE_NAME, "OpenAL32.dll", "steam_api64.dll", "discord_game_sdk.dll"):
            path = game / name
            if not path.is_file():
                continue
            facts = file_facts(path)
            entry["files"].append(facts)
            if name == EXE_NAME:
                entry["binaries"][name] = PEInfo(path).as_dict()
            else:
                info = PEInfo(path)
                entry["binaries"][name] = {
                    "architecture": info.machine,
                    "exports": len(info.exports),
                    "exports_sample": info.exports[:12],
                    "imports": sorted(info.imports),
                    "forwarders": info.forwarders[:8],
                }
        asset = game / "asset" / "asset"
        entry["asset_pack"] = file_facts(asset) if asset.is_file() else None
        entry["hyprlodr"] = hyprlodr_state(game)
        entry["proton"] = proton_state(library)
        report["installations"].append(entry)

    exe = Path(installs[0]["path"]) / EXE_NAME
    if exe.is_file():
        info = PEInfo(exe)
        if info.machine != "x86-64":
            report["notes"].append(f"Unexpected executable architecture: {info.machine}")
        if "OpenAL32.dll" not in info.imports:
            report["notes"].append(
                "hyperdemon.exe does not statically import OpenAL32.dll; DLL proxying "
                "for that library is not a viable integration point for this build."
            )
        else:
            report["notes"].append(
                "hyperdemon.exe statically imports OpenAL32.dll, so a replacement library "
                "in the game directory is loaded by the Windows loader before game code runs."
            )
    return report


def print_human(report: dict) -> None:
    print(f"HYPRLODR diagnostics {report['tool_version']} on {report['host']['platform']}")
    print(f"Steam roots: {', '.join(report['steam_roots']) or 'none found'}")
    for install in report["installations"]:
        print(f"\nInstallation: {install['path']}")
        print(f"  library      : {install['library']}")
        print(f"  source       : {install['source']}")
        if install.get("buildid"):
            print(f"  Steam buildid: {install['buildid']} (StateFlags {install.get('state_flags')})")
        for facts in install["files"]:
            print(f"  {facts['name']:<20} {facts['size']:>10}  {facts['sha256']}")
        exe = install["binaries"].get(EXE_NAME, {})
        print(f"  architecture : {exe.get('architecture')} / {exe.get('subsystem')}")
        print(f"  imports      : {', '.join(exe.get('imports', []))}")
        print(f"  exports      : {exe.get('exports') or 'none'}")
        proton = install["proton"]
        print(f"  compatdata   : {proton['compatdata'] or 'none'}")
        print(f"  proton       : {', '.join(proton['proton_versions']) or 'none'}")
        if proton["wine_overrides"]:
            print(f"  winsettings  : {proton['wine_overrides']}")
        state = install["hyprlodr"]
        print(f"  HYPRLODR     : proxy={state['proxy_installed']} mods_dir={state['mods_directory']} "
              f"mods={len(state['installed_mods'])} logs={len(state['event_logs'])}")
    if report["notes"]:
        print("\nNotes:")
        for note in report["notes"]:
            print(f"  - {note}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Read-only HYPER DEMON installation diagnostics")
    parser.add_argument("--game-dir", help="folder containing hyperdemon.exe")
    parser.add_argument("--json", help="write the structured report to this path")
    args = parser.parse_args(argv)

    report = build_report(args.game_dir)
    print_human(report)
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
        print(f"\nJSON report written to {args.json}")
    return 0 if report["installations"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
