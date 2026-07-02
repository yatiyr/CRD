import numpy as np, ml_dtypes, timeit
n = 1 << 20
x = (0.001 * (np.arange(n) % 9973) - 3.3).astype(np.float32)
h = x.astype(np.float16)


def t(name, stmt):
    reps = 20
    dt = timeit.timeit(stmt, number=reps) / (reps * n) * 1e9
    print(f"{name:22s} {dt:8.3f} ns/elem")


t("numpy f32->f16", lambda: x.astype(np.float16))
t("numpy f16->f32", lambda: h.astype(np.float32))
t("ml_dtypes f32->bf16", lambda: x.astype(ml_dtypes.bfloat16))
t("ml_dtypes f32->e4m3", lambda: x.astype(ml_dtypes.float8_e4m3fn))
try:
    import torch
    tx = torch.from_numpy(x)
    t("torch f32->bf16", lambda: tx.to(torch.bfloat16))
    t("torch f32->f16", lambda: tx.to(torch.float16))
except ImportError:
    pass
