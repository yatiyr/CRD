#!/usr/bin/env python3
# v14-g slicing gate: the memory bound must be honored EXACTLY (WCET pillar).
# Our SliceFinder vs cotengra's on the same trees + targets; both must land
# max-intermediate <= target; compare slice count + flops overhead.
# Run (WSL): python3 -u scripts/v14g_slice_check.py
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from v14g_hyperopt_recon import Net, bench_networks
from v14g_hyperopt_recon2 import Tree, hyper_search, slice_finder, SliceCosts

import cotengra as ctg


def main():
    for (tag, inputs, output, size_dict) in bench_networks():
        if tag not in ("rand120", "lat4x4x4", "rand200"):
            continue
        best = hyper_search(inputs, output, size_dict, ntrials=16, seed=3)
        sc, tree, flops, write, size, method, t = best
        base_w = math.log2(max(size, 1))
        # target: 6 powers of two below the unsliced width (guaranteed nontrivial)
        target = 1 << max(int(base_w) - 6, 4)
        inds, costs = slice_finder(tree, target, seed=5, max_repeats=16)
        if inds is None:
            print(f"[{tag:9s}] OURS: no slicing found for target 2^{int(math.log2(target))} — GATE FAIL", flush=True)
            continue
        ok = costs.max_size() <= target
        ovh = costs.overhead()
        print(f"[{tag:9s}] base log2sz={base_w:5.2f} target=2^{int(math.log2(target))} | "
              f"OURS nslices={costs.nslices} maxsz<=target: {ok} overhead={ovh:.3f}", flush=True)
        # cotengra on ITS OWN best tree at the same target (their strongest form)
        opt = ctg.HyperOptimizer(methods=["greedy"], max_repeats=16, parallel=False,
                                 optlib="random", optlib_opts={"seed": 3},
                                 minimize="flops", reconf_opts={})
        ct = opt.search(inputs, output, size_dict)
        st0 = ct.contract_stats()
        f0 = int(st0["flops"])
        ct.slice_(target_size=target, seed=5, max_repeats=16)
        st = ct.contract_stats()
        their_ok = int(st["size"]) <= target
        their_ovh = int(st["flops"]) / max(f0, 1)
        print(f"            CTG  nslices={int(ct.multiplicity)} maxsz<=target: {their_ok} "
              f"overhead={their_ovh:.3f}", flush=True)


if __name__ == "__main__":
    main()
