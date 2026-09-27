"""Band-limited phase-only correlation (BLPOC) matching experiment.

Ito et al., "A fingerprint matching algorithm using phase-only correlation"
(IEICE 2004). Score = height of the POC peak (after limiting the spectrum to
the ridge frequency band), maximised over small rotations.
"""

import numpy as np
import cv2

from sigfm_eval import H, W, load

ANGLES = range(-20, 21, 4)
PAD = 2  # zero padding factor, so partial overlaps are not wrapped


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


def rotations(img):
    out = []
    for a in ANGLES:
        m = cv2.getRotationMatrix2D((W / 2, H / 2), a, 1.0)
        out.append(cv2.warpAffine(img, m, (W, H)))
    return out


def poc(fa, fb, mask):
    cross = fa * np.conj(fb)
    cross /= np.abs(cross) + 1e-9
    r = np.real(np.fft.ifft2(cross * mask))
    return float(r.max()) / mask.sum() * mask.size ** 0  # normalised peak


def evaluate(frame_index, lo, hi, enroll_n=10, folds=6):
    genuine, impostor = load("genuine"), load("impostor")
    mask = band_mask(lo, hi)
    norm = mask.sum() / mask.size
    gen = [prepare(t[0], t[frame_index]) for t in genuine]
    imp = [spectrum(prepare(t[0], t[frame_index])) for t in impostor]
    gen_f = [spectrum(g) for g in gen]
    gen_rot = [[spectrum(r) for r in rotations(g)] for g in gen]

    def best(probe_f, templates):
        return max(
            float(np.real(np.fft.ifft2(
                (lambda c: c / (np.abs(c) + 1e-9))(probe_f * np.conj(rf))
                * mask)).max()) / norm
            for e in templates for rf in gen_rot[e])

    rng = np.random.default_rng(1)
    gs, is_ = [], []
    for _ in range(folds):
        idx = rng.permutation(len(gen))
        enrolled, probes = idx[:enroll_n], idx[enroll_n:]
        gs += [best(gen_f[p], enrolled) for p in probes]
        is_ += [best(q, enrolled) for q in imp]
    return np.array(gs), np.array(is_)


def main():
    for frame_index, name in ((1, "first"), (2, "full")):
        for lo, hi in ((0.06, 0.25), (0.08, 0.2), (0.05, 0.35)):
            gs, is_ = evaluate(frame_index, lo, hi)
            thr = is_.max()
            print(f"{name:5} band {lo:.2f}-{hi:.2f}: genuine median "
                  f"{np.median(gs):.3f} p25 {np.percentile(gs, 25):.3f} | "
                  f"impostor median {np.median(is_):.3f} max {thr:.3f} | "
                  f"FRR at FAR=0 {np.mean(gs <= thr):.0%}", flush=True)


if __name__ == "__main__":
    main()
