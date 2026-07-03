import numpy as np, timeit

cases = [
    ("ab,bc,cd->ad", {"a": 512, "b": 512, "c": 512, "d": 512}, 1),
    ("ea,fb,abcd,gc,hd->efgh", {c: 24 for c in "abcdefgh"}, 1),
    ("ij,jk->ik", {"i": 32, "j": 32, "k": 32}, 200),
    ("abc,bad->dc", {c: 96 for c in "abcd"}, 5),
]

def make(expr, sizes):
    return [np.random.default_rng(7).random(tuple(sizes[c] for c in t)) for t in expr.split("->")[0].split(",")]

for expr, sizes, reps in cases:
    ops = make(expr, sizes)
    f = lambda: np.einsum(expr, *ops, optimize=True)
    f()
    t = min(timeit.repeat(f, number=reps, repeat=10)) / reps * 1e6
    print(f"numpy  {expr:24s} {t:10.2f} us")

try:
    import torch
    torch.set_num_threads(1)
    for expr, sizes, reps in cases:
        ops = [torch.from_numpy(o) for o in make(expr, sizes)]
        f = lambda: torch.einsum(expr, *ops)
        f()
        t = min(timeit.repeat(f, number=reps, repeat=10)) / reps * 1e6
        print(f"torch  {expr:24s} {t:10.2f} us")
except ImportError:
    pass
