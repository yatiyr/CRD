# 2026-11-07: no GPL-derived code in Cerid

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Rule: [copyleft](../design/pinned-inputs.md#copyleft).
User order 2026-11-07: no GPL-derived code in Cerid; rewrite from published sources unless a comparison clearly shows no copy.

## What was compared, and what it showed

`link_models.hpp` and `link_models.cpp` against the pinned `sch_netem.c` (GPL-2.0, Linux 6.12) and iproute2's `netem/*.c`:

- **Copied.** `CorrelatedDraw::next` is `get_crandom`'s expression; `next_four_state` and `next_gilbert_elliott` are
  `loss_4state` and `loss_gilb_ell` with their `a1` to `a5`, branch order and threshold gaps; `netem_delay` is `tabledist`;
  `SlotGate` is `get_slot_next` and `netem_dequeue`'s slot lines; `netem_peer.c` committed kernel declarations and restated
  `netem_dequeue`. The header's "word for word" was true.
- **Not GPL.** iproute2's table generators are marked NIST-PD; `DelayProcess` was Cerid's own.

The first rewrite restated the models from the Gilbert, Elliott, Salsano and Clark papers with different shapes and gave
up exact agreement with netem. That was withdrawn: the user's rule is that a result may match another implementation
exactly, and only the text must be ours. The models are now restated function by function in Cerid's own text with
results byte-identical to the pre-sweep commit `965e31de` (3dda87d6): `CorrelatedDraw`, Gilbert-Elliott stepping by arrival
state, the four-state window, `netem_delay`, `SlotGate` and the gilbert helpers. The `Table` shape, `pareto_share` and
`MediumGrants` stay. `LossChain`, `LinkChance` and three older test cases went with the rewrite; each has a `965e31de`
counterpart that covers the same behaviour.

`sch-netem` and `iproute2` left `cmake/pins.json`. No netem source is read or compiled: `crd-hesap-link-netem-tests` runs
under `unshare -Urn`, sets each law through `tc` and sends numbered datagrams over loopback. Its slot case was dropped:
netem's slots served about 2,800 packets a second where five packets per 5 ms grant serves 1,000 (WSL 6.6.87), so the two
are different models and the grants answer to their own definition.

## The sweep

Tree-wide search for GPL, LGPL and AGPL names and for declared ports across engine, tools, tests and scripts. Each
rewrite below was compared against `965e31de` on a cloud machine (interleaved rounds, medians): outputs byte-identical and
no function more than 2% slower.

| Where | Declared | State |
|---|---|---|
| `multifrontal_ldlt.cpp` `amalgamate_fronts` | `cholmod_super_symbolic` | **Landed** 9636c5a7; 0.42x to 0.80x of old's time |
| `link_models.hpp`/`.cpp` | netem | **Landed** 3dda87d6; worst ratio 1.006 |
| `hesap-ordering/src/symbolic.cpp` | `cs_etree`, `cs_post`, `cs_tdfs`, `cs_leaf`, `cs_ereach`, `cs_counts` | **Landed** 3c7c6cd3; eight kernels, worst 1.004; `fundamental_supernodes_i32` left (a short formula) |
| `hesap-direct/src/sparse_lu.cpp` | `cs_lu`, `cs_dfs` | **Landed** 58ba2e0a: `lu_dfs`, `lu_spsolve`, `factorize`; worst 0.98 |
| `hesap-direct/src/lu_symbolic.cpp` | KLU-like | **Landed** e9675c3e: `lu_dfs_pattern`; 0.79 to 0.95 |
| `hesap-ordering/src/amd.cpp`, `camd.cpp` | "`cs_amd` port" in comments | **Read, unchanged**: data layout and organisation are Cerid's own; only comments and phase labels name `cs_amd` |
| `supernodal_cholesky`, `dense_lu_kernels.hpp`, `multifrontal_lu.hpp`, the pruning loop in `lu_symbolic` | CHOLMOD, UMFPACK, KLU names in comments | **Read by memory only**; the pruning loop may be close to KLU's and was not compared against its source |

Read and clean: zstd (taken under BSD-3-Clause, now stated by `license_taken`), the NLopt ports (MIT by each algorithm's
COPYRIGHT file; Luksan, which is LGPL, is not ported), Shewchuk's predicates (public domain), Cephes (BSD-3 per the notice
file), the L-BFGS-B translation (BSD-3), METIS (Apache-2.0, no METIS code present). GSL, FFTW, TetGen and CGAL appear only as
comparison peers. Copyleft pins left are `tools` run as separate programs. A comment search found no file declaring a port of
GPL, LGPL or AGPL code in engine, tools or scripts.

## Evidence

- `crd-hesap-net-tests [link-models]`: 16 cases, 911,431 assertions; the fuzz corpus replays 33 inputs with none
  rejected. Delay digests are unchanged.
- `crd-hesap-link-netem` against the running kernel's netem: the netem-tests binary passed at the 18b413ab build.
- Teeth, earlier round: 15 of 18 caught; two are equivalent mutants and one did not compile.
- [Board](../bench/2026-11-07-hesap-net-link-models.md). `crd-pins` refuses a GPL-family package that is neither a named
  permissive option nor a gated comparison, with its test.
- Amalgamation: the new `amalgamate_fronts` is byte-identical to the old one (87 fronts at relax 4) on 10 matrices at 13
  relax values. Its bounds cite Algorithm 887 (Davis, Rajamanickam, Sid-Lakhdar survey) as well as Ashcraft and Grimes 1989
  and Duff and Reid 1983.
- Tests: `crd-hesap-direct-tests` 143,833 assertions in 205 cases and `crd-hesap-ordering-tests` 7,771 in 44 passed on the
  machine after the rewrites; hosted CI has not yet run on e9675c3e.

## Next

- Compare the pruning loop in `lu_symbolic` and the supernodal, dense LU and multifrontal LU files against their upstream
  sources, and reword the `cs_amd` comments in `amd.cpp` and `camd.cpp` to cite the papers.
- Stale words: `pstack` in comments at `lu_symbolic.cpp` and `sparse_lu.cpp`; `symbolic.hpp` comments naming `cs_etree` and
  `cs_counts`.
- `docs/ROADMAP.md` row 122 quotes 8.2 million `crd-fuzz-hesap-link` executions that belonged to the earlier target; run it
  bounded under libFuzzer on a clang lane or correct the count.
- The hosted run must confirm `Allow netem in a user namespace` on linux-gcc-public-checks.
