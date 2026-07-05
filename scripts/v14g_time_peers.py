#!/usr/bin/env python3
# v14-g peer wall-clock: cotengra hyper (greedy+kahypar, 64 trials, the hq
# default) and cotengrust (Rust) random-greedy x32 on the SAME frozen corpus
# networks. Pin with taskset from the caller. Times in ms, quality log10.
import math
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from v14g_hyperopt_recon import bench_networks

import cotengra as ctg
import cotengrust as ctr
import cotengra.pathfinders.path_basic as pb

_orig_init = pb.RandomGreedyOptimizer.__init__


def _serial_init(self, *a, **k):
    k["parallel"] = False
    _orig_init(self, *a, **k)


pb.RandomGreedyOptimizer.__init__ = _serial_init


def main():
    for (tag, inputs, output, size_dict) in bench_networks():
        # cotengra hyper hq default @64
        t0 = time.perf_counter()
        opt = ctg.HyperOptimizer(methods=["greedy", "kahypar"], max_repeats=64,
                                 parallel=False, optlib="random",
                                 optlib_opts={"seed": 3}, minimize="flops",
                                 reconf_opts={})
        ct = opt.search(inputs, output, size_dict)
        t1 = time.perf_counter()
        st = ct.contract_stats()
        print(f"[{tag:9s}] ctg hyper64      : {1000*(t1-t0):8.2f} ms  log10={math.log10(max(int(st['flops']),1)):8.4f}", flush=True)
        # cotengrust random-greedy x32 (Rust, their fastest engine)
        li = [list(t) for t in inputs]
        t0 = time.perf_counter()
        path, l10 = ctr.optimize_random_greedy_track_flops(li, list(output), size_dict, ntrials=32, seed=7)
        t1 = time.perf_counter()
        print(f"[{tag:9s}] cotengrust rg x32: {1000*(t1-t0):8.2f} ms  log10={l10:8.4f}", flush=True)


if __name__ == "__main__":
    main()
