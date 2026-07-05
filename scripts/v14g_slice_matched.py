#!/usr/bin/env python3
# v14-g matched-tree slicing A/B: ONE tree (ours), both slicers on it.
# Pure slicer-vs-slicer — the honest protocol (fair peer, same input).
# Run (WSL): python3 -u scripts/v14g_slice_matched.py
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from v14g_hyperopt_recon import bench_networks
from v14g_hyperopt_recon2 import hyper_search, slice_finder

import cotengra as ctg


def main():
    for (tag, inputs, output, size_dict) in bench_networks():
        if tag not in ("rand120", "rand200", "lat4x4x4"):
            continue
        best = hyper_search(inputs, output, size_dict, ntrials=16, seed=3)
        sc, tree, flops, write, size, method, t = best
        base_w = math.log2(max(size, 1))
        target = 1 << max(int(base_w) - 6, 4)
        # ours on our tree
        inds, costs = slice_finder(tree, target, seed=5, max_repeats=16)
        # theirs on the SAME tree (export ssa path)
        ssa = tree.to_ssa_path()
        ct = ctg.ContractionTree.from_path(inputs, output, size_dict,
                                           ssa_path=[(a, b) for (a, b) in ssa])
        f0 = int(ct.contract_stats()["flops"])
        ct.slice_(target_size=target, seed=5, max_repeats=16)
        st = ct.contract_stats()
        print(f"[{tag:9s}] SAME tree, target=2^{int(math.log2(target))}:", flush=True)
        if inds is None:
            print("            OURS: FAILED to satisfy the bound", flush=True)
        else:
            print(f"            OURS nslices={costs.nslices:6d} ok={costs.max_size() <= target} "
                  f"overhead={costs.overhead():.3f}", flush=True)
        print(f"            CTG  nslices={int(ct.multiplicity):6d} ok={int(st['size']) <= target} "
              f"overhead={int(st['flops']) / max(f0, 1):.3f}", flush=True)


if __name__ == "__main__":
    main()
