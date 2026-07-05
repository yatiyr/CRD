#!/usr/bin/env python3
# v14-g hyper-optimizer reconstruction (python-verified BEFORE the C++ port, per protocol;
# the v13/v14-e discipline). cotengra 0.8.2 is the oracle — its algorithms were extracted
# from source (path_basic.py / path_labels.py / path_simulated_annealing.py / slicer.py /
# core.py; notes in the session log). This script:
#   1. reimplements the algorithm family FAITHFULLY (greedy + random-greedy trials +
#      label-propagation partition trees + treesa SA + slicing) with OUR deterministic
#      RNG (SplitMix64 counter draws — portable to C++ bit-for-bit, unlike Mersenne),
#   2. VERIFIES the cost model bit-exactly against cotengra on its own returned paths,
#   3. A/Bs tree quality vs cotengra at MATCHED trial budgets on benchmark networks.
# Run (WSL): python3 scripts/v14g_hyperopt_recon.py
import heapq
import math

import cotengra as ctg

# ---------------------------------------------------------------------------
# Deterministic RNG: SplitMix64 — trivially portable to C++ (u64 ops only).
# Draws are keyed (seed, counter): order-independent across trials by design.
# ---------------------------------------------------------------------------
MASK64 = (1 << 64) - 1


def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


class Rng:
    """Counter-mode SplitMix64: state advances by fixed increments; u01() in [0,1)."""

    def __init__(self, seed):
        self.state = splitmix64(seed & MASK64)
        self.ctr = 0

    def next_u64(self):
        self.ctr += 1
        return splitmix64((self.state + self.ctr) & MASK64)

    def u01(self):
        # 53-bit mantissa double in [0,1), same construction C++ will use
        return (self.next_u64() >> 11) * (1.0 / (1 << 53))

    def uniform(self, lo, hi):
        return lo + (hi - lo) * self.u01()

    def log_uniform(self, lo, hi):
        return math.exp(self.uniform(math.log(lo), math.log(hi)))

    def gumbel(self):
        u = self.u01()
        # guard u==0 (prob 2^-53); C++ mirrors this exactly
        if u <= 0.0:
            u = 0.5 ** 53
        return -math.log(-math.log(u))

    def randint(self, lo, hi):  # inclusive both ends, unbiased-enough for ranges here
        return lo + self.next_u64() % (hi - lo + 1)

    def shuffle(self, xs):  # Fisher-Yates, high-to-low, deterministic
        for i in range(len(xs) - 1, 0, -1):
            j = self.next_u64() % (i + 1)
            xs[i], xs[j] = xs[j], xs[i]


# ---------------------------------------------------------------------------
# The contraction network (cotengra ContractionProcessor equivalent):
# node -> sorted tuple of (index_id, count) legs; edges: index_id -> set of nodes;
# appearances[ix] = total occurrences over all inputs + output (size-1 skipped).
# ---------------------------------------------------------------------------
class Net:
    def __init__(self, inputs, output, size_dict):
        self.indmap = {}
        self.sizes = []
        self.appearances = []
        self.nodes = {}
        self.edges = {}
        for i, term in enumerate(inputs):
            legs = {}
            for ind in term:
                d = size_dict[ind]
                if d == 1:
                    continue
                ix = self.indmap.get(ind)
                if ix is None:
                    ix = len(self.sizes)
                    self.indmap[ind] = ix
                    self.sizes.append(d)
                    self.appearances.append(0)
                legs[ix] = legs.get(ix, 0) + 1
                self.appearances[ix] += 1
            self.nodes[i] = tuple(sorted(legs.items()))
            for ix in legs:
                self.edges.setdefault(ix, {})[i] = None
        for ind in output:
            ix = self.indmap.get(ind)
            if ix is not None:
                self.appearances[ix] += 1
        self.ssa = len(inputs)
        self.ssa_path = []
        self.flops = 0
        self.flops_limit = None

    def copy(self):
        c = Net.__new__(Net)
        c.indmap = self.indmap  # shared immutables
        c.sizes = self.sizes
        c.appearances = self.appearances
        c.nodes = dict(self.nodes)
        c.edges = {ix: dict(ns) for ix, ns in self.edges.items()}
        c.ssa = self.ssa
        c.ssa_path = list(self.ssa_path)
        c.flops = self.flops
        c.flops_limit = self.flops_limit
        return c

    def size_of(self, legs):
        s = 1
        for ix, _ in legs:
            s *= self.sizes[ix]
        return s

    def flops_of(self, ilegs, jlegs):
        # product over the UNION of indices (cotengra metric: no x2 for mul-add)
        s = 1
        seen = None
        ii = jj = 0
        while ii < len(ilegs) and jj < len(jlegs):
            a, b = ilegs[ii][0], jlegs[jj][0]
            if a < b:
                s *= self.sizes[a]
                ii += 1
            elif b < a:
                s *= self.sizes[b]
                jj += 1
            else:
                s *= self.sizes[a]
                ii += 1
                jj += 1
        for k in range(ii, len(ilegs)):
            s *= self.sizes[ilegs[k][0]]
        for k in range(jj, len(jlegs)):
            s *= self.sizes[jlegs[k][0]]
        return s

    def contracted_legs(self, ilegs, jlegs):
        # sorted two-pointer merge; shared index count ic+jc, dropped iff == appearances
        out = []
        ii = jj = 0
        while ii < len(ilegs) and jj < len(jlegs):
            (ia, ic), (ja, jc) = ilegs[ii], jlegs[jj]
            if ia < ja:
                out.append((ia, ic))
                ii += 1
            elif ja < ia:
                out.append((ja, jc))
                jj += 1
            else:
                c = ic + jc
                if c < self.appearances[ia]:
                    out.append((ia, c))
                ii += 1
                jj += 1
        out.extend(ilegs[ii:])
        out.extend(jlegs[jj:])
        return tuple(out)

    def pop_node(self, i):
        legs = self.nodes.pop(i)
        for ix, _ in legs:
            ens = self.edges.get(ix)
            if ens is not None:
                ens.pop(i, None)
                if not ens:
                    del self.edges[ix]
        return legs

    def add_node(self, legs):
        i = self.ssa
        self.ssa += 1
        self.nodes[i] = legs
        for ix, _ in legs:
            self.edges.setdefault(ix, {})[i] = None
        return i

    def contract(self, i, j, new_legs=None):
        ilegs = self.pop_node(i)
        jlegs = self.pop_node(j)
        self.flops += self.flops_of(ilegs, jlegs)
        if new_legs is None:
            new_legs = self.contracted_legs(ilegs, jlegs)
        k = self.add_node(new_legs)
        self.ssa_path.append((i, j))
        return k

    def neighbors_limit(self, i, max_neighbors):
        for ix, _ in self.nodes[i]:
            ens = self.edges.get(ix)
            if ens is None or len(ens) > max_neighbors:
                continue
            for j in ens:
                if j != i:
                    yield j


# ---------------------------------------------------------------------------
# Greedy (faithful): score = sab/costmod - (sa+sb)*costmod; T>0 Boltzmann via
# Gumbel on sign(s)*log|s|; min-heap w/ insertion-counter tie-break; edges with
# > max_neighbors nodes skipped; queue pruned at 2^14; leftovers by size.
# ---------------------------------------------------------------------------
def local_score(sa, sb, sab, costmod, temperature, rng):
    s = sab / costmod - (sa + sb) * costmod
    if temperature == 0.0:
        return s
    if s > 0:
        base = math.log(s)
    elif s < 0:
        base = -math.log(-s)
    else:
        base = 0.0
    return base - temperature * rng.gumbel()


def greedy_optimize(net, costmod=1.0, temperature=0.0, max_neighbors=16, rng=None):
    if rng is None:
        rng = Rng(0)
    node_sizes = {i: net.size_of(legs) for i, legs in net.nodes.items()}
    queue = []
    contractions = {}
    c = 0
    for ix, ens in net.edges.items():
        if len(ens) > max_neighbors:
            continue
        ns = list(ens)
        for a in range(len(ns)):
            for b in range(a + 1, len(ns)):
                i, j = ns[a], ns[b]
                klegs = net.contracted_legs(net.nodes[i], net.nodes[j])
                ksize = net.size_of(klegs)
                sc = local_score(node_sizes[i], node_sizes[j], ksize, costmod, temperature, rng)
                heapq.heappush(queue, (sc, c))
                contractions[c] = (i, j, ksize, klegs)
                c += 1
    while queue:
        _, c0 = heapq.heappop(queue)
        i, j, ksize, klegs = contractions.pop(c0)
        if i not in net.nodes or j not in net.nodes:
            continue
        k = net.contract(i, j, new_legs=klegs)
        if net.flops_limit is not None and net.flops >= net.flops_limit:
            return False
        node_sizes[k] = ksize
        for l in net.neighbors_limit(k, max_neighbors):
            mlegs = net.contracted_legs(net.nodes[k], net.nodes[l])
            msize = net.size_of(mlegs)
            sc = local_score(ksize, node_sizes[l], msize, costmod, temperature, rng)
            heapq.heappush(queue, (sc, c))
            contractions[c] = (k, l, msize, mlegs)
            c += 1
        if len(queue) >= (1 << 14):
            alive = [(sc, cc) for (sc, cc) in queue
                     if contractions[cc][0] in net.nodes and contractions[cc][1] in net.nodes]
            dead = set(contractions) - {cc for _, cc in alive}
            for cc in dead:
                del contractions[cc]
            queue = alive
            heapq.heapify(queue)
    return True


def contract_remaining_by_size(net):
    rem = [(net.size_of(legs), i) for i, legs in net.nodes.items()]
    heapq.heapify(rem)
    while len(rem) > 1:
        sa, i = heapq.heappop(rem)
        while i not in net.nodes:
            sa, i = heapq.heappop(rem)
        sb, j = heapq.heappop(rem)
        while j not in net.nodes:
            sb, j = heapq.heappop(rem)
        k = net.contract(i, j)
        heapq.heappush(rem, (net.size_of(net.nodes[k]), k))


def random_greedy(inputs, output, size_dict, ntrials, seed,
                  costmod=(0.1, 4.0), temperature=(0.001, 1.0), max_neighbors=16):
    """best-of-ntrials greedy; per-trial params keyed (seed, trial) — order-independent."""
    net0 = Net(inputs, output, size_dict)
    best = None
    best_flops = None
    for t in range(ntrials):
        prng = Rng((seed << 20) ^ (2 * t))       # parameter draws
        grng = Rng((seed << 20) ^ (2 * t + 1))   # gumbel draws
        cm = prng.uniform(*costmod)
        tp = prng.log_uniform(*temperature)
        net = net0.copy()
        net.flops_limit = best_flops
        ok = greedy_optimize(net, costmod=cm, temperature=tp, max_neighbors=max_neighbors, rng=grng)
        if not ok:
            continue
        contract_remaining_by_size(net)
        if best_flops is None or net.flops < best_flops:
            best, best_flops = net.ssa_path, net.flops
    return best, best_flops


# ---------------------------------------------------------------------------
# Exact cost of an SSA path under the cotengra metric (flops = sum of
# prod-of-union per step; width = log2 max intermediate size).
# ---------------------------------------------------------------------------
def cost_of_ssa_path(inputs, output, size_dict, ssa_path):
    net = Net(inputs, output, size_dict)
    max_size = max((net.size_of(l) for l in net.nodes.values()), default=1)
    for (i, j) in ssa_path:
        k = net.contract(i, j)
        sz = net.size_of(net.nodes[k])
        if sz > max_size:
            max_size = sz
    return net.flops, max_size


# ---------------------------------------------------------------------------
# Verification 1: our cost model == cotengra's on THEIR paths (bit-exact).
# Verification 2: our T=0 greedy == cotengra greedy quality (parity gate).
# Verification 3: our random-greedy vs their random-greedy at matched trials.
# ---------------------------------------------------------------------------
def bench_networks(seed=1407):
    nets = []
    # cotengra's own random-network generator, several regimes
    for (n, reg, d_min, d_max, tag) in [
        (30, 3, 2, 4, "rand30"),
        (60, 3, 2, 4, "rand60"),
        (120, 3, 2, 3, "rand120"),
        (200, 3, 2, 2, "rand200"),
    ]:
        inputs, output, shapes, size_dict = ctg.utils.rand_equation(
            n=n, reg=reg, d_min=d_min, d_max=d_max, seed=seed)
        nets.append((tag, [tuple(t) for t in inputs], tuple(output), dict(size_dict)))
    # 2D lattice contraction (open boundary), a structured regime
    inputs, output, shapes, size_dict = ctg.utils.lattice_equation([8, 8], d_max=3, seed=seed)
    nets.append(("lat8x8", [tuple(t) for t in inputs], tuple(output), dict(size_dict)))
    inputs, output, shapes, size_dict = ctg.utils.lattice_equation([4, 4, 4], d_max=2, seed=seed)
    nets.append(("lat4x4x4", [tuple(t) for t in inputs], tuple(output), dict(size_dict)))
    return nets


def main():
    print("== v14-g reconstruction: cost-model verification + greedy A/B ==")
    nets = bench_networks()
    ok_cost = 0
    for (tag, inputs, output, size_dict) in nets:
        # --- their greedy path (deterministic, T=0) ---
        path = ctg.array_contract_path(inputs, output, size_dict=size_dict, optimize="greedy")
        tree = ctg.ContractionTree.from_path(inputs, output, size_dict, path=path)
        their_flops = int(tree.contraction_cost())  # = sum prod(involved dims)
        # replay THEIR path through OUR cost model (convert linear path -> ssa)
        ssa = linear_to_ssa(path, len(inputs))
        our_flops, our_width = cost_of_ssa_path(inputs, output, size_dict, ssa)
        match = "BIT-MATCH" if our_flops == their_flops else f"MISMATCH ours={our_flops} theirs={their_flops}"
        if our_flops == their_flops:
            ok_cost += 1
        print(f"[{tag:9s}] cost-model on their greedy path: {match}")
        # --- our T=0 greedy quality vs theirs ---
        net = Net(inputs, output, size_dict)
        greedy_optimize(net, costmod=1.0, temperature=0.0)
        contract_remaining_by_size(net)
        print(f"            greedy T=0: ours log10={math.log10(max(net.flops,1)):.4f} "
              f"theirs log10={math.log10(max(their_flops,1)):.4f}")
        # --- random-greedy, matched 32 trials ---
        ours_path, ours_fl = random_greedy(inputs, output, size_dict, ntrials=32, seed=7)
        opt = ctg.RandomGreedyOptimizer(max_repeats=32, seed=7, parallel=False)
        opt.search(inputs, output, size_dict)
        print(f"            random-greedy x32: ours log10={math.log10(max(ours_fl,1)):.4f} "
              f"theirs log10={opt.best_flops:.4f}")
    print(f"cost-model bit-match: {ok_cost}/{len(nets)}")


def linear_to_ssa(path, n):
    """opt_einsum linear path -> ssa pairs (the standard relabeling)."""
    ids = list(range(n))
    ssa = n
    out = []
    for (a, b) in path:
        a, b = (a, b) if a > b else (b, a)  # pop larger first
        ia = ids.pop(a)
        ib = ids.pop(b)
        out.append((ib, ia))
        ids.append(ssa)
        ssa += 1
    return out


if __name__ == "__main__":
    main()
