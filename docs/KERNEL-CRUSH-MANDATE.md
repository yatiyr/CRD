# KERNEL CRUSH MANDATE — binding orders for kernel/perf work

> **This is not advice. These are ORDERS.** They bind every agent (human or AI) the moment you optimize a compute
> kernel (GPU or CPU) against a peer — a vendor library (cuBLAS/cuDNN/MKL/oneDNN/BLAS), a published kernel, or any other
> implementation. Read this before you touch a hot kernel. Violating an order = failing the task, not "using judgment."
> Origin: 2026-07-08, user callout — "if a vendor on the SAME machine is faster, parity is not impossible; we do not
> stop until we reach parity." Memory: `feedback_never_invoke_a_wall_a_peer_already_beat`. Techniques: `docs/hints/crush-playbook.md` §H.

---

## THE PRIME LAW — the existence proof
**If a peer reaches a number on the SAME hardware, that number is PROVEN achievable.** It is a fact, not a hope.
Therefore, for the rest of the task:
- **"Impossible" / "we can't reach it" is FORBIDDEN.** The only permitted framing is *"I have not found how yet."*
- **The gap is YOUR implementation's shortfall until measured otherwise** — never the hardware's, never a "wall".
- **You do not stop until PARITY (match the peer) or CRUSH (beat it).** Anything less is an open bug.

## THE SEVEN ORDERS

**ORDER 1 — PIN THE TARGET FIRST.** Before optimizing anything, MEASURE the peer on *this* box under identical
conditions (clock-locked, same size, warmed, kernel-only / steady-state timing). Never optimize against a number you
guessed. You must know the exact TFLOP/s (or ns) you are chasing.

**ORDER 2 — MEASURE, NEVER GUESS.** Every change is prescribed by a profiler counter, never by intuition or "this should
help." Loop: *profile → identify the ONE dominant limiter → fix exactly that → re-measure.* A speculative edit that
wasn't prescribed by a counter is a violation.

**ORDER 3 — NO WALLS WITHOUT PROOF.** You may NOT stop and cite a "wall / nerf / ceiling / SASS limit / memory wall /
diminishing returns" UNTIL you have, in order: **(a)** reproduced the best *public hand-written* result for this op on
this architecture, **(b)** still have a measured gap, AND **(c)** empirically TESTED the wall hypothesis (disassemble to
confirm the instruction mix; show the limiting counter is literally at hardware max; show the peer hits the same counter
ceiling). A wall is a MEASURED CONCLUSION with evidence — NEVER a first-reach assumption at 50-60% of peak.

**ORDER 4 — EXHAUST EVERY LEVER.** Work the full checklist below. You may not declare "done" or "ceiling" with an
unchecked lever. If you haven't tried it, you haven't earned the right to say it wouldn't help — measure it.

**ORDER 5 — ESCALATE, DON'T QUIT.** On a plateau, climb this ladder — do not stop mid-ladder:
  1. **Re-profile deeper** — exact stall reasons, SASS/roofline, bank conflicts, per-instruction.
  2. **REVERSE-ENGINEER the peer** — profile the vendor kernel *itself* with the same tools; read ITS occupancy, tile
     shape, register count, instruction mix; disassemble it (cuobjdump / SASS). Learn what it does that you don't.
  3. **DEEP-RESEARCH** — read CUTLASS/CUB/oneDNN/BLIS source, the vendor whitepapers, the SOTA blogs/papers, the public
     hand-written best. Someone wrote down how. Find it.
  4. **AUTOTUNE** the full config space (below).
  5. **Outside-the-box structural change** — a different algorithm, precision tier, memory layout, or instruction class.
  Only after ALL rungs, WITH Order 3 satisfied, may you report a measured frontier.

**ORDER 6 — AUTOTUNE, DON'T HAND-PICK.** The headline number is the swept-config winner (dozens of configs), not 2-3
hand guesses. The peer's number is autotuned; so must yours be before any comparison is fair.

**ORDER 7 — HONEST MEANS KEEP GOING.** "Honest" = reporting REAL numbers + REAL diagnosis. It NEVER means stopping and
declaring the crush impossible, or reframing a hard grind as "diminishing returns" or "let's ship what we have." A
documented loss is an OPEN BUG (SANITY #9). Honesty and persistence are the same order here.

---

## THE EXACT RECIPE (the loop you execute)
1. **Pin the peer** (Order 1) — measure cuBLAS/MKL/etc. on this box. Record it.
2. **Baseline** — simplest correct kernel; measure kernel-only (cudaEvent / rdtsc), warmed, clock-locked, size > last-level cache.
3. **Profile** — SpeedOfLight FIRST (decode the bound type), then the dominant stall / counter.
4. **Decide from the counter** (tables below) — one dominant limiter.
5. **Fix exactly that. Verify correctness** (bit-exact or matched tolerance vs a reference). **Re-measure.**
6. **Repeat 3-5** until parity/crush. Every iteration must move a counter, or you fixed the wrong thing.
7. **Autotune** the survivor across the config space.
8. **Report** only at parity/crush, or a ceiling that satisfies Order 3 + Order 5.

## COUNTER → FIX (GPU, Nsight Compute — `ncu -c 1 -k <regex> --section SpeedOfLight --section Occupancy`)
| symptom | bound | fix |
|---|---|---|
| SM% low + Mem% low | latency/occupancy | more warps (cut regs/shared), more ILP, prefetch |
| Mem-busy high + DRAM low + L2-hit high | **shared** bandwidth | raise arithmetic intensity (warptiling), vectorize shared reads |
| DRAM% high | memory | better reuse (bigger tiles), coalescing |
| SM% high but FLOP low | wrong instrs (address/int) | hoist/vectorize addressing, cut overhead ops |
| `long_scoreboard` | global latency | double-buffer / `cp.async` prefetch |
| `short_scoreboard` | shared latency | inner-loop register prefetch; reduce shared traffic |
| `barrier` | too many syncs | double-buffer (ping-pong) → fewer barriers |
| bank conflicts high | shared layout | XOR-swizzle / pad the layout (16B-aligned swizzle for cp.async) |
| Block-Limit-Registers low | register-limited | smaller microtile, `__launch_bounds__`, or warptiling |

## THE LEVER CHECKLIST (exhaust before any "ceiling")
**GPU:** shared-memory tiling · register microtiling · `float4`/128-bit vectorized loads · transposed/XOR-swizzled shared
layout (conflict-free) · double-buffering (register-prefetch AND `cp.async`, multi-stage 3-4 deep) · warptiling
(block→warp→thread) · occupancy tuning (registers / shared / `__launch_bounds__`) · bank-conflict elimination · **tensor
cores** (`wmma`/`mma`/`coopMatMulAdd`) where the precision tier allows · **autotune all tile params** (BM/BN/BK/WM/WN/TM/TN).
**CPU:** full SIMD width (AVX-512 / AVX2 / NEON) · register blocking · cache blocking (L1/L2/L3 tiles) · panel **packing**
(contiguous, aligned) · FMA · software prefetch · loop unrolling · NUMA-aware placement · matched thread count · alignment ·
no false sharing · **autotune block sizes**.

## FORBIDDEN (banned rationalizations — saying any of these to stop is a violation)
- "It's impossible / we can't reach it." (A peer on the same hardware proves otherwise.)
- "This is the ceiling of [FP32 CUDA-C / scalar C / …]" — without first reproducing the public SOTA.
- "The SASS wall / the nerf / the memory wall" as a first-reach excuse (allowed ONLY as a measured Order-3 conclusion).
- "Diminishing returns" / "the mission-aligned move is to port what we have" to escape the grind.
- Reporting "X% of peak" and stopping while the peer gets a higher %.

## WHEN YOU MAY STOP
ONLY when **(a)** you match or beat the peer (PARITY / CRUSH — the goal), OR **(b)** you have reproduced the best public
hand-written result, still have a gap, AND empirically PROVEN (Order 3) the residual is a hardware limit the peer *also*
cannot cross (e.g., the peer sits at the same counter ceiling) — that is the achievable frontier, and matching it IS the
crush. Nothing else is a valid stopping point.
