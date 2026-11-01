#!/usr/bin/env python3
"""Prepare the HYPRLODR title screen image.

The loader's title overlay reads an uncompressed 24/32-bit BMP, because the
Windows target has no image decoder and pulling one in would mean shipping a
third-party library into the game process. This tool converts whatever the author
has (PNG, JPEG, ...) into that BMP, and can draw a placeholder so the feature is
usable before real art exists.

Usage:
    hyprl_title.py convert artwork.png "<game>/hyprlodr-title.bmp" [--width 1920]
    hyprl_title.py placeholder "<game>/hyprlodr-title.bmp" [--width 1920] [--text HYPRLODR]

Requires Pillow (imported as PIL). The overlay window scales the image to the
game window, so any reasonable resolution works; 1920x1080 is a good default.
"""
from __future__ import annotations

import argparse
import sys

from PIL import Image, ImageDraw, ImageFont


def _default_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for candidate in (
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default(size=size)


def convert(source: str, destination: str, width: int) -> int:
    image = Image.open(source).convert("RGBA")
    if width and image.width != width:
        height = max(1, round(image.height * width / image.width))
        image = image.resize((width, height), Image.LANCZOS)
    # The overlay derives transparency from brightness for 24-bit BMPs, so keep
    # the art's black background and let the loader punch it out.
    image.convert("RGB").save(destination, format="BMP")
    print(f"wrote {destination} ({image.width}x{image.height})")
    return 0


def placeholder(destination: str, width: int, text: str) -> int:
    height = round(width * 9 / 16)
    image = Image.new("RGB", (width, height), (0, 0, 0))
    draw = ImageDraw.Draw(image)
    # Warm diagonal gradient behind the wordmark, echoing the art direction.
    for step in range(0, height, 2):
        ratio = step / max(1, height - 1)
        draw.line(
            [(0, step), (width, step)],
            fill=(int(90 + 120 * ratio), int(40 + 40 * ratio), int(10 + 20 * ratio)),
        )
    overlay = Image.new("RGB", (width, height), (0, 0, 0))
    font = _default_font(max(24, width // 6))
    text_width, text_height = draw.textbbox((0, 0), text, font=font)[2:]
    overlay_draw = ImageDraw.Draw(overlay)
    overlay_draw.text(
        ((width - text_width) // 2, (height - text_height) // 2),
        text,
        font=font,
        fill=(255, 255, 255),
    )
    masked = Image.composite(image, Image.new("RGB", (width, height), (0, 0, 0)), overlay.convert("L"))
    masked.save(destination, format="BMP")
    print(f"wrote placeholder {destination} ({width}x{height})")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    convert_parser = sub.add_parser("convert", help="convert an image to the overlay BMP")
    convert_parser.add_argument("source")
    convert_parser.add_argument("destination")
    convert_parser.add_argument("--width", type=int, default=1920)

    placeholder_parser = sub.add_parser("placeholder", help="draw a placeholder BMP")
    placeholder_parser.add_argument("destination")
    placeholder_parser.add_argument("--width", type=int, default=1920)
    placeholder_parser.add_argument("--text", default="HYPRLODR")

    args = parser.parse_args(argv)
    if args.command == "convert":
        return convert(args.source, args.destination, args.width)
    return placeholder(args.destination, args.width, args.text)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
