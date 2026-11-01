#!/usr/bin/env python3
"""Render the title glyph mask that the game's own title pass consumes.

Measured in the running game: the title fragment shader samples one 853x480
texture whose RGB is zero everywhere and whose alpha holds the letterforms, then
composites `(1.0 - alpha) * background` inside an 854x480 window. This tool
produces that same texture for different words, from one of the game's own fonts,
so the replacement is drawn by the original shader over the original animated
background and through the original post chain.

Two properties are matched to the shipped mask on purpose:

- the layout: two bands of oversized caps filling the width, letters running off
  the side edges, separated by a horizontal gap;
- the density: `--target-ink` fits the glyph scale until the fraction of inked
  pixels matches the measured original (0.428), so the replacement reads with the
  same weight instead of looking lighter.

Output is raw RGBA with the rows flipped, because the upload path treats row 0 as
the bottom row (measured: without the flip the words render inverted).
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 853, 480
BAND = HEIGHT // 2


def ink_extent(text: str, font: ImageFont.FreeTypeFont) -> np.ndarray:
    """Coverage image of `text`, cropped to its ink."""
    probe = font.getbbox(text)
    canvas = Image.new("L", (probe[2] - probe[0] + 40, probe[3] - probe[1] + 40), 0)
    ImageDraw.Draw(canvas).text((20 - probe[0], 20 - probe[1]), text, font=font, fill=255)
    array = np.array(canvas)
    ys, xs = np.nonzero(array)
    if len(xs) == 0:
        raise SystemExit("error: font produced no ink")
    return array[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def font_at_size(path: Path, size: int) -> ImageFont.FreeTypeFont:
    try:
        return ImageFont.truetype(str(path), size)
    except OSError as error:
        raise SystemExit(f"error: cannot open font {path}: {error}")


def scaled_word(path: Path, word: str, width: int, height: int) -> np.ndarray:
    """`word` rendered and rescaled so its ink box is exactly width x height."""
    size = max(8, height)
    for _ in range(24):                     # grow/shrink until the crop is tall enough
        crop = ink_extent(word, font_at_size(path, size))
        if crop.shape[0] >= height:
            break
        size = int(size * 1.15) + 1
    return np.array(Image.fromarray(crop).resize((width, height), Image.LANCZOS))


def build(path: Path, top: str, bottom: str, scale: float) -> np.ndarray:
    coverage = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
    bleed = 1.02 * scale
    fill = 0.92 * scale
    for index, word in enumerate((top, bottom)):
        line = scaled_word(path, word, int(WIDTH * bleed), int(BAND * fill))
        top_row = index * BAND + (BAND - line.shape[0]) // 2
        left = (WIDTH - line.shape[1]) // 2
        src_x0 = max(0, -left)
        dst_x0 = max(0, left)
        width = min(line.shape[1] - src_x0, WIDTH - dst_x0)
        if width <= 0:
            continue
        rows = np.clip(np.arange(top_row, top_row + line.shape[0]), 0, HEIGHT - 1)
        block = coverage[rows, dst_x0:dst_x0 + width]
        coverage[rows, dst_x0:dst_x0 + width] = np.maximum(block, line[:, src_x0:src_x0 + width])
    return coverage


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--top", default="HYPR")
    parser.add_argument("--bottom", default="LODR")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--target-ink", type=float,
                        help="fit the glyph scale until this fraction of pixels is inked "
                             "(the shipped mask measures 0.428)")
    args = parser.parse_args(argv)

    scale = 1.0
    coverage = build(args.font, args.top, args.bottom, scale)
    if args.target_ink:
        for _ in range(9):
            ink = float((coverage > 127).mean())
            if ink <= 0.0 or abs(ink - args.target_ink) < 0.004:
                break
            scale = min(1.45, max(0.80, scale * math.sqrt(args.target_ink / ink)))
            coverage = build(args.font, args.top, args.bottom, scale)

    rgba = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8)
    rgba[:, :, 3] = 255 - coverage           # letters are the transparent part
    rgba = np.flipud(rgba)                   # upload path treats row 0 as the bottom row
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(rgba.tobytes())
    if args.preview:
        Image.fromarray(np.dstack([np.flipud(rgba[:, :, 3])] * 3)).save(args.preview)
    print("%s: %dx%d %d B scale=%.3f ink=%.3f (words %s / %s)"
          % (args.out, WIDTH, HEIGHT, rgba.nbytes, scale, float((coverage > 127).mean()),
             args.top, args.bottom))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
