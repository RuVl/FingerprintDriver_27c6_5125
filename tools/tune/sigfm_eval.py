"""Offline evaluation of image processing variants for sigfm matching.

Replicates libfprint/sigfm/sigfm.cpp (SIFT + ratio test + pairwise length and
angle consistency) and scores a dataset collected with tools/collect.py.
A probe matches if any enrolled template scores >= threshold (as in
fpi_print_sigfm_match).
"""

import glob
import itertools
import math
import pathlib
import random

import cv2
import numpy as np

DATASET = pathlib.Path(__file__).resolve().parents[2] / "dumps" / "dataset"
W, H = 64, 80
DISTANCE_MATCH, LENGTH_MATCH, ANGLE_MATCH, MIN_MATCH = 0.75, 0.05, 0.05, 5
SIFT = cv2.SIFT_create()


def load(label):
    path = sorted(glob.glob(str(DATASET / f"{label}-*.npz")))[-1]
    return np.load(path)["frames"].astype(np.int32)


def process(bg, frame, scale=1, clahe=False, smooth=False):
    """Mirror of process_frame() in goodix5125.c, plus optional extras."""
    diff = np.clip(bg - frame, 0, None).reshape(H, W).astype(np.float64)
    lo, hi = np.percentile(diff, 1), np.percentile(diff, 99)
    img = np.clip((diff - lo) * 255 / max(hi - lo, 1), 0, 255)
    img = (255 - img).astype(np.uint8)
    if smooth:
        img = cv2.GaussianBlur(img, (3, 3), 0)
    if scale != 1:
        img = cv2.resize(img, (W * scale, H * scale),
                         interpolation=cv2.INTER_CUBIC)
    if clahe:
        img = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(4, 4)).apply(img)
    return img


def extract(img):
    kp, desc = SIFT.detectAndCompute(img, None)
    return [k.pt for k in kp], desc


def score(probe, enrolled):
    (pts1, d1), (pts2, d2) = probe, enrolled
    if d1 is None or d2 is None or len(d1) < 2 or len(d2) < 2:
        return 0
    knn = cv2.BFMatcher().knnMatch(d1, d2, k=2)
    matches = set()
    count = 0
    for pair in knn:
        if len(pair) < 2:
            continue
        m1, m2 = pair
        if m1.distance < DISTANCE_MATCH * m2.distance:
            p1 = tuple(int(v) for v in pts1[m1.queryIdx])
            p2 = tuple(int(v) for v in pts2[m1.trainIdx])
            matches.add((p1, p2))
            count += 1
    if count < MIN_MATCH:
        return 0
    matches = sorted(matches)
    angles = []
    for (a1, a2), (b1, b2) in itertools.combinations(matches, 2):
        v1 = (a1[0] - b1[0], a1[1] - b1[1])
        v2 = (a2[0] - b2[0], a2[1] - b2[1])
        l1, l2 = math.hypot(*v1), math.hypot(*v2)
        if l1 == 0 or l2 == 0:
            continue
        if 1 - min(l1, l2) / max(l1, l2) <= LENGTH_MATCH:
            prod = l1 * l2
            s = math.pi / 2 + math.asin(
                max(-1, min(1, (v1[0] * v2[0] + v1[1] * v2[1]) / prod)))
            c = math.acos(
                max(-1, min(1, (v1[0] * v2[1] - v1[1] * v2[0]) / prod)))
            angles.append((s, c))
    if len(angles) < MIN_MATCH:
        return 0
    n = 0
    for (s1, c1), (s2, c2) in itertools.combinations(angles, 2):
        if (max(s1, s2) > 0 and 1 - min(s1, s2) / max(s1, s2) <= ANGLE_MATCH
                and max(c1, c2) > 0
                and 1 - min(c1, c2) / max(c1, c2) <= ANGLE_MATCH):
            n += 1
    return n


def evaluate(genuine, impostor, frame_index, enroll_n, folds=6, **opts):
    """Returns per-probe best scores for genuine and impostor probes."""
    gen = [extract(process(t[0], t[frame_index], **opts)) for t in genuine]
    imp = [extract(process(t[0], t[frame_index], **opts)) for t in impostor]
    rng = random.Random(1)
    gen_scores, imp_scores = [], []
    for _ in range(folds):
        idx = list(range(len(gen)))
        rng.shuffle(idx)
        enrolled = [gen[i] for i in idx[:enroll_n]]
        probes = [gen[i] for i in idx[enroll_n:]]
        gen_scores += [max(score(p, e) for e in enrolled) for p in probes]
        imp_scores += [max(score(p, e) for e in enrolled) for p in imp]
    return np.array(gen_scores), np.array(imp_scores)


def summary(name, gen, imp, threshold=24):
    safe = imp.max() + 1
    return (f"{name:38} FRR@{threshold}={np.mean(gen < threshold):5.0%} "
            f"FAR@{threshold}={np.mean(imp >= threshold):5.0%} | "
            f"imp max {imp.max():4d} -> FRR@{safe}={np.mean(gen < safe):5.0%} "
            f"| gen median {int(np.median(gen)):5d}")


def main():
    genuine, impostor = load("genuine"), load("impostor")
    variants = [
        ("first frame (current driver)", 1, {}),
        ("full-contact frame", 2, {}),
        ("late frame (+150ms)", 3, {}),
        ("late, x2 upscale", 3, {"scale": 2}),
        ("late, x2 upscale + CLAHE", 3, {"scale": 2, "clahe": True}),
        ("late, x2 upscale + smooth", 3, {"scale": 2, "smooth": True}),
        ("late, x3 upscale", 3, {"scale": 3}),
    ]
    for enroll_n in (10, 15):
        print(f"--- enrolled templates: {enroll_n}")
        for name, frame_index, opts in variants:
            gen, imp = evaluate(genuine, impostor, frame_index, enroll_n,
                                **opts)
            print(summary(name, gen, imp), flush=True)


if __name__ == "__main__":
    main()
