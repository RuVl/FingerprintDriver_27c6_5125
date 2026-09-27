"""BLPOC with wide rotation search, natural->natural and varied->natural."""
import numpy as np
import eval_all
from eval_all import Blpoc, load, run

varied, natural, impostor = load("genuine"), load("natural"), load("impostor")
for angles in (np.arange(-20, 21, 4), np.arange(0, 360, 8), np.arange(0, 360, 5)):
    eval_all.ANGLES = angles
    res = run(Blpoc(0.05, 0.42, 2), varied, natural, impostor, folds=3)
    line = " | ".join(f"{p}: FRR {a:4.0%} @FAR0, {b:4.0%} @FAR1%" for p, (a, b) in res.items())
    print(f"angles {angles[0]}..{angles[-1]} step {angles[1]-angles[0]} ({len(angles)}): {line}", flush=True)
