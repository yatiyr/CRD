import re, timeit
import opt_einsum as oe

inc = open("/mnt/d/Dev/cerid/tests/hesap-tensor/ref_einsum_paths.inc").read()
cases = re.findall(r'\{"([^"]+)", "([^"]+)", kEinSizes(\d+), (\d+)U, (\d+)ULL, (\d+)ULL\},', inc)
sizes_arrays = {int(m.group(1)): [int(x) for x in m.group(2).replace("ULL", "").split(",")]
                for m in re.finditer(r'kEinSizes(\d+)\[\] = \{([^}]+)\};', inc)}
work = []
for expr, names, sid, nidx, gf, of in cases:
    sizes = dict(zip(names, sizes_arrays[int(sid)]))
    shapes = [tuple(sizes[c] for c in t) for t in expr.split("->")[0].split(",")]
    work.append((expr, shapes))

def plan_all(mode):
    for expr, shapes in work:
        oe.contract_path(expr, *shapes, shapes=True, optimize=mode)

for mode in ("greedy", "optimal"):
    t = timeit.timeit(lambda: plan_all(mode), number=5) / 5
    print(f"opt_einsum {mode:8s}: {t*1e6/len(work):9.2f} us/plan (33-case corpus mean)")
