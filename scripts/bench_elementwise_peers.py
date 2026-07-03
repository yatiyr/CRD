import numpy as np, timeit
def t(name, elems, stmt, reps=10):
    dt = timeit.timeit(stmt, number=reps) / (reps * elems) * 1e9
    print(f"{name:28s} {dt:8.4f} ns/elem")

a1 = (0.001 * (np.arange(1 << 20) % 999)).astype(np.float32); b1 = (1.0 - a1).astype(np.float32)
r1 = np.empty_like(a1)
t("numpy contig add f32 1M", a1.size, lambda: np.add(a1, b1, out=r1))

a2 = (0.5 + 0.0001 * np.arange(4096)).astype(np.float32).reshape(4096, 1)
b2 = (1.5 - 0.0001 * np.arange(4096)).astype(np.float32).reshape(1, 4096)
r2 = np.empty((4096, 4096), np.float32)
t("numpy outer bcast mul 16M", r2.size, lambda: np.multiply(a2, b2, out=r2))

a3 = (0.001 * (np.arange(2048 * 2048) % 4999)).reshape(2048, 2048)
b3 = np.arange(2048).astype(np.float64)
r3 = np.empty_like(a3)
t("numpy row bcast add f64 4M", a3.size, lambda: np.add(a3, b3, out=r3))

a4 = (0.001 * (np.arange(4096 * 2048) % 777)).astype(np.float32).reshape(4096, 2048)
av = a4[::2]
r4 = np.empty((2048, 2048), np.float32)
t("numpy strided mul f32 4M", r4.size, lambda: np.multiply(av, np.float32(1.0009), out=r4))

try:
    import torch
    torch.set_num_threads(1)
    ta1, tb1 = torch.from_numpy(a1), torch.from_numpy(b1); tr1 = torch.empty_like(ta1)
    t("torch contig add f32 1M", a1.size, lambda: torch.add(ta1, tb1, out=tr1))
    ta2, tb2 = torch.from_numpy(a2), torch.from_numpy(b2); tr2 = torch.empty(4096, 4096)
    t("torch outer bcast mul 16M", r2.size, lambda: torch.mul(ta2, tb2, out=tr2))
    ta3, tb3 = torch.from_numpy(a3), torch.from_numpy(b3); tr3 = torch.empty_like(ta3)
    t("torch row bcast add f64 4M", a3.size, lambda: torch.add(ta3, tb3, out=tr3))
except ImportError:
    pass
