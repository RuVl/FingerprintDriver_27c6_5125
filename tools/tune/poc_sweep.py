"""Sweep BLPOC band and peak scoring (raw peak vs peak-to-sidelobe ratio)."""
import numpy as np
from poc_eval import band_mask, load, prepare, rotations, spectrum

def run(frame_index, lo, hi, psr, enroll_n=10, folds=6):
    genuine, impostor = load("genuine"), load("impostor")
    mask = band_mask(lo, hi)
    gen = [prepare(t[0], t[frame_index]) for t in genuine]
    gen_f = [spectrum(g) for g in gen]
    imp_f = [spectrum(prepare(t[0], t[frame_index])) for t in impostor]
    gen_rot = [[spectrum(r) for r in rotations(g)] for g in gen]
    def s(pf, rf):
        c = pf * np.conj(rf); c /= np.abs(c) + 1e-9
        r = np.real(np.fft.ifft2(c * mask))
        if not psr:
            return r.max()
        return (r.max() - r.mean()) / (r.std() + 1e-9)
    def best(pf, enrolled):
        return max(s(pf, rf) for e in enrolled for rf in gen_rot[e])
    rng = np.random.default_rng(1)
    gs, is_ = [], []
    for _ in range(folds):
        idx = rng.permutation(len(gen))
        en, pr = idx[:enroll_n], idx[enroll_n:]
        gs += [best(gen_f[p], en) for p in pr]
        is_ += [best(q, en) for q in imp_f]
    gs, is_ = np.array(gs), np.array(is_)
    return np.mean(gs <= is_.max()), np.median(gs), is_.max()

for fi in (2, 3):
    for lo, hi in ((0.05, 0.5), (0.03, 0.5), (0.05, 0.42)):
        for psr in (False, True):
            frr, med, imax = run(fi, lo, hi, psr)
            print(f"frame {fi} band {lo}-{hi} {'PSR ' if psr else 'peak'}: FRR@FAR0 {frr:.0%} (gen median {med:.3f}, imp max {imax:.3f})", flush=True)
