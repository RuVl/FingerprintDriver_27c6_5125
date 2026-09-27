"""Export RAW 16-bit 64x80 frames for the vendor AlgoMilan pipeline.

The vendor preprocessor wants raw uint16 sensor data (ROW=80 x COL=64 = 5120),
NOT our 8-bit background-subtracted image. Per record the .npz holds 4 frames;
index 0 = background (no finger), index 2 = full contact.

Outputs (little-endian uint16, row-major, W=64 fastest):
  algo/frames_raw.bin  — contact frame (idx 2) per record, concatenated
  algo/bg_raw.bin      — background frame (idx 0) per record, concatenated
  algo/meta_raw.txt    — one line "label index" per record (same order)
"""
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tune"))
import numpy as np  # noqa: E402
from sigfm_eval import load, W, H  # noqa: E402

N = W * H  # 5120
here = pathlib.Path(__file__).resolve().parent
fr = open(here / "frames_raw.bin", "wb")
bg = open(here / "bg_raw.bin", "wb")
meta = open(here / "meta_raw.txt", "w")
n = 0
for label in ("genuine", "natural", "impostor"):
    data = load(label)  # (records, 4, 5120) int32
    for i in range(len(data)):
        contact = data[i, 2].astype(np.uint16)
        background = data[i, 0].astype(np.uint16)
        assert contact.shape == (N,) and background.shape == (N,)
        fr.write(contact.tobytes())
        bg.write(background.tobytes())
        meta.write(f"{label} {i}\n")
        n += 1
fr.close(); bg.close(); meta.close()
print(f"wrote {n} raw records ({W}x{H} uint16) to algo/frames_raw.bin + bg_raw.bin")
