# Lesson 16 — Autotuning: finding the fastest way to run a kernel

> The story of the AS band (ADR-0098 §4). Written for the engineer who asks "what *is* autotuning, and why does a compiler need it?" — the answer is the difference between "CKIR runs everywhere" and "CKIR runs everywhere *and is the fastest*."

## The fact that starts everything

**There are many *correct* ways to run the same computation on a GPU, and they differ in speed by 10–50×.**

Matrix multiply `C = A·B` is the canonical example. The *answer* is fixed. But *how* you organize the work is wide open:

- How do you chop the matrices into **tiles** (small blocks that fit in fast on-chip memory)?
- How many threads per tile, and how much output does each thread compute (its **register tile**)?
- How much do you stage in shared memory before touching slow global memory?
- Do you pre-load the next tile while computing the current one (**double-buffering**)?

Every combination computes the *identical* result at a *different* speed. And critically: **there is no closed-form for the best one.** It depends on the exact GPU (shared-memory size, register file, core count, DRAM bandwidth), the problem shape, and how they interact. cuBLAS was tuned over *years* by measuring. You cannot predict the winner — you measure.

**Autotuning = generate the valid ways → run each on the real hardware → keep the fastest.** Automatically.

## The warehouse analogy (how tiling actually works)

Fulfilling a giant order (the output matrix):

- Split the floor into **zones** (tiles), one **crew** (thread block) per zone.
- Each worker keeps current items on a small **cart** (registers); the crew shares a **staging table** (shared memory).
- **Zone too big** → the staging table overflows and carts run out of room, so fewer crews fit at once (**low occupancy**) → slow.
- **Zone too small** → crews spend their time walking to far shelves (waiting on slow DRAM) instead of packing → slow.

The sweet spot balances **reuse** (big enough tiles that data loaded once is used many times) against **occupancy** (enough crews resident to hide memory latency). Where that balance lands depends on the warehouse. You find it by trying layouts and timing them — then you **write the winner on a card and reuse it**.

## Schedule vs graph — the two-level idea

CKIR separates two things (ADR-0098's two-level IR):

- **CKIR-Graph** = *what* to compute — the math. `C = A·B + bias; out = SiLU(C)`. Backend-neutral, differentiable, bit-exact-checkable.
- **CKIR-Tile (a `TileSchedule`)** = *how* to run it on the hardware — tile sizes, thread mapping, double-buffering. This is where speed lives.

**Same graph, many schedules.** The autotuner's whole job is: given a graph, pick the schedule. `select_schedule(graph, node)` is that decision.

## The pipeline we built (each concept → the code)

| Concept | Code |
|---|---|
| A candidate "how" | `TileSchedule` (`ckir_tile.hpp`): `bm/bn/bk`, warp tile, `tm/tn`, `nt`, `double_buffer`, `fma` |
| All *valid* candidates | `enumerate_contract_schedules` (`ckir_autotune.hpp`): the ~1516 that satisfy every CUTLASS block→warp→thread constraint + fit the device (shared mem / registers / occupancy / divisibility) |
| Measure one on the GPU | `KirBackendCuda::time_contract_schedule`: compile → upload → GPU-event-time → readback |
| **Determinism gate (the CKIR twist)** | every candidate is validated against the CPU oracle. **A fast-but-WRONG schedule can never win.** Vendor autotuners trust the kernel; ours cannot ship a wrong one |
| Prune the search | the **analytical cost model** `predict_contract_ms` (roofline × occupancy): predicts the top-K worth measuring, so we measure ~6 not 1516 — 253× fewer compiles |
| Remember the winner | a **checked-in DB** (`ckir_tuning_db.inc`), replayed by `select_schedule` — *tune offline once, replay at runtime forever, never tune at runtime* (the determinism guarantee) |
| Run the search offline | the `kir_autotune` CLI (`tools/kir-autotune`) → regenerates the DB from on-GPU measurements |

Order of the search: **enumerate → cost-model-rank → measure top-K → oracle-validate → keep fastest correct → write to DB.**

## Two things that make it CKIR's autotuner, not a Triton clone

1. **Determinism-gated.** Every measured candidate is certified correct against the CPU reference before it can win. This fuses the two moats — *fast* and *provably correct* — into one search.
2. **Offline DB → deterministic replay.** Tuning happens offline; the winner is checked in; runtime just looks it up. So performance is reproducible and inspectable, not a runtime black box. (Vendor kernels are black boxes.)

## Three GPU lessons the search surfaced (each cost real debugging)

- **The cost model MUST count the register file.** A 256²-tile wants ~190 registers/thread × 512 threads = ~98K > the 64K/SM register file — *zero* blocks fit. Model occupancy from smem + threads only, and it ranks these fat slow tiles best (8× off). Registers are the limiter big tiles hit first. → [[feedback_gpu_cost_model_must_include_register_occupancy]]
- **Bit-exact compile flags cripple the fast tier.** The backend compiled *everything* with `--fmad=false` (for cross-backend bit-exactness), which disables FMA fusion and *halves* GEMM throughput. But that flag belongs only on the bit-exact tier — the ULP fast tier must compile `--fmad=true`. Gating flags per-tier lifted CKIR from 0.49× → 1.04× cuBLAS. → [[feedback_fast_tier_must_enable_fma_bitexact_flags_cripple_gemm]]
- **A determinism test must compare the *same* schedule twice.** With FMA on, different tile configs contract differently (each still deterministic run-to-run) — so comparing *different* configs' outputs falsely "fails" determinism. Only same-config replay is bit-identical; that's correct for the ULP tier.

## Why a compiler needs this (the mission)

"Runs everywhere, bit-exact" is half the mandate. The other half is "**and the fastest**." A hand-written tuning table only covers shapes a human measured; everything else falls back to a slow reference. The autotuner makes **vendor-beating speed a *property of the compiler*** — a new shape or a new GPU gets the best schedule found by search, not by luck. It is the piece that separates a *translator* (Slang, naga) from an *optimizing compiler* (Triton, cuBLAS, IREE).

## What it won (measured live, oracle-correct)

- **Fused MLP: CKIR crushes cuBLAS 2.40–2.88×.** GEMM+bias+SiLU in one kernel vs cuBLAS Sgemm + a separate epilogue pass. The autotuner tunes the fused kernel; the fusion saves a whole memory pass cuBLAS can't avoid. This is the structural moat.
- **Raw GEMM: parity — CKIR beats cuBLAS Sgemm on 1024³ (1.04×, 13107 GFLOP/s)** at matched f32, after the FMA fix.
- **Honest gap:** large compute-bound GEMM — cuBLAS SASS (`cp.async`) leads ≤2×. That's a kernel-family/tensor-core frontier, not a scheduling one, and it's on the record (`docs/bench/2026-07-22-as4-ckir-gemm-vs-cublas.md`).
- **Shape-general:** rectangular MLP GEMMs tuned to 26153 GFLOP/s, 14.3× naive; the DB is a real square+rectangular ladder.

## The pattern to reach for next time

When you have a kernel with knobs that don't change the *answer* but change the *speed*, and you can't predict the best knobs: **make the knobs a struct, enumerate the valid settings, measure on the real hardware, validate each against a reference, and cache the winner.** Prune the search with a cheap physical cost model so you measure tens, not thousands. Tune offline, replay from a checked-in table. That is autotuning, and it is how a portable compiler earns the right to say "the fastest."
