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

Everything netem-derived is deleted and rewritten from the Gilbert, Elliott, Salsano and Clark papers: one `LossChain` table,
`LinkChance` (a word below it hits, so zero and certainty are exact), no correlated word (the two-state chain gives rate and
correlation exactly), a `Table` delay shape, `pareto_share` instead of netem's weights, and `MediumGrants`, a pure
departure-time function. Exact agreement with netem is given up.

`sch-netem` and `iproute2` left `cmake/pins.json`. No netem source is read or compiled: `crd-hesap-link-netem-tests` runs
under `unshare -Urn`, sets each law through `tc` and sends numbered datagrams over loopback. Its slot case was dropped:
netem's slots served about 2,800 packets a second where five packets per 5 ms grant serves 1,000 (WSL 6.6.87), so the two
are different models and the grants answer to their own definition.

## The sweep

Tree-wide search for GPL, LGPL and AGPL names and for declared ports across engine, tools, tests and scripts.

| Where | Declares | Licence | State |
|---|---|---|---|
| `multifrontal_ldlt.cpp` `amalgamate_fronts` | "faithful port" of `cholmod_super_symbolic`; compared: same names, flow and defaults | GPL-2.0+ | **Rewritten here** |
| `hesap-ordering/src/symbolic.cpp` | `cs_etree`, `cs_post`, `cs_tdfs`, `cs_leaf`, `cs_ereach`, `cs_counts` | CSparse LGPL-2.1+ | **Open** |
| `hesap-ordering/src/amd.cpp`, `camd.cpp`, `nested_dissection.hpp` | "`cs_amd` port" and its copy | CSparse LGPL-2.1+ | **Open** |
| `hesap-direct/src/sparse_lu.cpp`, `sparse_lu.hpp` | "faithful CSparse `cs_lu`", `cs_dfs` | CSparse LGPL-2.1+ | **Open** |
| `supernodal_cholesky.hpp`/`.cpp` (`Head`, `Next`, `Lpos`), `dense_lu_kernels.hpp`, `multifrontal_lu.hpp`, `lu_symbolic.cpp` | names or techniques of CHOLMOD, UMFPACK, KLU | GPL-2.0+, LGPL-2.1+ | **Open**, not yet compared |

Read and clean: zstd (taken under BSD-3-Clause, now stated by `license_taken`), the NLopt ports (MIT by each algorithm's
COPYRIGHT file; Luksan, which is LGPL, is not ported), Shewchuk's predicates (public domain), Cephes, the L-BFGS-B translation (BSD), METIS (Apache-2.0). GSL,
FFTW, TetGen and CGAL appear only as comparison peers. Copyleft pins left are `tools` run as separate programs.

## Evidence

- `crd-hesap-net-tests [link-models]`: 13 cases, digests re-pinned. Delay digests for uniform, normal, log-normal and Pareto
  did not change, so `DelayProcess` kept its stream.
- `crd-hesap-link-netem` against the running kernel's netem: independent, simple and full Gilbert-Elliott, four states (a
  million packets each, chi-square on loss and received runs at about 1e-6) and uniform and normal delay (quantiles).
- `crd-fuzz-hesap-link` rewritten and reseeded: 8.2 million ASan and UBSan executions, no finding.
- Teeth: 16 of 18 caught; two are equivalent mutants (a lapse's `leaves` equals `ready_ns`; equal bytes give zero either way).
- [Board](../bench/2026-11-07-hesap-net-link-models.md). `crd-pins` refuses a GPL-family package that is neither a named
  permissive option nor a gated comparison, with its test.
- The amalgamation rewrite (Ashcraft and Grimes 1989, Duff and Reid 1983), natural-order 16^3 stencil, new against old at
  relax 4: 80 against 87 fronts, 1,111,434 against 1,110,018 nonzeros, 32.6 against 31.5 ms in one run each, same residual.

## Next

Rewrite the open rows from Liu 1986 and 1990, Gilbert, Ng and Peyton 1994, Gilbert and Peierls 1988 and Amestoy, Davis and
Duff 1996; AMD and CAMD proper are BSD-3-Clause and may be read. The hosted run must confirm `Allow netem in a user
namespace` on linux-gcc-public-checks.
