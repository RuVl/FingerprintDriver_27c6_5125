"""Evaluate matchers on the full dataset.

Protocols (enrollment -> probes):
  varied->natural : enroll 15 of the varied touches, probe the 30 natural ones
  natural->natural: enroll 15 natural touches, probe the other 15 natural
Impostors: all impostor touches, against the same enrollment.
Reported: FRR at the threshold giving FAR = 0 and FAR <= 1%.
"""

import glob

import numpy as np
import cv2

from sigfm_eval import DATASET, H, W, extract, process, score as sigfm_score

ANGLES = np.arange(-20, 21, 4)
PAD = 2


def load(label):
    files = sorted(glob.glob(str(DATASET / f"{label}-*.npz")))
    return np.concatenate([np.load(f)["frames"] for f in files]).astype(
        np.int32)


# ---- BLPOC ----

def prepare(bg, frame):
    diff = np.clip(bg - frame, 0, None).reshape(H, W).astype(np.float32)
    img = diff - cv2.GaussianBlur(diff, (0, 0), 4)
    win = np.outer(np.hanning(H), np.hanning(W)).astype(np.float32)
    return (img - img.mean()) / (img.std() + 1e-6) * win


def spectrum(img):
    padded = np.zeros((H * PAD, W * PAD), np.float32)
    padded[:H, :W] = img
    return np.fft.fft2(padded)


def band_mask(lo, hi):
    fy = np.fft.fftfreq(H * PAD)[:, None]
    fx = np.fft.fftfreq(W * PAD)[None, :]
    r = np.hypot(fy, fx)
    return ((r >= lo) & (r <= hi)).astype(np.float32)


class Blpoc:
    def __init__(self, lo=0.05, hi=0.42, frame=2):
        self.mask = band_mask(lo, hi)
        self.frame = frame

    def features(self, touch):
        return prepare(touch[0], touch[self.frame])

    def enroll(self, feats):
        rots = []
        for img in feats:
            for a in ANGLES:
                m = cv2.getRotationMatrix2D((W / 2, H / 2), float(a), 1.0)
                rots.append(spectrum(cv2.warpAffine(img, m, (W, H))))
        return np.stack(rots)

    def score(self, feat, enrolled):
        c = spectrum(feat)[None] * np.conj(enrolled)
        c /= np.abs(c) + 1e-9
        r = np.real(np.fft.ifft2(c * self.mask, axes=(1, 2)))
        return float(r.max()) / (self.mask.sum() / self.mask.size)


class Sigfm:
    def __init__(self, frame=1):
        self.frame = frame

    def features(self, touch):
        return extract(process(touch[0], touch[self.frame]))

    def enroll(self, feats):
        return feats

    def score(self, feat, enrolled):
        return max(sigfm_score(feat, e) for e in enrolled)


def rates(gen, imp):
    thr0 = imp.max()
    thr1 = np.quantile(imp, 0.99)
    return np.mean(gen <= thr0), np.mean(gen <= thr1)


def run(matcher, varied, natural, impostor, folds=5):
    fv = [matcher.features(t) for t in varied]
    fn = [matcher.features(t) for t in natural]
    fi = [matcher.features(t) for t in impostor]
    rng = np.random.default_rng(7)
    out = {}
    for proto in ("varied->natural", "natural->natural"):
        gen, imp = [], []
        for _ in range(folds):
            if proto == "varied->natural":
                idx = rng.permutation(len(fv))[:15]
                enrolled = matcher.enroll([fv[i] for i in idx])
                probes = fn
            else:
                idx = rng.permutation(len(fn))
                enrolled = matcher.enroll([fn[i] for i in idx[:15]])
                probes = [fn[i] for i in idx[15:]]
            gen += [matcher.score(p, enrolled) for p in probes]
            imp += [matcher.score(q, enrolled) for q in fi]
        out[proto] = rates(np.array(gen), np.array(imp))
    return out


def main():
    varied, natural = load("genuine"), load("natural")
    impostor = load("impostor")
    print(f"varied {len(varied)}, natural {len(natural)}, "
          f"impostor {len(impostor)}")
    matchers = {
        "sigfm (current driver)": Sigfm(frame=1),
        "BLPOC 0.05-0.42 full": Blpoc(0.05, 0.42, 2),
        "BLPOC 0.05-0.42 late": Blpoc(0.05, 0.42, 3),
        "BLPOC 0.05-0.5 full": Blpoc(0.05, 0.5, 2),
    }
    for name, m in matchers.items():
        res = run(m, varied, natural, impostor)
        line = " | ".join(f"{p}: FRR {a:4.0%} @FAR0, {b:4.0%} @FAR1%"
                          for p, (a, b) in res.items())
        print(f"{name:24} {line}", flush=True)


if __name__ == "__main__":
    main()
