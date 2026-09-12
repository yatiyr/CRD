# 2026-09-06 — CEIR-31 band close (Media/UI/audio bridges — the §141 UI-effect + §142 audio proofs)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**CEIR-31 is CLOSED.** The "everything executable is an authorable asset" spine reached the media/UI/audio bridges with TWO
runnable proofs, no new schedulers: **§142 audio** — the `engine/audio` `render_graph` (a device-free, deterministic, bit-exact
CPU DSP renderer) routed through CEIR as a `ceir.audio` dialect (source/gain/biquad/delay/compressor/mix), `execute_audio_graph_ceir`
walking CEIR's block/def-use order **bit-exact vs render_graph** (which stays as the differential oracle); and **§141 UI effect** —
the frosted-glass chain authored as a `.frame.toml` of committed `.ckir` fullscreen kernels on the EXISTING RAF frame graph
(backdrop copy → half-res fetch → separable Gaussian ping-pong → tint/noise → fixed-rect mask → bindless composite), device-proven
on Vulkan + DX12 + llvmpipe. This band close (31z) resolves the four 31a rulings and flips the band row.

Detail → `docs/detours/D-007-ceir-tracker.md` (CEIR-31 rows). §141 close → `2026-09-06` 31b-z (capability `ui_effect_graph`
target→gate-only/L4). No bench: the §141/§142 proofs are CORRECTNESS/DELETION, not speed (the census ruling at 31-0).

## Slices (row per slice)

| slice | what | record |
|---|---|---|
| 31-0 | band-open census (advisor grep-verified): §141 UI-effect = an AUTHORED RAF frame, ZERO new ops; §142 audio = `ceir.audio` bit-exact vs render_graph, no new scheduler | tracker (docs-only) |
| 31a-1 | the `ceir.audio` dialect (source/gain/biquad/send/mix) + `find_audio_misuse` + committed `audio_source_gain_biquad_mix.ceir` + `execute_audio_graph_ceir` bit-exact vs render_graph | device-free (MSVC + WSL gcc) |
| 31a-2 | `audio.delay` (shift) + `audio.compressor` (feed-forward peak, linked detector, one-pole envelope, hard-knee) kernels + `sample_rate` func-attr | device-free (MSVC + WSL gcc) |
| 31a-3 | the committed `audio_full_chain.ceir` (all six nodes) + a 3-arm reading gate (anti-drift + completeness + execution) | device-free (MSVC + WSL gcc) |
| 31b-* | §141 frosted-glass — the 5 `ui_*` `.ckir` kernels + `ui_frosted_glass.frame.toml` + spec_N wiring + the Vk+DX12 device gate (incl. the g-2 extent-derived blur step) | device (Vk + DX12 + llvmpipe) |
| 31b-z | §141 close: capability `ui_effect_graph` target→gate-only/L4; glow/bloom/distortion same-recipe ledgered | docs (2026-09-06) |
| 31z | the four 31a rulings resolved + §141/§142 band-row flip ✅ | this record |

## The four 31a rulings (RESOLVED at 31z)

- **(1) 3-input mix ORDER-SENSITIVITY.** `audio.mix` sums operands in authoring order; f32 add is non-associative. Committed
  `assets/ceir/audio_mix3.ceir` (3 sources → one mix, `[dyn,2]`) + a reading gate (anti-drift-through-the-printer + completeness:
  exactly 4 ops, `mix.num_operands()==3`) + an EXECUTION gate: CEIR == render_graph **bit-exact in the authored order**, and a
  PERMUTED in-test module (`mix(a,c,b)`) **differs**. ⛔ the tooth is guarded by a REQUIRE precondition asserting f32
  `(va+vb)+vc != (va+vc)+vb` (va=1e8/vb=1/vc=-1e8 → authored 0.0, permuted 1.0 — no sign-of-zero) so a triple that happened to
  associate can't leave it inert. The 31a-3 mix was 2-input (commutative — a no-op); this is why (1) needed 3.

- **(2) feedback-echo — the `StateEdge` exemption was DECLARED-NOT-EXERCISED.** `audio.delay`/`audio.biquad` carry `OpTrait::StateEdge`.
  Structural teeth (test_audio_gate 31z-(2)): a delay-headed `delay(mix(src, delay))` back-edge → `find_structure_error==None`
  (the exemption EXERCISED); the SAME cycle Pure-headed (`gain(...)`) → `FeedbackWithoutState` (the TRAIT admits it, not the op
  name). **Executor fix** (ceir_audio_render.cpp): `execute_audio_graph_ceir` now REFUSES a back-edge (`*s >= my` → return 0),
  mirroring render_graph's Kahn cycle-refusal. This closed a **can't-fail**: the verifier BLESSED (StateEdge) a graph the single-pass
  executor would SILENTLY mis-run as an echo-less passthrough (its input bus not yet computed). The straight-line delay still
  renders (the reject is back-edge-specific). ⛔ graph-feedback EXECUTION (sample-interleaved SCC eval) is a FUTURE capability —
  no bit-exact oracle exists (render_graph refuses cycles) — ledgered, not built.

- **(4) `render_graph` delete-vs-keep → KEEP.** render_graph is the LIVE offline product path AND the bit-exact differential
  oracle; deletion-is-done does NOT apply to code the CEIR asset has not replaced (only a proof exists; realtime is deferred). The
  falsifiable claim is "no scheduler ADDED to CEIR", and it IS proven: `execute_audio_graph_ceir` walks CEIR's block/def-use order
  with ZERO Kahn/queue code. The 31-0 census "bespoke node-walk to DELETE" clause was SUPERSEDED-in-place in the tracker.

- **(5) Tensor-shape-vs-`frames` NON-CHECK.** The committed assets declared static `!dim<256>` while `execute_audio_graph_ceir`
  takes a runtime `frames` param and never checked it — a latent contradiction the 1000-frame block-invariance test drove through.
  FIX: the audio buffer's correct type is `[dyn, 2]` (runtime block size × 2 stereo channels) — committed assets `!dim<256>`→`!dim<dyn>`.
  Cook-time `find_audio_misuse` now validates the SHAPE (rank-2 [`TensorRankInvalid`]; dim1 Static==2 [`ChannelCountInvalid`];
  dim0 Static≥1-or-Dynamic, reject Symbolic/0 [`FrameDimInvalid`]). Execute-time: `frames` must EQUAL a PINNED (Static) dim0
  (equality, not ≥; a Dynamic dim0 accepts any block). The declared-words-must-be-validated-at-cook-time mandate.

  ~~(3) the committed full-chain asset~~ — ✅ done at 31a-3.

## Band-close reclassify (every `build_*`/`ensure_*` classified — the NO-C++-KGRAPH-BUILDERS audit)

No runtime C++ builder authors any audio/render ALGORITHM — every algorithm is a committed asset, LOADED. The `build_*` that exist
are ORACLES/fixtures:

- `build_audio_module` / `build_audio_full_chain_module` / `build_audio_mix3_module` (test_audio_gate) — **printer-oracles** (the
  anti-drift SOURCE for the committed `.ceir`; bootstrap-via-print, the write stripped). KEEP.
- `make_graph` / `make_graph_mix3` (test_ceir_audio_render) — the render_graph AGRF **differential-oracle** builders (ruling 4:
  render_graph IS the oracle). KEEP.
- `apply_source/gain/biquad/delay/compressor` (audio_kernels.hpp) — SHARED CPU stages consumed by BOTH render_graph and the CEIR
  executor (bit-exact by construction). Not builders. KEEP.
- `execute_audio_graph_ceir` — the CEIR **loader/interpreter** (walks a parsed `.ceir`). Not a builder.
- (31b side, closed) `ensure_ui_program` — a **loader** (`apply_spec_set` on committed `.ckir`); `blur_weights_r4_sigma_r3` — a
  numeric oracle; `build_hash_scalar` — a KGraph compute builder KEPT as a verification fixture (the 31b-1a-iii ruling: it verifies
  a hand-authored asset, builds no rendering algorithm).

## Gate (2 Windows + 2 Linux + tidy, scoped to crd-ceir + crd-audio)

| config | crd-ceir-tests `[audio]` | crd-audio-tests `[audio]` |
|---|---|---|
| win-debug | 159 assertions / 12 cases | 109721 / 22 |
| win-asan | 159 / 12 | 109721 / 22 |
| linux-gcc-debug | 159 / 12 | 109718 / 22 |
| linux-gcc-asan | 159 / 12 | 109718 / 22 |

(The 109721 vs 109718 delta is the `[soak]` realtime test's headless-host skip — 3 fewer assertions on Linux, a host-dependent
skip, not a failure.) tidy (LLVM-20): 5/5 files clean (audio.hpp, audio.cpp, ceir_audio_render.cpp, test_audio_gate.cpp,
test_ceir_audio_render.cpp). No `.ceirop.toml` change → no opgen (the all-dialects-regen scar stays asleep). No `gpu-platform-capabilities.toml`
audio row exists (it is a GPU registry) — nothing to reconcile there.

## Deferral ledger (each with a trigger — no speculative build)

- **graph-feedback EXECUTION** (a feedback echo rendered) → a sample-interleaved SCC evaluator + an independent sample-by-sample
  oracle (render_graph refuses cycles, so it cannot serve). The verifier already accepts the topology; the executor refuses to run
  it. Trigger: a consumer needing a rendered feedback delay.
- **glow / bloom / distortion** (§141, from 31b-z) → the SAME authoring recipe (a `.frame.toml` of committed `.ckir` fullscreen
  kernels); frosted-glass is the ONE §141 proof. Trigger: a UI consumer.
- the 31-0 census long tail (MED workflows · realtime/RT-safe audio + hot-reload · CLAP · UiWorld/Canvas · the I2D-7 per-panel
  `UiEffectGraph` asset question) carried forward, each to its first consumer.

## Scars (1 clause added to an EXISTING memory home — no new file)

- `scars_ceir_ir.md` (Passes/verifiers) — **a trait that WIDENS what the VERIFIER accepts must be matched at every EXECUTOR —
  run it or REFUSE it.** `OpTrait::StateEdge` made the §20 verifier accept a delay-headed feedback back-edge, but the single-pass
  `execute_audio_graph_ceir` would silently mis-run it as an echo-less passthrough — a verifier-blessed can't-fail live in-tree
  since 31a-2a. When a dialect declares a trait admitting a new structural shape, grep every executor for that shape and handle
  or refuse it (the graceful-reject convention). The EXECUTION-side mirror of the declare-verify-every-contract rule.

## Uncommitted batch (the user commits — NO AI co-author trailer)

CEIR-31 rides the ongoing CEIR-26→31 working-tree batch (last commit `c35548a "working on CEIR-25"`). The 31z additions/mods:
`engine/ceir/include/crd/ceir/audio.hpp` (+3 `AudioMisuseKind`: TensorRankInvalid/ChannelCountInvalid/FrameDimInvalid) +
`engine/ceir/src/audio.cpp` (+`audio_shape_kind` cook-time shape validation on source + every op result, +3 name cases) +
`engine/audio/src/ceir_audio_render.cpp` (+the `*s >= my` back-edge reject, +the execute-time frames-vs-pinned-dim0 check, +the
type.hpp include) + `assets/ceir/audio_source_gain_biquad_mix.ceir` + `assets/ceir/audio_full_chain.ceir` (`!dim<256>`→`!dim<dyn>`) +
**the committed `assets/ceir/audio_mix3.ceir` (NEW, LF-only)** + `tests/ceir/test_audio_gate.cpp` (`tensor_audio`→`[dyn,2]`,
+`build_audio_mix3_module`, +3 shape-misuse SECTIONs + a positive accept, +the mix3 reading gate, +the feedback structural-teeth
TEST_CASE) + `tests/audio/test_ceir_audio_render.cpp` (+`make_graph_mix3`, +the order-sensitivity / feedback-reject / frames-vs-shape
TEST_CASEs) + the tracker (the 31z rows + the ruling-(4) census strike + the band-row ✅) + this session log + `context.md` +
1 memory scar clause (`scars_ceir_ir.md`, the StateEdge trait/executor gap — see Scars above).
`git status` is authoritative; the per-slice tracker rows carry the definitive per-slice files.

## Proposed commit message (Conventional Commits — NO AI co-author trailer, per CLAUDE.md/AGENTS.md)

```
feat(ceir): CEIR-31 media/UI/audio bridges — §142 audio + §141 UI-effect proofs

Close the CEIR-31 band. §142: the ceir.audio dialect (source/gain/biquad/
delay/compressor/mix) executed by execute_audio_graph_ceir bit-exact vs the
kept render_graph oracle, no new scheduler. §141: the frosted-glass chain
authored as a .frame.toml of committed .ckir fullscreen kernels on the
existing RAF frame graph, device-proven on Vulkan + DX12 + llvmpipe.

Band close (31z) resolves the four 31a rulings: (1) a committed 3-input-mix
asset + an order-sensitivity execution gate (CEIR == render_graph in operand
order, a permuted order differs); (2) the delay StateEdge feedback exemption
exercised (verifier accepts a delay-headed back-edge, rejects a Pure-headed
one) and the executor now refuses a feedback cycle (mirroring render_graph's
Kahn refusal, closing a can't-fail); (4) render_graph KEPT as the live path +
differential oracle; (5) the audio buffer type corrected to [dyn,2] with
cook-time shape validation and an execute-time frames-vs-pinned-dim0 check.
```
