"""Keypoints + RANSAC rigid-transform verification.

Features are extracted once per touch; a pair score is the number of RANSAC
inliers of a rotation+translation(+uniform scale close to 1) model.
"""

import sys

import cv2
import numpy as np

from eval_all import load, rates
from sigfm_eval import process

RATIO = 0.9


def detector(name):
    if name == "sift":
        return cv2.SIFT_create(contrastThreshold=0.01)
    if name == "akaze":
        return cv2.AKAZE_create(threshold=0.0005)
    if name == "orb":
        return cv2.ORB_create(nfeatures=500, edgeThreshold=8, patchSize=15)
    raise ValueError(name)


def features(touch, det, scale, frame=2):
    img = process(touch[0], touch[frame], scale=scale)
    kp, desc = det.detectAndCompute(img, None)
    pts = np.array([k.pt for k in kp], np.float32).reshape(-1, 2)
    return pts, desc


def pair_score(a, b, norm):
    (pa, da), (pb, db) = a, b
    if da is None or db is None or len(da) < 3 or len(db) < 3:
        return 0
    knn = cv2.BFMatcher(norm).knnMatch(da, db, k=2)
    src, dst = [], []
    for pair in knn:
        if len(pair) == 2 and pair[0].distance < RATIO * pair[1].distance:
            src.append(pa[pair[0].queryIdx])
            dst.append(pb[pair[0].trainIdx])
    if len(src) < 4:
        return 0
    model, inliers = cv2.estimateAffinePartial2D(
        np.array(src), np.array(dst), method=cv2.RANSAC,
        ransacReprojThreshold=4.0, maxIters=2000, confidence=0.99)
    if model is None:
        return 0
    s = float(np.hypot(model[0, 0], model[1, 0]))
    if not 0.85 < s < 1.18:
        return 0
    return int(inliers.sum())


def evaluate(name, scale, folds=5):
    det = detector(name)
    norm = cv2.NORM_HAMMING if name in ("orb", "akaze") else cv2.NORM_L2
    varied, natural, impostor = load("genuine"), load("natural"), load("impostor")
    fv = [features(t, det, scale) for t in varied]
    fn = [features(t, det, scale) for t in natural]
    fi = [features(t, det, scale) for t in impostor]
    rng = np.random.default_rng(7)
    res = {}
    for proto in ("varied->natural", "natural->natural"):
        gen, imp = [], []
        for _ in range(folds):
            if proto == "varied->natural":
                enrolled = [fv[i] for i in rng.permutation(len(fv))[:15]]
                probes = fn
            else:
                idx = rng.permutation(len(fn))
                enrolled = [fn[i] for i in idx[:15]]
                probes = [fn[i] for i in idx[15:]]
            gen += [max(pair_score(p, e, norm) for e in enrolled) for p in probes]
            imp += [max(pair_score(q, e, norm) for e in enrolled) for q in fi]
        gen, imp = np.array(gen), np.array(imp)
        res[proto] = (*rates(gen, imp), np.median(gen), imp.max())
    kp = np.mean([len(f[0]) for f in fn])
    line = " | ".join(f"{p}: FRR {a:4.0%}@FAR0 {b:4.0%}@FAR1% "
                      f"(gen med {m:.0f}, imp max {x})"
                      for p, (a, b, m, x) in res.items())
    print(f"{name:5} x{scale} kp~{kp:4.0f} {line}", flush=True)


if __name__ == "__main__":
    for spec in sys.argv[1:] or ["sift:2", "sift:3", "akaze:3", "orb:3"]:
        name, scale = spec.split(":")
        evaluate(name, int(scale))
