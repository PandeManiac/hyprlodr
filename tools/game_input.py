#!/usr/bin/env python3
"""Drive the real game with a kernel-level virtual mouse and keyboard.

The game reads raw device input, so XTest events are discarded (docs/RE_TITLE.md).
A device created through /dev/uinput is a real device from the kernel's point of
view and reaches the game's input path. This tool creates one relative mouse and
one keyboard, then replays a short script:

    python3 tools/game_input.py rel:0,400 key:space keydown:w sleep:1200 keyup:w

Actions:
    rel:DX,DY        relative mouse motion (for mouselook)
    key:NAME         tap a key
    keydown:NAME     hold a key
    keyup:NAME       release a key
    click / rclick   left / right mouse button
    sleep:MS         wait

It is a test instrument, not part of the mod. No game data is written.
"""
from __future__ import annotations

import argparse
import fcntl
import os
import struct
import sys
import time

UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_DEV_SETUP = 0x405C5503
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_RELBIT = 0x40045566

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
SYN_REPORT = 0
REL_X, REL_Y = 0, 1
BUS_VIRTUAL = 0x06

KEYS = {
    "esc": 1, "enter": 28, "space": 57, "tab": 15, "backspace": 14,
    "leftshift": 42, "rightshift": 54, "leftctrl": 29, "rightctrl": 97,
    "w": 17, "a": 30, "s": 31, "d": 32,
    "q": 16, "e": 18, "r": 19, "f": 33,
    "1": 2, "2": 3, "3": 4, "4": 5, "5": 6,
    "left": 105, "right": 106, "up": 103, "down": 108,
}
BTN_LEFT, BTN_RIGHT = 0x110, 0x111


def emit(fd: int, kind: int, code: int, value: int) -> None:
    os.write(fd, struct.pack("@llHHi", 0, 0, kind, code, value))


def sync(fd: int) -> None:
    emit(fd, EV_SYN, SYN_REPORT, 0)


def open_device(name: str) -> int:
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
    for code in (BTN_LEFT, BTN_RIGHT, *KEYS.values()):
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for axis in (REL_X, REL_Y):
        fcntl.ioctl(fd, UI_SET_RELBIT, axis)
    setup = (struct.pack("@HHHH", BUS_VIRTUAL, 0x1D6B, 0x0105, 1)
             + name.encode()[:79].ljust(80, b"\0") + struct.pack("@I", 0))
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(0.4)
    return fd


def close_device(fd: int) -> None:
    try:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
    finally:
        os.close(fd)


def tap(fd: int, name: str) -> None:
    code = KEYS[name]
    emit(fd, EV_KEY, code, 1)
    sync(fd)
    time.sleep(0.045)
    emit(fd, EV_KEY, code, 0)
    sync(fd)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("actions", nargs="+")
    parser.add_argument("--delay", type=float, default=0.06)
    args = parser.parse_args(argv)

    fd = open_device("HYPRLODR game input")
    try:
        for action in args.actions:
            verb, _, argument = action.partition(":")
            if verb == "rel":
                dx, dy = (int(part) for part in argument.split(","))
                # Split large deltas so a client that accumulates raw motion
                # sees a smooth sweep rather than one enormous jump.
                steps = max(1, min(20, max(abs(dx), abs(dy)) // 40))
                for _ in range(steps):
                    emit(fd, EV_REL, REL_X, dx // steps)
                    emit(fd, EV_REL, REL_Y, dy // steps)
                    sync(fd)
                    time.sleep(0.012)
            elif verb == "key":
                tap(fd, argument)
            elif verb == "keydown":
                emit(fd, EV_KEY, KEYS[argument], 1)
                sync(fd)
            elif verb == "keyup":
                emit(fd, EV_KEY, KEYS[argument], 0)
                sync(fd)
            elif verb == "click":
                emit(fd, EV_KEY, BTN_LEFT, 1)
                sync(fd)
                time.sleep(0.05)
                emit(fd, EV_KEY, BTN_LEFT, 0)
                sync(fd)
            elif verb == "rclick":
                emit(fd, EV_KEY, BTN_RIGHT, 1)
                sync(fd)
                time.sleep(0.05)
                emit(fd, EV_KEY, BTN_RIGHT, 0)
                sync(fd)
            elif verb == "sleep":
                time.sleep(int(argument) / 1000.0)
            else:
                raise SystemExit(f"error: unknown action {action!r}")
            time.sleep(args.delay)
    finally:
        close_device(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
