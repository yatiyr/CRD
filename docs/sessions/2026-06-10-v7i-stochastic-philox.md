# 2026-06-10 — hesap v7-i CLOSE: stochastic/ML optimizers + the Philox v12-pull (same session, part 5)

**Phase:** 3.1.6 `crd-hesap` · v7 optimisation (`crd-hesap-opt`, ADR-0090) + a v12 pull
**Slices:** the v12-PULL (new module `crd-hesap-stats` with the Philox counter-RNG) then **v7-i complete**.
Sequencing per user direction: rather than moving v7-i to the statistics cluster or shipping it with the
pinned moat asterisk ("minibatch reproducibility needs the v12 counter-RNG"), the tiny frozen-interface RNG
was pulled forward first — v7-i then ships WHOLE. Continues the v7-f/g/h/j logs.

---

## Part 1 — the v12-pull: `crd-hesap-stats` + Philox4x32-10

New module (engine/hesap-stats; edges: core + containers only — a leaf primitive):
- `philox4x32(counter, key)` — the constexpr PURE block function (Salmon-Moraes-Dror-Shaw SC'11), **verified
  against the three published Random123 known-answer vectors** (zero / all-ones / π-digits — together they
  probe both round multipliers, both key-schedule constants, and the 10-round structure; a coincidental match
  is impossible).
- `PhiloxRng(seed, stream)` — 64-bit seed × 64-bit stream × 64-bit position; `next_u32/u64/f64/f32`
  (full-mantissa uniforms), unbiased `next_below`, **O(1) random access** (`jump_to_block`).
- `shuffle` — deterministic Fisher-Yates.
Counter-based ⇒ the moat is BY CONSTRUCTION: a pure function of (seed, stream, position) has no execution-order
or worker-count dependence. Tests: KAT + purity + wrapper self-consistency + stream separation + U(0,1)
moments + permutation/reproducibility of the shuffle. System-doc stub: `docs/systems/hesap-stats.md`.

## Part 2 — v7-i: the stochastic/ML optimizer family

`stochastic.hpp` (umbrella): ALL TEN steppers in the torch.optim shape (caller owns the loop; stepper owns the
moment state), implemented to the reference formulas with PyTorch's documented default semantics:
SGD (+momentum/Nesterov/dampening, torch first-buffer rule) · Adam · AdamW (decoupled decay) · Nadam (the
PyTorch μ-product schedule) · RAdam (ρ>5 rectification gate) · RMSprop (+momentum) · Adagrad · Adadelta ·
Lion · LAMB (trust ratio; single parameter group — layer grouping is the caller's slicing). Plus LR schedules
(step / exponential / cosine-annealing / linear-warmup) and gradient clipping (by norm / by value).

`minibatch.hpp`: the Philox-backed **MinibatchSampler** — the epoch-e permutation is the Fisher-Yates shuffle
under `PhiloxRng(seed, stream = e)`, a pure function of (seed, epoch): same batches in any visit order, on any
worker count; replay can jump straight to epoch 17. New acyclic edge `crd-hesap-opt → crd-hesap-stats`.

## Tests — Philox 5 cases · v7-i 271 asserts / 9 cases · opt suite 915 / 73

- **Exact closed-form first/second steps for every rule**, written from the reference formulas independently
  of the implementation (catches bias-correction / state-order / update-order transcription bugs; e.g. RAdam's
  t=1 branch must be exactly Δx = −lr·g — un-rectified, no adaptive denominator).
- All ten converge on a strongly convex quadratic (deterministic budgets; LAMB under cosine annealing and Lion
  at small lr — NORMALIZED updates orbit the minimizer at the lr scale, the textbook behavior, so decay is the
  correct test harness, not a fudge).
- Sampler: true permutation per epoch (short-last-batch boundary, n=103 prime) + **out-of-order epoch
  reproduction** (visit 0,1,2 vs jump-to-2 ⇒ identical batches).
- **Two moats:** full-batch Adam over the parallel-but-bit-exact spmv — trajectory bit-identical
  {1,2,4,8,16}; and the full minibatch-SGD pipeline (sampler + momentum-SGD + per-epoch decay) bit-identical
  across independent runs while actually learning the regression slope.
- n = 0 steps are no-ops.

## Verification (module-local; CI owns the sweep)

win-debug: opt 915/73 + stats green + 5 source guards · win-asan both exit 0 · win-shipping both exit 0 (the
healed dir; the dependency closure rebuilt correctly on the umbrella change — deps tracking confirmed working)
· win-tidy both build clean. Builds invoked with the EXPLICIT standalone CMake per the new policy.

## Honest notes

- Gold parity vs live PyTorch trajectories is the v7-z scoreboard (gated script, the lbfgs_ref pattern); the
  in-tree closed-form gates ARE torch's documented update formulas, so the per-step math is pinned now.
- Two test-budget fixes during bring-up were tolerance/practice issues, not logic bugs (LAMB needs decay to
  converge tightly; constant-lr minibatch SGD has the classic noise ball ⇒ per-epoch decay) — both now
  exercise `set_lr` + schedules in real loops.

## Next

Back to the constrained spine: **v7-k ⭐ QP** (Goldfarb-Idnani active-set · Mehrotra IPM · OSQP-class ADMM).
