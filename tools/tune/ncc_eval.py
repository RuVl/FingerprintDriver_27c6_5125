"""Correlation-based matching experiment for the 64x80 5125 frames.

Score of a probe against a template: the best normalized cross-correlation
of the probe's central patch over translations and small rotations of the
template. A probe's score against an enrollment is the max over templates.
"""

import numpy as np
import cv2

from sigfm_eval import H, W, load

ANGLES = range(-20, 21, 5)


def prepare(bg, frame, frame_is_bg_minus=True):
    diff = np.clip(bg - frame, 0, None).reshape(H, W).astype(np.float32)
    # remove low-frequency pressure/illumination, keep ridges
    ridges = diff - cv2.GaussianBlur(diff, (0, 0), 4)
    ridges = cv2.GaussianBlur(ridges, (0, 0), 0.8)
    return ridges / (ridges.std() + 1e-6)


def rotations(img):
    out = []
    for a in ANGLES:
        m = cv2.getRotationMatrix2D((W / 2, H / 2), a, 1.0)
        out.append(cv2.warpAffine(img, m, (W, H), borderMode=cv2.BORDER_REFLECT))
    return out


def match(probe, template_rots, patch=(40, 48)):
    ph, pw = patch[1], patch[0]
    y0, x0 = (H - ph) // 2, (W - pw) // 2
    tpl = probe[y0:y0 + ph, x0:x0 + pw]
    best = -1.0
    for rot in template_rots:
        res = cv2.matchTemplate(rot, tpl, cv2.TM_CCOEFF_NORMED)
        best = max(best, float(res.max()))
    return best


def main():
    genuine, impostor = load("genuine"), load("impostor")
    for frame_index, name in ((1, "first"), (2, "full"), (3, "late")):
        gen = [prepare(t[0], t[frame_index]) for t in genuine]
        imp = [prepare(t[0], t[frame_index]) for t in impostor]
        gen_rots = [rotations(g) for g in gen]
        rng = np.random.default_rng(1)
        gs, is_ = [], []
        for _ in range(6):
            idx = rng.permutation(len(gen))
            enrolled, probes = idx[:10], idx[10:]
            for p in probes:
                gs.append(max(match(gen[p], gen_rots[e]) for e in enrolled))
            for q in imp:
                is_.append(max(match(q, gen_rots[e]) for e in enrolled))
        gs, is_ = np.array(gs), np.array(is_)
        thr = is_.max()
        print(f"{name:6} genuine median {np.median(gs):.3f} p10 "
              f"{np.percentile(gs, 10):.3f} | impostor median "
              f"{np.median(is_):.3f} max {thr:.3f} | FRR at FAR=0: "
              f"{np.mean(gs <= thr):.0%}")


if __name__ == "__main__":
    main()
