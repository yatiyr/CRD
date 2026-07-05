#!/usr/bin/env python3
# v14-g reconstruction pass 2: contraction TREE machinery + subtree-reconfigure
# (exact-DP re-solve) + treesa SA + labels-partition divisive trees + SliceFinder
# + OUR deterministic hyper driver. Faithful to cotengra 0.8.2 (formulas extracted
# from source; pass-1 verified the cost model bit-exactly + greedy parity).
# Run (WSL): python3 scripts/v14g_hyperopt_recon2.py
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from v14g_hyperopt_recon import (Rng, Net, greedy_optimize, contract_remaining_by_size,
                                 random_greedy, cost_of_ssa_path)

# ---------------------------------------------------------------------------
# ContractionTree over a Net's leaves: nodes are ints (leaves 0..n-1, fresh ids
# after). children[p] = (l, r). Per-node legs (tuple of (ix, count)), size,
# flops — cotengra's exact accounting (flops = prod over union of child legs).
# ---------------------------------------------------------------------------
class Tree:
    def __init__(self, net0):
        self.net = net0  # frozen reference Net (leaves only)
        self.children = {}
        self.legs = {i: net0.nodes[i] for i in net0.nodes}
        self.parent = {}
        self.nxt = max(net0.nodes) + 1 if net0.nodes else 0
        self.root = None

    @staticmethod
    def from_ssa(net0, ssa_path):
        t = Tree(net0)
        # replay: ssa ids in the path refer to net-contraction order; mirror it
        idmap = {i: i for i in net0.nodes}
        ssa = net0.ssa
        for (a, b) in ssa_path:
            la, lb = t.legs[idmap[a]], t.legs[idmap[b]]
            merged = net0.contracted_legs(la, lb)
            p = t.nxt
            t.nxt += 1
            t.children[p] = (idmap[a], idmap[b])
            t.parent[idmap[a]] = p
            t.parent[idmap[b]] = p
            t.legs[p] = merged
            idmap[ssa] = p
            ssa += 1
        t.root = idmap[ssa - 1] if ssa_path else next(iter(net0.nodes))
        return t

    def node_flops(self, p):
        l, r = self.children[p]
        return self.net.flops_of(self.legs[l], self.legs[r])

    def node_size(self, p):
        return self.net.size_of(self.legs[p])

    def stats(self):
        flops = 0
        write = 0
        size = 1
        for p in self.children:
            flops += self.node_flops(p)
            s = self.node_size(p)
            write += s
            if s > size:
                size = s
        return flops, write, size

    def leaves_under(self, p):
        out = []
        st = [p]
        while st:
            x = st.pop()
            c = self.children.get(x)
            if c is None:
                out.append(x)
            else:
                st.extend(c)
        return out

    def extent(self, p):
        c = self.children.get(p)
        if c is None:
            return 1
        n = 0
        st = [p]
        while st:
            x = st.pop()
            cc = self.children.get(x)
            if cc is None:
                n += 1
            else:
                st.extend(cc)
        return n

    def to_ssa_path(self):
        """post-order emission of (a, b) ssa pairs in net-replayable form."""
        order = []
        st = [(self.root, False)]
        while st:
            x, done = st.pop()
            c = self.children.get(x)
            if c is None:
                continue
            if done:
                order.append(x)
            else:
                st.append((x, True))
                st.append((c[0], False))
                st.append((c[1], False))
        # relabel to ssa ids: leaves keep ids, internals get n, n+1, ...
        idmap = {i: i for i in self.legs if i not in self.children or True}
        idmap = {leaf: leaf for leaf in self.leaves_under(self.root)}
        ssa = self.net.ssa
        out = []
        for p in order:
            l, r = self.children[p]
            out.append((idmap[l], idmap[r]))
            idmap[p] = ssa
            ssa += 1
        return out


# ---------------------------------------------------------------------------
# score_local (cotengra scoring.py, exact): objective in {flops, combo-F, size}
# ---------------------------------------------------------------------------
def score_local(objective, flops_list, size_list, factor=64):
    if objective == "flops":
        return math.log2(max(sum(flops_list), 1))
    if objective == "size":
        return math.log2(max(max(size_list), 1))
    # combo
    return math.log2(max(sum(flops_list) + factor * sum(size_list), 1))


def score_tree(objective, flops, write, size, factor=64):
    if objective == "flops":
        return math.log2(max(flops, 1)) + 1e-3 * math.log2(max(write, 1)) + 1e-3 * math.log2(max(size, 1))
    if objective == "size":
        return math.log2(max(size, 1)) + 1e-3 * math.log2(max(flops, 1)) + 1e-3 * math.log2(max(write, 1))
    return math.log2(max(flops + factor * write, 1))


# ---------------------------------------------------------------------------
# Exact DP over <= ~10 pseudo-inputs (subset DP on the union graph): returns the
# ssa pair order minimizing flops (or combo). Used for subtree reconfigure and
# the partition glue. Faithful in role to cotengra's OptimalOptimizer call.
# ---------------------------------------------------------------------------
def optimal_dp(net, node_ids, objective="flops", factor=64):
    n = len(node_ids)
    if n == 1:
        return [], 0
    legs0 = [net.nodes[i] for i in node_ids]
    full = (1 << n) - 1
    best = {}
    for k in range(n):
        best[1 << k] = (0, legs0[k], None)  # (cost, legs, plan)
    # iterate subsets by popcount
    subsets_by_pop = [[] for _ in range(n + 1)]
    for m in range(1, full + 1):
        subsets_by_pop[bin(m).count("1")].append(m)
    for pc in range(2, n + 1):
        for m in subsets_by_pop[pc]:
            bm = None
            # split into (s, m^s): iterate proper submasks, canonical s < m^s
            s = (m - 1) & m
            while s:
                o = m ^ s
                if s < o:
                    s = (s - 1) & m
                    continue
                if s in best and o in best:
                    cs, ls, _ = best[s]
                    co, lo, _ = best[o]
                    fl = net.flops_of(ls, lo)
                    if objective == "combo":
                        ml = net.contracted_legs(ls, lo)
                        step = fl + factor * net.size_of(ml)
                    else:
                        ml = None
                        step = fl
                    tot = cs + co + step
                    if bm is None or tot < bm[0]:
                        if ml is None:
                            ml = net.contracted_legs(ls, lo)
                        bm = (tot, ml, (s, o))
                s = (s - 1) & m
            if bm is not None:
                best[m] = bm
    # unwind plan to pair order over node_ids indices
    order = []

    def unwind(m):
        c, l, plan = best[m]
        if plan is None:
            return
        s, o = plan
        unwind(s)
        unwind(o)
        order.append((s, o))

    unwind(full)
    return order, best[full][0]


# ---------------------------------------------------------------------------
# subtree_reconfigure (deterministic: select=max by node flops, bfs subtrees of
# <= subtree_size leaves, exact-DP re-solve; maxiter bounded)
# ---------------------------------------------------------------------------
def subtree_reconfigure(tree, subtree_size=8, maxiter=1024, objective="flops", factor=64):
    net = tree.net
    improved_any = True
    seen = set()
    iters = 0
    while improved_any and iters < maxiter:
        improved_any = False
        # candidates: internal nodes sorted by descending flops
        cands = sorted(tree.children, key=lambda p: (-tree.node_flops(p), p))
        for sub_root in cands:
            if iters >= maxiter:
                break
            if sub_root not in tree.children:
                continue
            # bfs frontier from sub_root until subtree_size leaves-of-subtree
            frontier = [sub_root]
            while True:
                grow = None
                for i, x in enumerate(frontier):
                    if x in tree.children and len(frontier) < subtree_size:
                        grow = i
                        break
                if grow is None:
                    break
                x = frontier.pop(grow)
                l, r = tree.children[x]
                frontier.extend((l, r))
            if len(frontier) < 3:
                continue
            key = frozenset(frontier)
            if key in seen:
                continue
            seen.add(key)
            iters += 1
            # current cost of the subtree's internal contractions
            internal = []
            st = [sub_root]
            while st:
                x = st.pop()
                if x in tree.children and x not in frontier:
                    internal.append(x)
                    l, r = tree.children[x]
                    for c in (l, r):
                        if c not in frontier:
                            st.append(c)
            cur = 0
            for p in internal:  # includes sub_root (collected by the walk)
                if p in tree.children:
                    fl = tree.node_flops(p)
                    cur += fl + (factor * tree.node_size(p) if objective == "combo" else 0)
            # DP re-solve over frontier pseudo-inputs
            mini = MiniNet(net, [tree.legs[x] for x in frontier])
            order, cost = optimal_dp(mini, list(range(len(frontier))), objective, factor)
            if cost >= cur:
                continue
            # splice: rebuild the subtree per DP order
            improved_any = True
            rebuild_subtree(tree, sub_root, frontier, order)
    return tree


class MiniNet:
    """adapter exposing flops_of/contracted_legs/size_of over pseudo-input legs."""

    def __init__(self, net, legs_list):
        self.base = net
        self.nodes = {i: legs for i, legs in enumerate(legs_list)}
        # appearances for merge: within this closed mini-problem, an index
        # appearing in k pseudo-inputs + (survives above?) — approximate with
        # the BASE appearances so boundary indices are kept (faithful: legs use
        # global appearances)
        self.appearances = net.appearances
        self.sizes = net.sizes

    def flops_of(self, a, b):
        return self.base.flops_of(a, b)

    def contracted_legs(self, a, b):
        return self.base.contracted_legs(a, b)

    def size_of(self, legs):
        return self.base.size_of(legs)


def rebuild_subtree(tree, sub_root, frontier, order):
    """replace sub_root's internal structure with the DP pair order over frontier."""
    # drop old internals (except sub_root label, reused for the final pair)
    st = [sub_root]
    to_del = []
    while st:
        x = st.pop()
        if x in tree.children:
            l, r = tree.children[x]
            for c in (l, r):
                if c not in frontier:
                    st.append(c)
            if x != sub_root:
                to_del.append(x)
    for x in to_del:
        del tree.children[x]
        tree.legs.pop(x, None)
        tree.parent.pop(x, None)
    idmap = {(1 << i): f for i, f in enumerate(frontier)}
    net = tree.net
    for k, (s, o) in enumerate(order):
        a, b = idmap[s], idmap[o]
        m = s | o
        last = k == len(order) - 1
        p = sub_root if last else tree.nxt
        if not last:
            tree.nxt += 1
        tree.children[p] = (a, b)
        tree.parent[a] = p
        tree.parent[b] = p
        tree.legs[p] = net.contracted_legs(tree.legs[a], tree.legs[b])
        idmap[m] = p


# ---------------------------------------------------------------------------
# treesa: SA over the tree — 4 associativity rotations, local energy, Metropolis
# log-acceptance, geometric temperature ladder. (path_simulated_annealing.py)
# ---------------------------------------------------------------------------
def simulated_anneal(tree, tstart=2.0, tfinal=0.05, tsteps=50, numiter=50,
                     objective="flops", factor=64, seed=0):
    rng = Rng(seed)
    net = tree.net
    if tsteps == 1:
        temps = [math.sqrt(tstart * tfinal)]
    else:
        l0, l1 = math.log2(tstart), math.log2(tfinal)
        temps = [2.0 ** (l0 + i * (l1 - l0) / (tsteps - 1)) for i in range(tsteps)]
    for temp in temps:
        for _ in range(numiter):
            queue = [tree.root]
            while queue:
                p = queue.pop(0)
                c = tree.children.get(p)
                if c is None:
                    continue
                l, r = c
                lleaf = l not in tree.children
                rleaf = r not in tree.children
                if lleaf and rleaf:
                    continue
                if lleaf:
                    rule = 2 + int(rng.next_u64() & 1)
                elif rleaf:
                    rule = int(rng.next_u64() & 1)
                else:
                    rule = int(rng.next_u64() & 3)
                if rule in (0, 1):
                    x = l
                    cnode = r
                    a, b = tree.children[x]
                    new_order = (a, cnode, b) if rule == 0 else (b, cnode, a)
                else:
                    a = l
                    x = r
                    b, cnode = tree.children[x]
                    new_order = (a, cnode, b) if rule == 2 else (a, b, cnode)
                # current local energy: the two affected contractions p and x
                cur = score_local(objective, [tree.node_flops(p), tree.node_flops(x)],
                                  [tree.node_size(p), tree.node_size(x)], factor)
                n0, n1, n2 = new_order
                inner_legs = net.contracted_legs(tree.legs[n0], tree.legs[n1])
                f0 = net.flops_of(tree.legs[n0], tree.legs[n1])
                s0 = net.size_of(inner_legs)
                f1 = net.flops_of(inner_legs, tree.legs[n2])
                outer_legs = net.contracted_legs(inner_legs, tree.legs[n2])
                s1 = net.size_of(outer_legs)
                prop = score_local(objective, [f0, f1], [s0, s1], factor)
                dE = prop - cur
                if dE <= 0 or math.log(max(rng.u01(), 1e-300)) < -dE / temp:
                    # apply: x becomes the inner pair (n0, n1); p = (x, n2)
                    tree.children[x] = (n0, n1)
                    tree.parent[n0] = x
                    tree.parent[n1] = x
                    tree.legs[x] = inner_legs
                    tree.children[p] = (x, n2)
                    tree.parent[x] = p
                    tree.parent[n2] = p
                    # p's legs unchanged (same overall contraction)
                l2, r2 = tree.children[p]
                for ch in (l2, r2):
                    if ch in tree.children and tree.extent(ch) > 2:
                        queue.append(ch)
    return tree


# ---------------------------------------------------------------------------
# labels partition + divisive tree builder (path_labels.py + core.build_divide)
# ---------------------------------------------------------------------------
def labels_partition(net, node_ids, parts, rng, memory=0, pop_small=1.0, pop_big=1.0,
                     pop_decay=1.0, con_pow=1.0, final_sweep=True, maxiter=None):
    ids = list(node_ids)
    n = len(ids)
    pos = {x: i for i, x in enumerate(ids)}
    # pair weights: max-edge-normalized log2 edge weight; boosted by shared count
    nbrs = [dict() for _ in range(n)]
    maxw = 1.0
    edge_w = {}
    idset = set(ids)
    for x in ids:
        for ix, _ in net.nodes[x]:
            w = math.log2(net.sizes[ix]) + 1.0
            edge_w[ix] = w
            if w > maxw:
                maxw = w
    for x in ids:
        i = pos[x]
        for ix, _ in net.nodes[x]:
            for y in net.edges.get(ix, ()):  # neighbors through ix
                if y == x or y not in idset:
                    continue
                nbrs[i][pos[y]] = edge_w[ix] / maxw
    for i in range(n):
        for j, w in list(nbrs[i].items()):
            shared = 1 + sum(1 for k in nbrs[i] if k in nbrs[j])
            nbrs[i][j] = w * (shared ** con_pow)
    labels = list(range(n))
    pops = {i: 1 for i in range(n)}
    m = n / max(parts, 1)
    if maxiter is None:
        maxiter = n
    sites = list(range(n))
    for r in range(maxiter):
        rng.shuffle(sites)
        static = True
        for i in sites:
            scores = {}
            old = labels[i]
            scores[old] = float(memory)
            for j, w in nbrs[i].items():
                scores[labels[j]] = scores.get(labels[j], 0.0) + w
            decay = (r + 1) ** pop_decay
            for lbl in list(scores):
                p = pops.get(lbl, 0)
                if p <= m:
                    bias = pop_small * n * math.sin(math.pi * p / m) if m > 0 else 0.0
                else:
                    bias = -pop_big * n * math.sin(math.pi / 2 * (p - m) / max(n - m, 1e-12))
                scores[lbl] += bias / decay
            new = max(scores.items(), key=lambda kv: (kv[1], kv[0] == old))[0]
            if new != old:
                static = False
                pops[old] -= 1
                pops[new] = pops.get(new, 0) + 1
                labels[i] = new
        if static:
            break
    if final_sweep:
        rng.shuffle(sites)
        for i in sites:
            scores = {labels[i]: 0.0}
            for j, w in nbrs[i].items():
                scores[labels[j]] = scores.get(labels[j], 0.0) + w
            labels[i] = max(scores.items(), key=lambda kv: kv[1])[0]
    # group by label, sorted by label id (cotengra: sorted block labels)
    groups = {}
    for i, lbl in enumerate(labels):
        groups.setdefault(lbl, []).append(ids[i])
    return [groups[k] for k in sorted(groups)]


def build_divide(net0, seed, parts=4, parts_decay=0.5, cutoff=16, memory=0,
                 pop_small=1.0, pop_big=1.0, pop_decay=1.0, con_pow=1.0,
                 final_sweep=True, glue_trials=16, objective="flops", factor=64):
    """divisive partition tree: recursively partition; glue partitions with
    random-greedy over pseudo-nodes; fill small subgraphs with greedy."""
    rng = Rng(seed)
    net = net0.copy()
    N = len(net.nodes)

    def solve(ids):
        """returns the ssa-relative contraction: contracts net's ids into one node."""
        if len(ids) == 1:
            return ids[0]
        if len(ids) == 2:
            return net.contract(ids[0], ids[1])
        if len(ids) <= cutoff:
            # greedy fill (T=0) restricted to these ids: build a subnet view
            return greedy_fill(ids)
        s = len(ids) / N
        parts_s = max(int((s ** parts_decay) * parts), 2)
        groups = labels_partition(net, ids, parts_s, rng, memory, pop_small, pop_big,
                                  pop_decay, con_pow, final_sweep)
        if len(groups) <= 1:
            return greedy_fill(ids)
        reps = [solve(g) for g in groups]
        # glue: best-of random-greedy over the partition pseudo-nodes
        return glue(reps)

    def greedy_fill(ids):
        # T=0 greedy among ids only: local heap over shared-edge pairs
        alive = set(ids)
        import heapq as hq
        q = []
        c = 0
        tbl = {}
        pairs = set()
        for x in ids:
            for y in net.neighbors_limit(x, 16):
                if y in alive and y != x:
                    key = (min(x, y), max(x, y))
                    if key not in pairs:
                        pairs.add(key)
        for (i, j) in sorted(pairs):
            kl = net.contracted_legs(net.nodes[i], net.nodes[j])
            sc = net.size_of(kl) - (net.size_of(net.nodes[i]) + net.size_of(net.nodes[j]))
            hq.heappush(q, (sc, c))
            tbl[c] = (i, j)
            c += 1
        while q and len(alive) > 1:
            _, c0 = hq.heappop(q)
            i, j = tbl.pop(c0)
            if i not in net.nodes or j not in net.nodes or i not in alive or j not in alive:
                continue
            k = net.contract(i, j)
            alive.discard(i)
            alive.discard(j)
            alive.add(k)
            for l in net.neighbors_limit(k, 16):
                if l in alive and l != k:
                    kl = net.contracted_legs(net.nodes[k], net.nodes[l])
                    sc = net.size_of(kl) - (net.size_of(net.nodes[k]) + net.size_of(net.nodes[l]))
                    hq.heappush(q, (sc, c))
                    tbl[c] = (k, l)
                    c += 1
        # leftovers (disconnected): by size
        while len(alive) > 1:
            xs = sorted(alive, key=lambda x: (net.size_of(net.nodes[x]), x))
            k = net.contract(xs[0], xs[1])
            alive.discard(xs[0])
            alive.discard(xs[1])
            alive.add(k)
        return next(iter(alive))

    def glue(reps):
        if len(reps) == 1:
            return reps[0]
        if len(reps) == 2:
            return net.contract(reps[0], reps[1])
        # best-of random-greedy over the pseudo-node mini-net
        best_order = None
        best_cost = None
        for t in range(glue_trials):
            trng = Rng((seed << 24) ^ (t * 2 + 1))
            mini = MiniNet(net, [net.nodes[x] for x in reps])
            order, cost = mini_random_greedy(mini, len(reps), trng)
            if best_cost is None or cost < best_cost:
                best_order, best_cost = order, cost
        cur = list(reps)
        for (a, b) in best_order:
            k = net.contract(cur[a], cur[b])
            cur.append(k)
        return cur[-1]

    def mini_random_greedy(mini, n, trng):
        cm = trng.uniform(0.1, 4.0)
        tp = trng.log_uniform(0.001, 1.0)
        legs = {i: mini.nodes[i] for i in range(n)}
        alive = set(range(n))
        order = []
        cost = 0
        nxt = n
        while len(alive) > 1:
            bs = None
            for i in sorted(alive):
                for j in sorted(alive):
                    if j <= i:
                        continue
                    kl = mini.contracted_legs(legs[i], legs[j])
                    sab = mini.size_of(kl)
                    s = sab / cm - (mini.size_of(legs[i]) + mini.size_of(legs[j])) * cm
                    base = math.log(s) if s > 0 else (-math.log(-s) if s < 0 else 0.0)
                    sc = base - tp * trng.gumbel()
                    if bs is None or sc < bs[0]:
                        bs = (sc, i, j, kl)
            _, i, j, kl = bs
            cost += mini.flops_of(legs[i], legs[j])
            legs[nxt] = kl
            alive.discard(i)
            alive.discard(j)
            alive.add(nxt)
            order.append((i, j))
            # remap order entries to list positions at replay: keep ssa-like ids
            nxt += 1
        return order, cost

    root = solve(sorted(net.nodes))
    return net.ssa_path, net.flops


# ---------------------------------------------------------------------------
# SliceFinder (slicer.py, integer-exact) + slice_and_reconfigure
# ---------------------------------------------------------------------------
class SliceCosts:
    def __init__(self, tree):
        net = tree.net
        self.size_dict = {}
        self.cons = {}  # cid -> [involved:set, legs:set, size, flops]
        self.flops = 0
        self.sizes = {}
        self.flop_red = {}
        self.write_red = {}
        self.where = {}
        self.nslices = 1
        for p in tree.children:
            l, r = tree.children[p]
            involved = {ix for ix, _ in tree.legs[l]} | {ix for ix, _ in tree.legs[r]}
            legs = {ix for ix, _ in tree.legs[p]}
            size = tree.node_size(p)
            flops = tree.node_flops(p)
            self.cons[p] = [involved, legs, size, flops]
            self.flops += flops
            for ix in involved:
                d = net.sizes[ix]
                self.size_dict[ix] = d
                self.flop_red[ix] = self.flop_red.get(ix, 0) + (flops - flops // d)
                self.where.setdefault(ix, set()).add(p)
                if ix in legs:
                    self.write_red[ix] = self.write_red.get(ix, 0) + (size - size // d)
        self.original_flops = self.flops

    def overhead(self):
        return (self.nslices * self.flops) / max(self.original_flops, 1)

    def max_size(self):
        return max((c[2] for c in self.cons.values()), default=1)

    def remove(self, ix):
        d = self.size_dict[ix]
        self.nslices *= d
        for cid in self.where[ix]:
            inv, legs, size, flops = self.cons[cid]
            nf = flops // d
            self.flops += nf - flops
            for oix in inv:
                if oix == ix or oix not in self.size_dict:
                    continue
                di = self.size_dict[oix]
                old_red = flops - flops // di
                new_red = nf - nf // di
                self.flop_red[oix] += new_red - old_red
            if ix in legs:
                ns = size // d
                for oix in legs:
                    if oix == ix or oix not in self.size_dict:
                        continue
                    di = self.size_dict[oix]
                    old_sred = size - size // di
                    new_sred = ns - ns // di
                    self.write_red[oix] += new_sred - old_sred
                self.cons[cid][2] = ns
            self.cons[cid][3] = nf
            inv.discard(ix)
            legs.discard(ix)
        del self.size_dict[ix]
        self.flop_red.pop(ix, None)
        self.write_red.pop(ix, None)
        del self.where[ix]


def slice_finder(tree, target_size, seed=0, max_repeats=16, temperature=0.01,
                 forbidden=frozenset()):
    """greedy Gumbel descent, best-of-max_repeats; returns (indices, costs)."""
    best = None
    for rep in range(max_repeats):
        rng = Rng((seed << 16) ^ rep)
        costs = SliceCosts(tree)
        chosen = []
        while costs.max_size() > target_size and costs.size_dict:
            bi = None
            for ix in costs.size_dict:
                if ix in forbidden:
                    continue
                sc = math.log(costs.flop_red.get(ix, 0) + 1e-3 * costs.write_red.get(ix, 0) + 1)
                sc += temperature * rng.gumbel()
                if bi is None or sc > bi[0]:
                    bi = (sc, ix)
            if bi is None:
                break
            costs.remove(bi[1])
            chosen.append(bi[1])
        tot = costs.nslices * costs.flops
        key = (tot, costs.nslices, costs.max_size())
        if costs.max_size() <= target_size and (best is None or key < best[0]):
            best = (key, chosen, costs)
    return (best[1], best[2]) if best else (None, None)


# ---------------------------------------------------------------------------
# OUR deterministic hyper driver: LHS-stratified param draws keyed (seed, trial),
# methods {greedy, labels-divide}, per-trial reconf, best-by-objective.
# ---------------------------------------------------------------------------
def hyper_search(inputs, output, size_dict, ntrials=128, seed=0, objective="flops",
                 factor=64, reconf=True, sa_finalists=0):
    net0 = Net(inputs, output, size_dict)
    best = None
    finalists = []  # (score, tree, flops, write, size, method, t) — kept small
    half = ntrials // 2
    for t in range(ntrials):
        method = "greedy" if t < half else "labels"
        prng = Rng((seed << 32) ^ (t * 3 + 0))
        if method == "greedy":
            grng = Rng((seed << 32) ^ (t * 3 + 1))
            # stratified: costmod stratum over trials, temp log-stratified
            u0 = (t + prng.u01()) / max(half, 1) if t < half else prng.u01()
            cm = 0.1 + (4.0 - 0.1) * min(u0, 0.999999)
            tp = math.exp(math.log(0.001) + (math.log(1.0) - math.log(0.001)) * prng.u01())
            net = net0.copy()
            ok = greedy_optimize(net, costmod=cm, temperature=tp, rng=grng)
            if not ok:
                continue
            contract_remaining_by_size(net)
            ssa, fl = net.ssa_path, net.flops
        else:
            k = t - half
            parts = 2 + (k % 8)
            pdecay = prng.uniform(0.0, 1.0)
            cpow = prng.uniform(0.0, 3.0)
            ssa, fl = build_divide(net0, seed=(seed << 8) ^ (k + 1), parts=parts,
                                   parts_decay=pdecay, cutoff=16, con_pow=cpow,
                                   glue_trials=8, objective=objective, factor=factor)
        tree = Tree.from_ssa(net0, ssa)
        if reconf:
            subtree_reconfigure(tree, subtree_size=8, maxiter=256, objective=objective, factor=factor)
        flops, write, size = tree.stats()
        sc = score_tree(objective, flops, write, size, factor)
        entry = (sc, tree, flops, write, size, method, t)
        if best is None or sc < best[0]:
            best = entry
        if sa_finalists > 0:
            finalists.append(entry)
            finalists.sort(key=lambda e: e[0])
            del finalists[sa_finalists:]
    # SA polish on the finalist trees (the treesa tier), then one more reconf
    for rank, (_, tree, _, _, _, method, t) in enumerate(finalists):
        simulated_anneal(tree, tstart=1.0, tfinal=0.02, tsteps=30, numiter=30,
                         objective=objective, factor=factor, seed=(seed << 40) ^ rank)
        subtree_reconfigure(tree, subtree_size=8, maxiter=256, objective=objective, factor=factor)
        flops, write, size = tree.stats()
        sc = score_tree(objective, flops, write, size, factor)
        if sc < best[0]:
            best = (sc, tree, flops, write, size, method + "+sa", t)
    return best


# ---------------------------------------------------------------------------
# A/B vs cotengra HyperOptimizer at matched budgets
# ---------------------------------------------------------------------------
def main():
    import cotengra as ctg
    # force cotengra's NESTED random-greedy glue serial: its loky workers segfault
    # on this host (their trial errors would unfairly weaken THEIR score)
    import cotengra.pathfinders.path_basic as pb
    _orig_init = pb.RandomGreedyOptimizer.__init__

    def _serial_init(self, *a, **k):
        k["parallel"] = False
        _orig_init(self, *a, **k)

    pb.RandomGreedyOptimizer.__init__ = _serial_init
    from v14g_hyperopt_recon import bench_networks
    print("== v14-g pass 2: A/B at matched 64-trial budgets (theirs = greedy+kahypar, their hq default) ==", flush=True)
    for (tag, inputs, output, size_dict) in bench_networks():
        best = hyper_search(inputs, output, size_dict, ntrials=64, seed=3)
        sc, tree, flops, write, size, method, t = best
        print(f"[{tag:9s}] ours  log10={math.log10(max(flops,1)):8.4f} log2sz={math.log2(max(size,1)):6.2f} (via {method})", flush=True)
        opt = ctg.HyperOptimizer(methods=["greedy", "kahypar"], max_repeats=64,
                                 parallel=False, optlib="random",
                                 optlib_opts={"seed": 3}, minimize="flops",
                                 reconf_opts={})
        ct = opt.search(inputs, output, size_dict)
        st = ct.contract_stats()
        print(f"            ctg   log10={math.log10(max(int(st['flops']),1)):8.4f} log2sz={math.log2(max(int(st['size']),1)):6.2f}", flush=True)


if __name__ == "__main__":
    main()
