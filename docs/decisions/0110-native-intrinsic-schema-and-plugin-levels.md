# ADR-0110 — Native-intrinsic schema, the legitimacy rule, and the three plugin-extension levels

**Status:** **ACCEPTED** (2026-08-07, user-approved at the CEIR-0d gate) — the D-007 **CEIR band**. Builds on
ADR-0108 (owned language stack) + ADR-0109 (layer contract + `crd-ceir` placement). Answers the open question
`docs/design/ceir-0a-execution-path-inventory.md` §10 raised: do the atomic capabilities the inventory found
(`present`, scene resolvers, `submit_overlay`, media codecs, swapchain) fit the §100 intrinsic schema cleanly?
Refined-by / sibling-to CEIR-0e.
**Phase:** D-007 (CEIR programme). Law: mission `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md`
§100 (native intrinsics), §101 (plugin levels), §99 (capability security), §69 (providers), §178 (must-not-stay-native).
**Tags:** `[ceir]` `[intrinsics]` `[plugin]` `[capability]` `[architecture]` `[extensibility]` `[substrate]`

---

## 1. Context

CEIR's open-world completeness (§6) rests on a bright line: **an algorithm is a program asset; a genuinely new
host/hardware primitive is native C++** (§1 mantra, §177/§178). If that line is fuzzy, either (a) people smuggle
composite algorithms back into native code (`ForwardPlusExecutor` — the `FramePassKind` mistake RAF deleted), or
(b) the escape hatch is so restricted that a real device primitive (present, a codec, an NPU call) has nowhere to
live. This ADR draws the line with a **typed intrinsic schema** (so a native op is as inspectable/verifiable as any
other) and a **legitimacy test**, and it maps the three ways an app/plugin extends CEIR (§101) onto it.

## 2. Decision — a native intrinsic is an ordinary CEIR op with a native binding (not a parallel system)

An intrinsic is **a CEIR op defined via the normal CEIR-2 schema generator** (ADR-0109 §5 / mission §8), carrying
the standard fields (typed operands/results, attributes, effects, domain, capability requirements, verifier), PLUS
the §100 **native-binding metadata** and a registered native **handler**. There is NO separate intrinsic IR — an
intrinsic op prints, verifies, serializes, and appears in the graph like any other op; it differs only in that its
"lowering" is a direct native call, not a rewrite to other CEIR ops.

### 2.1 The `IntrinsicDesc` schema (§100), reusing the RAF `ExecutorRegistry` precedent
Registered into an `IntrinsicRegistry` (in `crd-ceir`, mirroring `crd::renderpass::ExecutorRegistry`: stable hashed
id, binary-searched lookup, `register_intrinsic` rejects duplicates — no central enum, §7). Fields:

| Field | Meaning |
|---|---|
| `OpId id` + `name` | stable hashed identity (the `ExecutorTypeId`/`OpId` precedent); `name` diagnostic-only |
| `version` | schema version (§104 — old versions handled, not silently broken) |
| typed operands / results | via the CEIR-3 type system (units-tagged where physical, §17) |
| `attributes` | compile-time config (the tagged-union `TypedValue` shape, no `void*`) |
| `Effect[] effects` | §26 effect families + resource/range identity (so the scheduler can order/parallelize it) |
| `EvalDomain domain` | §15 (CompileTime…HostFrameTime…DeviceTime…OfflineTime) + realtime class (§32) |
| `Determinism determinism` | the ADR-0098 tier (BitExact / DeterministicWithinTarget / …Backend / Nondeterministic / ExternalNondeterminism, §27) — *(CEIR-4b reconciled the token to the §27-verbatim `ExternalNondeterminism`; the old short `External` was never used in any TOML)* |
| `bool thread_safe` | may it run off the main thread / concurrently |
| `Lifetime lifetime` | ownership/borrow rules of its resource operands (§19) |
| `Capability[] capabilities` | §99 — declared, host-granted; a sandboxed program without the cap cannot call it |
| `ProviderClass provider` | which bridge implements it (host / gpu / npu / media / external, §69) |
| `CostHint cost` | for the §82 cost model / scheduler |
| `bool hot_reload_safe` | may the program that calls it hot-swap while it is in flight (§108) |
| `SourceMeta debug` | provenance + docs (feeds §111 source maps + §161 agent discovery) |

### 2.2 Where the schema and the handler live (ADR-0109 consistency)
- The **schema registration** (`IntrinsicDesc`) is host-only, in `crd-ceir` — a value type, no backend headers (I4).
- The **handler** (the native function) is registered from a **bridge/provider module** at startup — `present` and
  swapchain from `crd-ceir-gpu`; file/window/OS from `crd-ceir-host`; codec/NPU from their bridges. `crd-ceir` core
  holds the `IntrinsicRegistry` + the abstract handler seam (append-at-END vtable, D135); it links no backend (I3/I4).
- An intrinsic is thus the **op-granular** sibling of `IExecutionProvider` (§69, which is region-granular): both are
  native capability behind an abstract seam registered by a bridge.
- **An intrinsic op's schema lives in its owning DIALECT's TOML** (the CEIR-2 per-dialect generator): `present` in
  the `ceir.io`/`ceir.frame` dialect, a codec op in `ceir.media`. A **Level-C plugin ships its own dialect file** +
  its handler bridge (§101) — no central engine edit, consistent with the open-world model.
- **Landing sequence:** the `IntrinsicDesc` fields depend on CEIR-3 types + CEIR-4's effect/domain/determinism
  vocabulary, so the `IntrinsicRegistry` lands **after CEIR-4** (with CEIR-2's generator gaining the §2.1 fields).
  The first SHIPPED intrinsic is **`present` at CEIR-12** (every frame asset ends in it); the full CEIR-0a atomic
  set (scene resolvers, transfer/present) registers at **CEIR-13c**. This ADR fixes the schema now so those slices
  build to it.

## 3. The legitimacy rule — the one test

> **An intrinsic is legitimate IFF it introduces a capability Cerid did not previously understand — a hardware
> primitive, an OS/device/runtime integration, an external-service/library call, or a provider entry point — that
> CANNOT be composed from existing CEIR ops + CKIR. If it can be so composed, it is an ALGORITHM and MUST be a CEIR
> program asset (§178), never a native intrinsic (§100).**

| Legitimate intrinsics (§100 / §177) | FORBIDDEN as intrinsics (§100 / §178 — these are CEIR programs) |
|---|---|
| swapchain acquire / present; OS window interaction; read a hardware device/sensor | `ForwardPlusExecutor` · `DeferredExecutor` · `MyNewGIExecutor` — renderer architectures |
| external codec call; external SDK / NPU call; vendor compiler invocation | culling / lighting / post-FX / GI / RT workflows |
| filesystem / network syscalls; native plugin callback | compute chains / tensor graphs / ML graphs / media chains |
| a provider entry point (dispatch a region on a device the compiler can't otherwise reach) | UI effect graphs / geometry pipelines |

⛔ **The performance loophole is closed: composable-but-slow is NEVER Level C.** "My hand-written version is 100×
faster, so it must be a native intrinsic" is rejected — **intrinsics are for CAPABILITIES, providers are for
PERFORMANCE.** If an algorithm is expressible but a naive lowering is slow, keep the op semantic and let a
**provider claim the subgraph** (§102 "a provider may accept a maximal subgraph") or select a vendor library
(§52 linalg, §70 "don't flatten a tensor graph before the compiler decides if a device runs it as one native
program"). A fast `GEMM` is a provider choice, not an intrinsic; a `present` is a capability, so it is.

**Applied to the CEIR-0a inventory (resolving its §10 question):** `present` ✅ legitimate (swapchain — host/OS
primitive); media codecs ✅ legitimate (§177 external decode/encode); the scene resolvers
(`resolve_material/technique/program/geometry`) ✅ legitimate as **host data-acquisition** intrinsics (§45's
replaceable convenience tier — a user may still write a different resolver as a CEIR program); `submit_overlay`
◧ **borderline** — the overlay *composite* (LOAD + alpha blend + draw) is an algorithm and should lower to
`ceir.render`. And the underlying `draw_overlay` device verb is NOT a permanent capability either: under §40/§41 a
render scope with loadOp=LOAD + blend dynamic state + read-only depth is ordinary `ceir.render`, so `draw_overlay`
is a **residual special-case verb expected to DISSOLVE into general render ops at CEIR-11** — the `visbuffer.raster`
class (target vs tree-reality, named per the CEIR-0a honesty pattern). Verdict: `submit_overlay` becomes a small
CEIR program over `render.*` ops, and NO new intrinsic is introduced for it — a good early test of the line.

## 4. The three plugin-extension levels (§101) — visible in CR-D007 + docs

| Level | What the extender ships | Native code? | Example |
|---|---|---|---|
| **A — subgraph/function only** | a CEIR function/subgraph asset (a `ceir.func`) composed from existing ops | **none** | a new GI variant, a custom cull, a post-FX chain — the common case |
| **B — custom high-level op + lowering** | a dialect op (CEIR-2 schema) + a rewrite/lowering to existing CEIR/CKIR (§72) | plugin *compiler* code; **no backend change** | a domain op (`myapp.terrain_lod`) that lowers to `ceir.compute` + `ceir.geometry` |
| **C — new native capability / provider** | an intrinsic (this ADR) or an `IExecutionProvider` (§69) | **yes** — only when A/B genuinely cannot express it | a new hardware codec, an NPU backend, a novel device primitive |

The hierarchy is **A → B → C, hardest last** (§101): reach for a native intrinsic only after a subgraph and a
lowering are both proven impossible. This is the mechanism that keeps §178's list (renderers, culling, GI, …) out
of native code by construction — they are Level A/B, never Level C.

## 5. Consequences

**Positive:** the escape hatch exists but is typed, effect-declaring, capability-gated, and verifiable — a native op
is as inspectable/schedulable/replayable as any CEIR op; the legitimacy test + the A→B→C hierarchy make "smuggle an
algorithm into C++" a reviewable violation, not an easy default; the CEIR-0a atomic capabilities have a clean home;
the schema reuses the RAF executor-registry pattern (no new machinery).

**Negative / risk:** the borderline cases (`submit_overlay`, the scene resolvers) need a per-case call — mitigated by
§3's worked verdicts + the test; a lazy extender may reach for Level C to skip authoring a subgraph — mitigated by
making the level explicit in CR-D007 + code review (an intrinsic PR must justify why Level A/B can't express it).

## 6. References

- Mission §100 (intrinsics), §101 (plugin levels), §99 (capability security), §69 (providers), §177 (native-forever),
  §178 (must-not-stay-native), §26/§27/§15 (effects/determinism/domain the schema carries).
- ADR-0108 — owned language stack (capability security parent); ADR-0109 — layer contract + `crd-ceir`/bridge
  placement (where the schema vs handler live).
- ADR-0098 — CKIR determinism tiers (the `determinism` field's vocabulary).
- `docs/design/ceir-0a-execution-path-inventory.md` §8/§10 — the atomic-capability list this ADR classifies.
- `engine/render-pass/src/executor_registry.cpp` — the `ExecutorRegistry` pattern `IntrinsicRegistry` mirrors.
