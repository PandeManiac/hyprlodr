#!/usr/bin/env python3
"""Render the title glyph mask that the game's own title pass consumes.

Measured in the running game: the title fragment shader samples one 853x480
texture whose RGB is zero everywhere and whose alpha holds the letterforms, then
composites `(1.0 - alpha) * background` inside an 854x480 window. This tool
produces that same texture for different words, from one of the game's own fonts,
so the replacement is drawn by the original shader over the original animated
background and through the original post chain.

Output is raw RGBA (row 0 first, matching glTexSubImage2D) at exactly the size
the game's texture has, so it can be uploaded without changing any geometry.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 853, 480


def render_word(font_path: Path, word: str, target_w: int, target_h: int) -> np.ndarray:
    """Ink coverage (0..255) of `word`, scaled to fill target_w x target_h."""
    best = None
    for size in range(40, 620, 4):
        try:
            font = ImageFont.truetype(str(font_path), size)
        except OSError as error:
            raise SystemExit(f"error: cannot open font {font_path}: {error}")
        canvas = Image.new("L", (target_w * 3, target_h * 3), 0)
        ImageDraw.Draw(canvas).text((20, 20), word, font=font, fill=255)
        array = np.array(canvas)
        ys, xs = np.nonzero(array)
        if len(xs) == 0:
            continue
        crop = array[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        best = crop
        if crop.shape[0] >= target_h:
            break
    if best is None:
        raise SystemExit("error: font produced no ink")
    scaled = np.array(Image.fromarray(best).resize((target_w, target_h), Image.LANCZOS))
    return scaled


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--top", default="HYPR")
    parser.add_argument("--bottom", default="LODR")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--bleed", type=float, default=1.12,
                        help="scale glyphs wider than the canvas so they run off the edges")
    args = parser.parse_args(argv)

    coverage = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
    band_h = HEIGHT // 2
    for index, word in enumerate((args.top, args.bottom)):
        line = render_word(args.font, word, int(WIDTH * args.bleed), int(band_h * 0.94))
        top = index * band_h + (band_h - line.shape[0]) // 2
        left = (WIDTH - line.shape[1]) // 2
        src_x0 = max(0, -left)
        dst_x0 = max(0, left)
        width = min(line.shape[1] - src_x0, WIDTH - dst_x0)
        coverage[top:top + line.shape[0], dst_x0:dst_x0 + width] = \
            np.maximum(coverage[top:top + line.shape[0], dst_x0:dst_x0 + width],
                       line[:, src_x0:src_x0 + width])

    rgba = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8)
    rgba[:, :, 3] = 255 - coverage          # letters are the transparent part
    # The game uploads with glTexSubImage2D, whose first row is the *bottom* row,
    # so the buffer is written upside down to make the words read upright on
    # screen (measured: without this the title renders inverted).
    rgba = np.flipud(rgba)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(rgba.tobytes())
    if args.preview:
        Image.fromarray(np.dstack([rgba[:, :, 3]] * 3)).save(args.preview)
    ink = (coverage > 127).mean()
    print("%s: %dx%d, %d bytes, ink=%.3f (words: %s / %s)"
          % (args.out, WIDTH, HEIGHT, rgba.nbytes, ink, args.top, args.bottom))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
