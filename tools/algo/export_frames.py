"""Export processed 8-bit 64x80 frames from the dataset for the C harness.

Layout of frames.bin: for each touch, one uint8[80*64] image = process(bg,frame)
(background-subtracted, contrast-stretched, ridges dark), matching the driver.
meta.txt: one line per record "label index".
"""
import struct, sys
sys.path.insert(0, "tune")
import numpy as np
from eval_all import load
from sigfm_eval import process, W, H

out = open("algo/frames.bin", "wb")
meta = open("algo/meta.txt", "w")
n = 0
for label in ("genuine", "natural", "impostor"):
    data = load(label)
    for i in range(len(data)):
        img = process(data[i, 0], data[i, 2])  # frame index 2 = full contact
        assert img.shape == (H, W) and img.dtype == np.uint8
        out.write(img.tobytes())
        meta.write(f"{label} {i}\n")
        n += 1
out.close(); meta.close()
print(f"wrote {n} frames of {W}x{H} to algo/frames.bin")
