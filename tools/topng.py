"""Convert capture PGMs (16-bit, 12-bit data) to contrast-stretched PNGs."""

import sys

import numpy as np
from PIL import Image


def to_png(pgm_path, scale=6):
    img = np.array(Image.open(pgm_path)).astype(np.float64)
    lo, hi = np.percentile(img, 1), np.percentile(img, 99)
    img = np.clip((img - lo) / max(hi - lo, 1), 0, 1) * 255
    out = Image.fromarray(img.astype(np.uint8))
    out = out.resize((out.width * scale, out.height * scale), Image.NEAREST)
    png = str(pgm_path).rsplit(".", 1)[0] + ".png"
    out.save(png)
    return png


if __name__ == "__main__":
    for path in sys.argv[1:]:
        print(to_png(path))
