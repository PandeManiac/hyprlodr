#!/usr/bin/env python3
"""Drive the game through a virtual input device, for testing a real menu round trip.

XTEST-injected events are discarded by the game (see docs/RE_TITLE.md), because it reads input the
way a client reading raw device input does. A device created through ``/dev/uinput`` is not a
synthetic event source: the X server and the game see a real pointer and keyboard, which is what
makes a menu test possible on a headless X server at all.

The pointer is absolute (a single-touch tablet), so a click lands on exact screen coordinates
instead of depending on pointer acceleration, which relative motion would.

Usage:
    python3 tools/virtual_input.py "move:371,532" "click" "sleep:800" "key:esc"
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
UI_ABS_SETUP = 0x401C5504
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
ABS_X, ABS_Y = 0, 1

BUS_VIRTUAL = 0x06
BTN_LEFT = 0x110

KEYS = {
    "esc": 1, "enter": 28, "space": 57, "tab": 15, "backspace": 14,
    "left": 105, "right": 106, "up": 103, "down": 108,
    "1": 2, "2": 3, "3": 4, "4": 5, "5": 6,
}
KEY_NAMES = {value: name for name, value in KEYS.items()}

SCREEN = (1920, 1080)


def emit(fd: int, kind: int, code: int, value: int) -> None:
    os.write(fd, struct.pack("@llHHi", 0, 0, kind, code, value))


def sync(fd: int) -> None:
    emit(fd, EV_SYN, SYN_REPORT, 0)


def open_device(name: str) -> int:
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    for code in (BTN_LEFT, *KEYS.values()):
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for axis, (low, high) in ((ABS_X, (0, SCREEN[0] - 1)), (ABS_Y, (0, SCREEN[1] - 1))):
        fcntl.ioctl(fd, UI_SET_ABSBIT, axis)
        info = struct.pack("@iiiiii", 0, low, high, 0, 0, 0)
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack("@HH", axis, 0) + info)
    setup = (struct.pack("@HHHH", BUS_VIRTUAL, 0x1D6B, 0x0104, 1)
             + name.encode()[:79].ljust(80, b"\0") + struct.pack("@I", 0))
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(0.4)                     # let the server hotplug it
    return fd


def close_device(fd: int) -> None:
    try:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
    finally:
        os.close(fd)


def move(fd: int, x: int, y: int) -> None:
    emit(fd, EV_ABS, ABS_X, max(0, min(SCREEN[0] - 1, x)))
    emit(fd, EV_ABS, ABS_Y, max(0, min(SCREEN[1] - 1, y)))
    sync(fd)


def click(fd: int) -> None:
    emit(fd, EV_KEY, BTN_LEFT, 1)
    sync(fd)
    time.sleep(0.06)
    emit(fd, EV_KEY, BTN_LEFT, 0)
    sync(fd)


def press(fd: int, name: str) -> None:
    if name not in KEYS:
        raise SystemExit(f"error: unknown key {name!r}; known: {sorted(KEYS)}")
    code = KEYS[name]
    emit(fd, EV_KEY, code, 1)
    sync(fd)
    time.sleep(0.05)
    emit(fd, EV_KEY, code, 0)
    sync(fd)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("actions", nargs="+",
                        help='"move:X,Y", "click", "key:NAME" or "sleep:MS"')
    parser.add_argument("--delay", type=float, default=0.25,
                        help="seconds between actions (default: %(default)s)")
    args = parser.parse_args(argv)

    fd = open_device("HYPRLODR test input")
    try:
        for action in args.actions:
            verb, _, argument = action.partition(":")
            if verb == "move":
                x, y = (int(part) for part in argument.split(","))
                move(fd, x, y)
            elif verb == "click":
                click(fd)
            elif verb == "key":
                press(fd, argument)
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
