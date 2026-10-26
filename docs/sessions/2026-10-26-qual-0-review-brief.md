# QUAL.0 review brief (2026-10-26)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md#slice-qual.0). Rules: [AGENTS](../../AGENTS.md).

Planning only: no code, benchmark or runtime result. Prepared ahead of the row so the user can decide while DIAG closes.
QUAL.0 reviews the cross-domain qualification contract ([system quality contract](../design/system-quality-contract.md),
[ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md), finding
[G01](../research/2026-09-12-cerid-whole-system-review.md#finding-g01)). The contract text already exists; what QUAL.0
needs from the user are the choices below, which the contract leaves open.

## Decisions for the user

1. **Minimum x64 CPU.** The user chose one ISA level (AVX2) for the whole program on 2026-10-26 (DIAG.11b (r)). That
   makes AVX2 (Intel Haswell 2013+, AMD Zen/Excavator) the floor unless runtime dispatch keeps a lower one. The CI
   matrix still runs `*-sse2` and `*-scalar` lanes. Choose:
   - (a) AVX2 is the supported floor; the sse2/scalar lanes stay only as code-path tests of the SIMD dispatch, not as
     supported products;
   - (b) keep SSE2 as a supported floor through runtime dispatch, and AVX2 is the default tuning only.
2. **GPU qualification cells for "Windows/Linux first".** The contract asks for representative Intel/AMD/NVIDIA GPUs.
   Today's evidence: one local NVIDIA RTX 4070 Ti SUPER plus software adapters in CI (WARP, lavapipe; accepted as
   software evidence 2026-10-05). Choose for AMD and Intel GPUs:
   - (a) leave them as named open gates until hardware is available;
   - (b) rent cloud GPU machines when a row needs them (each is a cost and a download/setup the user approves);
   - (c) borrow physical cards for dated evidence runs.
3. **Native Linux display.** WSL does not prove a native Linux compositor/driver path (contract, "Cross-platform").
   PRESENT.3 needs Wayland/X11 on real hardware. Choose: a dual-boot partition on this machine, a second machine, or an
   open gate until one exists.
4. **Peer oracles per domain** (each later download is approved separately). Proposed starting set:
   - profiling/diagnostics: Tracy 0.14.1 (pinned, done);
   - dense/sparse numerics: Eigen, the vendor BLAS already boarded (cuBLAS), and MKL or OpenBLAS on CPU;
   - renderer images: a reference path tracer (Mitsuba 3 or pbrt-v4) for ground truth, plus Filament for real-time PBR;
   - physics: Jolt and PhysX for matched scenes, analytic/invariant oracles first;
   - UI: none until I2D.
   Accept, change, or add peers.
5. **Who sets budgets.** The contract says "predeclare budgets and significance before optimizing; no invented budgets".
   Choose the rule:
   - (a) a budget is the current measured baseline plus a stated margin, recorded before the change;
   - (b) the user sets each budget number;
   - (c) peer-relative: "no worse than the best matched peer".
   Today's DIAG budgets (1% disabled, 2% basic) were set by ADR-0133 and fit (a).
6. **Agent-driven products.** ADR-0130's mechanisms (shared reflection/command model for GUI/CLI/MCP/agents, trust modes,
   merge rules) are marked "Proposed". QUAL.0 only reviews the contract; each owning slice decides its mechanism.
   Confirm that split, or name mechanisms to accept now.

## Recommendation

1(a), 2(a) with 2(b) for rows that cannot close otherwise, 3: dual boot when PRESENT.3 arrives, 4 as proposed,
5(a) with peer-relative claims reported separately, 6 confirm the split.

## What QUAL.0 then does (after AUD-2)

Record the choices in the contract and ADR-0130, write the platform tuple table and the peer/oracle list as dated
evidence pointers, and mark each later row's applicable sections. It changes no code.
