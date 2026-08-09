# CEIR-4 — Effect + determinism model (§26/§27/§15/§32) — session log

Band 4 promotes the frame graph's proven read/write/lifetime discipline (the WAR-needs-lifetime, RMW-not-RWM scars) to
first-class IR: every effectful op declares semantic effects, determinism classes align 1:1 with the ADR-0098 tiers, and
the compiler distinguishes reorderable vs ordered ops. This log covers the band slice by slice.

## CEIR-4a — the §26 effect vocabulary + typed effect records (declared via the 2a schema)

**Scope (advisor fork settled).** §26 only (determinism §27 → 4b, domains §15/§32 → 4c, hazards → 4d). Deliverable = the
typed effect vocabulary + records + the query surface + the 2a-schema wiring. The effect *check* over a whole module is
CEIR-4d; 4a delivers the declaration + `op_effects` query the compiler reads.

**Design.**
- **`EffectFamily`** (`effect.hpp`) — all **27** §26 families verbatim, `u8`, ⛔ APPEND-AT-END (the ordinal is the enum
  value the generator emits + the `.ops.json` mirrors). NO subsetting (NO-FOLLOW): `MemoryReadWrite` stays its own family.
  A `static_assert(u8(kLastEffectFamily)==26)` pins the C++ side; `test_opgen.py` asserts `len(EFFECT_FAMILIES)==27` +
  first/last — an append to ONE language alone is caught (a C++-only append silently makes a family undeclarable from TOML).
- **`EffectRecord`** POD `{EffectFamily family; EffectTarget target(None/Operand/Result); u32 index; u32 range_mask}` —
  no `StringView`, so generated `constexpr` arrays AND the `register_op` arena copy are both cheap (the compiler reads
  this on every reorder/hazard query). `range_mask` REUSES the CEIR-3c `ViewRange` bitmask (byte/element/mip/layer/aspect;
  0 = whole resource). **Kind-level** declaration — an effect names an operand/result POSITION; the per-instance SSA
  resource is CEIR-4d. Instance-*dependent* effects (a `func.call` whose effects depend on the callee) are NOT
  representable in a static span — they get an `EffectsFn` hook when the producer path exists (CEIR-5); the static span
  does not preclude it, and it is NOT built now.

**Attach point — B1 (advisor, firmly).** §26 is "central, not optional" — core-consumed semantics live on the core
registration record. Effects went on **`OpInfo`** beside `traits`/`verify`; `register_op` gained a defaulted
`effects={}` param (every existing call site still compiles) that **arena-copies** the records (the alloc-outlives-
borrowers rule — a hand caller's stack array must not dangle); queried via **`Context::op_effects(OpId)`**. NOT a CEIR-1d
`InterfaceId` (that is for analysis-owned dispatchable behavior attached from outside — effects are static core data the
compiler itself reads) and NOT reflection-only (`OpSchema` is not Context-wired; hand-registered ops have no schema).
`OpSchema.effects` upgraded `StringView[]→EffectRecord[]` (the same records — the "strings until CEIR-4" scaffold retired).

**⛔ EMPTY≠UNKNOWN (the contract, and its landmine).** `op_effects` returning empty is ambiguous between "registered +
declared no effects" (provably effect-free ⇒ typically `Pure`) and "unregistered kind" (unknown dialect, §6.11). They are
distinguished ONLY by `op_info(kind)!=nullptr`; an unregistered kind is **maximally effectful**, never reorderable. This
is written into the `op_effects` header. **The landmine:** any REGISTERED op that forgets to declare gets the empty
default, which reads as *provably* effect-free — the strongest claim, by omission. **`func.call` had exactly this bug**
(hand-registered, no effects) → it would let 4d reorder/DCE a call. Fixed with a conservative `ExternalCall` barrier
(§26 lists names without prose; ExternalCall = "control leaves to code we don't model"). Grepped every hand
`register_op` site — func.cpp is the only production hand path. Memory:
`feedback_registered_default_empty_reads_as_provably_none`.

**Pure coherence.** `Pure` ⇒ **zero** effect records, strictly (a `MemoryRead` op is not CSE-safe across a write, so ANY
effect disqualifies Pure). One direction (an effectless op needn't claim Pure). Enforced at BOTH live arms: the generator
(a pointing cook-time error naming the op) + a `register_op` assert. No decoder arm — registration isn't serialized.

**2a schema (the declaration path).** TOML `effects` accepts a **bare family string** (`"MemoryWrite"`, ambient, no
identity) OR an **inline table** (`{family = "MemoryWrite", operand = 0, range = ["element"]}`) — the `[op.native]`
precedent. The generator validates the family vocabulary + operand/result index against the op's OWN declared counts
(`operand = 7` on a 2-operand op = a pointing error — the validate-at-cook-time scar) + range names, and emits typed
`EffectRecord` arrays **before `register_%s_ops`** (an anon-namespace member is visible across both anon-namespace blocks
in the TU, so the `OpSchema` table reuses the SAME arrays — no duplicate emission). `.ops.json` effects went
`string[]→object[]` under **schema_version 1** deliberately (a scaffold field; no external consumers). `test.ceirop.toml`
now exercises BOTH forms (the full-surface-reference-input scar). Every dialect regenerated (OpSchema layout changed);
`crd-ceir-opgen-drift` verifies byte-identical cross-OS.

**Not serialized.** Effects are registration-time data on `OpInfo`, not module content — no binary surface touched, **no
version bump**.

## Traps + tests

- **`func.call` landmine** (above) — the empty≠unknown contract's own trap; caught by the advisor, not a gate (nothing
  consumes effects yet). Fixed + tested (`op_effects(func.call).size() >= 1`, family ExternalCall).
- **Arena-copy lifetime** — `test_effect.cpp` registers an op from a SCOPE-LOCAL array, lets it die, then reads
  `op_effects` — a real use-after-scope probe under ASan (a non-copying `register_op` would dangle).
- **Cross-language lockstep** — the `static_assert` (C++) + the count/first/last assertions (`test_opgen.py`) pin the 27
  families on both sides. Emission is by NAME so a transposition can't corrupt values, but an append to one side alone is
  silent in exactly one direction; the two assertions close it.
- `test_effect.cpp` (4 cases): round-trip BOTH forms from the 2a schema, empty≠unknown (registered-empty vs unknown),
  arena-copy, func.call barrier. `test_opgen.py` +11: unknown family (string + table), missing family, unknown field,
  operand/result index out of range, operand+result both, unknown range, Pure+effects, a positive both-forms parse, the
  27-family lockstep. The obsolete "effects entries must be strings" test was retired (tables are now legal).

## Gate

crd-ceir-tests **98/98 ctest** (820 assertions) on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** —
Windows run via `ctest` (not binary-direct: the invariants + opgen ctests are Windows-covered too) — + LLVM-20 tidy + GCC
`-Werror=switch` + `crd-ceir-opgen-{drift,validator}` (36 py tests) + `crd-ceir-invariants` green both OSes. New surface:
`EffectFamily`/`EffectTarget`/`EffectRecord`/`kLastEffectFamily` (`effect.hpp`), `OpInfo::effects`, the extended
`register_op`, `Context::op_effects`, `OpSchema.effects` (typed). **No binary version bump.** Next = CEIR-4b
(determinism classes → ADR-0098 tiers; compiler modes; numerical-semantics attrs, §27/§28).

## Proposed commit — CEIR-4a (user commits; NO AI trailer)

```
feat(ceir-4a): the §26 effect vocabulary + typed effect records on OpInfo

- effect.hpp: EffectFamily (27 §26 families, u8, append-at-end, static_assert
  pins Debug==26) + EffectRecord POD {family, target(None/Operand/Result), index,
  range_mask}; range_mask reuses the 3c ViewRange bitmask (0 = whole resource).
- OpInfo/register_op: effects live on OpInfo (attach point B1 -- core data the
  compiler reads, not a 1d interface, not reflection-only); register_op(op, traits,
  verify, effects={}) arena-COPIES the records; Context::op_effects(OpId) queries.
- EMPTY != UNKNOWN contract (header + tested): an empty span is "provably
  effect-free" ONLY when op_info != nullptr; an unregistered kind is maximally
  effectful. func.call now declares a conservative ExternalCall barrier (a
  registered op that forgot to declare would read as provably effect-free).
- Pure => zero effects at both live arms (generator cook error + register_op
  assert); register_op also bounds family<=Debug && target<=Result.
- generator (2a schema): effects accept a bare family string OR an inline table
  {family, operand|result, range=[...]}; validates the family vocabulary + index
  bounds + range names (pointing errors); emits typed EffectRecord arrays;
  OpSchema.effects StringView[] -> EffectRecord[]; .ops.json string[] -> object[]
  under schema_version 1 (scaffold field, no external consumers). All dialects
  regenerated; test.ceirop.toml exercises both forms.
- tests: test_effect.cpp (round-trip both forms, empty!=unknown, arena-copy under
  ASan, func.call barrier); test_opgen.py +11 (family/index/range/Pure rejections
  + 27-family cross-language lockstep).

Gated: crd-ceir-tests 98/98 ctest (820 assertions) on win-debug + win-asan +
linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen
drift/validator (36 py) + invariants green both OSes. No binary version bump.
```

## CEIR-4b — §27 determinism classes + compiler modes + §28 numerical semantics

**Two axes, two granularities (advisor).** A per-op-KIND **`DeterminismClass`** (a semantic contract — how reproducible
is this op?) carried on `OpInfo` beside its effects, declared via the 2a schema; and a per-op-INSTANCE
**`NumericalSemantics`** (the §28 IEEE/FMA/… knobs — the SAME add can be strict here, fast there) carried as ONE packed
`numerics` int ATTRIBUTE. The mode↔class and mode↔numerics legality predicates are the enforcement primitive a pass will
call; no pass manager until CEIR-6, so 4b ships the predicates + a module walk (the 4a "predicate-now" precedent).

**§27 determinism.**
- **`DeterminismClass`** = the 5 §27 tiers + **`Unspecified = 0`** (the default). ⛔ Unspecified is NOT Nondeterministic —
  absence of a claim ≠ a claim (the 4a func.call landmine); the legality predicate treats it conservatively (it satisfies
  only Normal/Fast). Attach point **B1** again: on `OpInfo`, `register_op(…, det)`, `Context::op_determinism`, same
  EMPTY≠UNKNOWN header discipline (an unregistered kind is Unspecified-but-UNKNOWN; check `op_info` first).
- **Naming reconciled:** the generator said `External`; §27 says `ExternalNondeterminism`. Renamed to the verbatim token
  (no TOML used the short form), `DETERMINISM_TIERS` set→ordered tuple (ordinal lockstep), ADR-0110 §2.1 struck in-place.
  `static_assert(u8(kLastDeterminismClass)==5)` + a py count/first/last assertion pin both languages.
- **Two levels (§27 "operation/provider").** The op-level class is the abstract contract; `[op.native].determinism` is a
  specific provider's claim; both stay. A **cook-time consistency** check requires native ≥ op by a documented rank
  (BitExact>WithinTarget>WithinBackend>Nondeterministic≥ExternalNondeterminism — the last `≥` is a judgment call: External
  is a *narrower* source of nondeterminism, so it ranks just below plain Nondeterministic). `OpSchema.native_determinism`
  typed `StringView→DeterminismClass`.
- **`CompilerMode`** (`Normal`/`Fast`/`Deterministic`/`CertifiedDeterministic`, default Normal) is **session state, never
  serialized** — proven by a content-purity assertion (setting the mode does not change a module blob). Legality map
  (documented, since §27 doesn't define it): Certified→BitExact only; Deterministic→the three deterministic tiers;
  Normal/Fast→everything. Tested as the **full 6×4 matrix** (one case per cell — a single case can't tell a real predicate
  from a constant).

**§28 numerical semantics.** `NumericalSemantics` = all 12 §28 lines verbatim (`ieee`, `fast_math`, `fma`,
`flush_to_zero`, `denorm`, `rounding`, `overflow`, `int_wrap`, `nan`, `precision_promote`, `mixed_precision`,
`stochastic_round`), each field's 0 = Inherit so a partial attr composes. Per-INSTANCE ⇒ it rides ONE packed `numerics`
int attribute (12 nibbles → i64, one `pack`/`unpack` pair — the 3e move). ⛔ `unpack_numerics` **validates bounds** (a
nibble past its field's value count, or any bit ≥ 48, is rejected without touching the out param) — the binary layer
stores an opaque i64, so this IS the decoder arm. `numerics` is now RESERVED attr vocabulary. `numerics_satisfies_mode`
honors the **fmad scar**: Fast/Normal admit everything (Fast MUST admit FMA/fast-math — bit-exact flags cripple GEMM),
Certified/Deterministic forbid fast-math + Relaxed-IEEE but ALLOW FMA (contraction is deterministic when applied
consistently — a bit-exact GEMM/FFT wants it).

**Enforcement — `find_mode_violation(Module&)` (renamed from the determinism-only draft, advisor).** Pre-order walk,
first-offender, checking the WHOLE active contract: the op's determinism class (§27) OR its per-instance numerics (§28) OR
a **corrupt `numerics` attr — a violation in EVERY mode, Normal included** (malformed data is not a legal-knob question).
The determinism-only draft was blind to §28; caught at pre-close. **Kind-vs-instance is NOT cross-checked here** — a
`BitExact` kind whose instance sets `fast_math=On` is effectively degraded, but reconciling the two is an op-verifier
concern (CEIR-6-ish); 4b keeps them independent axes.

## Traps + tests

- **ASCII-only test names (a known scar, re-hit).** A TEST_CASE named "…the §28 numerics contract" *passed* when the whole
  `[semantics]` tag ran in one process but *failed under ctest* — `catch_discover_tests` registers the name with the `§`,
  and ctest cannot invoke a non-ASCII name (encoding mismatch). Renamed to "(sec 28)". The full 4-config **ctest** gate
  (not binary-direct) is what surfaced it — the binary-direct-misses-ctest scar, again.
- **The unregistered-container test failure** (my `scf.region` holder was itself Unspecified, so it was flagged first) is
  the 4a EMPTY≠UNKNOWN discipline working as designed — fixed by using a registered BitExact region-holder. No new memory.
- `test_semantics.cpp` (8 cases): op_determinism round-trip + empty≠unknown, the 6×4 matrix, `find_mode_violation` for §27
  and §28 (+ corrupt-attr under Normal), mode-not-serialized, numerics pack/unpack (every field + out-of-range reject),
  numerics attr survival through TEXT and BINARY, fmad-scar legality. `test_opgen.py` +4 (unknown determinism, native
  weaker-than-op reject, native-stronger OK, the 5-tier lockstep).

## Gate

crd-ceir-tests **106/106 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** (both OSes via `ctest`) +
LLVM-20 tidy + GCC `-Werror=switch` (the `determinism_rank` + `determinism_satisfies_mode` switches are complete) +
`crd-ceir-opgen-{drift,validator}` (40 py tests) + invariants green. **No binary version bump.** New surface:
`semantics.hpp` (DeterminismClass, CompilerMode, NumericalSemantics + its field enums, `pack/unpack_numerics`,
`determinism_rank`, `determinism_satisfies_mode`, `numerics_satisfies_mode`), `OpInfo::determinism`, the extended
`register_op`, `Context::op_determinism`/`set_compiler_mode`/`compiler_mode`/`find_mode_violation`/`set_numerics`/`op_numerics`,
`OpSchema` typed determinism fields. Next = CEIR-4c (evaluation-domain + realtime-class metadata + the domain-legality
verifier, §15/§32).

## Proposed commit — CEIR-4b (user commits; NO AI trailer)

```
feat(ceir-4b): §27 determinism classes + compiler modes + §28 numerics

- semantics.hpp: DeterminismClass (5 §27 tiers + Unspecified=0 default,
  static_assert-pinned) + CompilerMode (Normal/Fast/Deterministic/Certified) +
  determinism_satisfies_mode (Certified->BitExact, Deterministic->3 tiers,
  Normal/Fast->all) + determinism_rank; NumericalSemantics (all 12 §28 knobs,
  0=Inherit) packed into one i64 via pack/unpack (unpack validates bounds) +
  numerics_satisfies_mode (Fast admits FMA/fast-math -- the fmad scar).
- OpInfo/register_op: a per-op-KIND DeterminismClass (register_op param 5);
  Context::op_determinism, same EMPTY!=UNKNOWN discipline as effects.
- Context: set_compiler_mode/compiler_mode (session state, NOT serialized);
  find_mode_violation(Module&) -> first op violating the active mode (determinism
  OR numerics OR a corrupt numerics attr, which violates every mode);
  set_numerics/op_numerics (the per-INSTANCE `numerics` int attribute).
- generator (2a schema): a top-level `determinism` field (validated against the
  §27 vocab) emitted onto register_op + OpSchema; native_determinism typed
  StringView->DeterminismClass; DETERMINISM_TIERS set->ordered tuple, External->
  ExternalNondeterminism; cook-time native>=op consistency check. arith int ops
  declare BitExact; test.dummy carries an op-level class weaker than its native.
- docs: ADR-0110 §2.1 token reconciled in-place.
- tests: test_semantics.cpp (op_determinism, 6x4 matrix, find_mode_violation for
  §27+§28+corrupt, mode-not-serialized, numerics pack/unpack/reject + text/binary
  survival, fmad legality); test_opgen.py +4 (determinism rejections + lockstep).

Gated: crd-ceir-tests 106/106 ctest on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator (40 py)
+ invariants green both OSes. No binary version bump.
```

## CEIR-4c — §15 evaluation domains + §32 realtime classes + the domain-legality verifier

**register_op → OpSpec (the deferred meta-decision, settled at 4c open).** register_op's positional param list hit the
threshold (four defaulted semantic params + domain landing); consolidated into `OpSpec{traits, verify, effects,
determinism, domain}`, one signature `register_op(op, spec={})`, C++20 designated initializers (`NumericalSemantics`'s
`==` already proved the standard is on). Migrated EVERY site this slice (generated regen + func.cpp×3 + test_effect /
test_semantics / test_builder). `spec.effects` is still arena-COPIED — the spec is a temporary. One churn, not two (the
advisor's reason to do it now rather than at 4d).

**§15 EvalDomain (per-op-KIND).** `Unspecified=0` + the 10 §15 domains verbatim (static_assert-pinned to the generator's
`EVAL_DOMAINS`), on `OpInfo` via the spec, declared through the 2a `domain` field, queried by `op_domain`, same
EMPTY≠UNKNOWN discipline. `OpSchema.domain` StringView→EvalDomain (the 3rd scaffold retirement); the `"host"` placeholder
was not even a §15 value → real domains (arith `EitherHostOrDevice`, test `HostFrameTime`). ⛔ The 4c walk does NOT yet
consume kind-domain — kind×region placement checking is an op-verifier concern (CEIR-6-ish); the §15 partial-evaluation
use is later still. Row says so, or it overclaims.

**§32 RealtimeClass + the region tag.** `Unspecified` + the 7 §32 classes verbatim (static_assert-pinned). Deliberately
**NOT a 2a-schema field** — it is a REGION property, never per-op-kind, so there is NO generator vocabulary (said in-code,
so nobody adds a tuple nothing consumes). `Offline` (a deadline class) and `OfflineTime` (an eval domain) are DISTINCT
axes. A region's (domain + realtime) rides ONE packed `region_exec` int ATTRIBUTE on the region-OWNING op (regions have no
attr dict; rides the existing attr machinery — no new serialized surface, no version bump; `region_exec` now RESERVED
vocab; pack/unpack validated, the numerics move). **Symmetry with 4b:** the compiler mode is session state (never
serialized); the region tag IS module content — it survives print/parse AND serialize/deserialize, and the verifier
RE-FIRES post-round-trip (tested both forms end-to-end, not just tag survival).

**The verifier `find_domain_violation(Module&) → DomainViolation{op, region_owner, effect, unknown_kind}`.** Pre-order,
INNERMOST-tag-wins (an owner's tag replaces its parent's for its subtree). Contract corners, each a sentence in-code:
- module body is untagged / unconstrained;
- an ABSENT `region_exec` INHERITS the enclosing tag AND its owner (the violation names the op that SET the constraint,
  not the nearest container — tested through an untagged intermediate);
- a PRESENT-empty `{}` is an explicit unconstrain OVERRIDE (distinct from absent — tested);
- whether a nested region may RELAX its parent's class is execution semantics → deferred CEIR-5+;
- a CORRUPT `region_exec` = a violation at the owner (`op==region_owner` the only discriminator; `effect` filler);
- the unknown-op check uses `effect_legal_in_region(FileIO, tag)` — FileIO stands PROXY for "is this tag constraining";
  generalize to any-forbidden-family when the seeded table grows.
Seeded §32 rule (the extensible table function): `FileIO`/`NetworkIO` illegal where `realtime==AudioRealTime` OR
`domain∈{DeviceTime,HostAudioTime}`. `ExternalCall` stays legal-for-now (CEIR-5's EffectsFn refines calls). Separate from
`find_mode_violation` (mode=session vs tag=content); CEIR-4z composes them. (ADR-0110's IntrinsicDesc effect/domain/
determinism bundling is future — no edit needed.)

**The vacuous-pass trap (advisor headline).** A walk iterating DECLARED effects passes an UNREGISTERED op by vacuity —
but unknown = maximally effectful = potential FileIO, so an unregistered op in an audio region must be FLAGGED
(`unknown_kind`), while a registered effect-free op (arith.addi) in an audio region is LEGAL. That pair is empty≠unknown
one level up (the func.call landmine again); tested both halves.

## Traps + tests

- **Two blocking test gaps caught at pre-close:** (1) the round-trip test asserted only TAG survival while its comment
  claimed a verifier re-fire — fixed to build a real io.read in the tagged region and re-run `find_domain_violation` on
  the reparsed AND deserialized modules (`unknown_kind==false` proves the FNV-1a OpId of "io.read" rebinds to the fresh
  ctx's FileIO effect); (2) inheritance through an UNTAGGED intermediate was untested — added, asserting `region_owner ==`
  the OUTER audio owner. Both were "the row would claim what the test doesn't prove."
- **Two existing-discipline catches (no new memory):** `OpSchema.domain` type change was missed until the build failed
  (typed generator emission vs stale StringView field); `semantics.hpp` failed standalone-TU tidy because it used
  `EffectFamily` without including `effect.hpp` (transitively satisfied in the real build, but the per-file tidy checks
  each header standalone) — added the include. Both are the header-hygiene + validate-at-cook discipline, not surprises.
- `test_domain.cpp` (9 cases, 51 assertions) + `test_opgen.py` +3 (unknown domain, 10-tuple lockstep, `"gpu"`→DeviceTime).

## Gate

crd-ceir-tests **115/115 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** (both OSes via `ctest`) +
LLVM-20 tidy + GCC `-Werror=switch` + `crd-ceir-opgen-{drift,validator}` (43 py) + invariants green. **No binary version
bump.** New surface: `EvalDomain`/`RealtimeClass`/`RegionExec` + `pack/unpack_region_exec` + `effect_legal_in_region`
(semantics.hpp), `OpSpec` + `OpInfo::domain` (dialect.hpp), the consolidated `register_op`, `Context::op_domain`/
`set_region_exec`/`op_region_exec`/`find_domain_violation`, `struct DomainViolation`, `OpSchema.domain` typed. Next =
CEIR-4d (hazard foundations: effect-derived ordering constraints between ops over the same range — the analysis
CEIR-12d's scheduler consumes, §26/§116).

## Proposed commit — CEIR-4c (user commits; NO AI trailer)

```
feat(ceir-4c): §15 eval domains + §32 realtime classes + domain-legality verifier

- register_op consolidated into OpSpec{traits, verify, effects, determinism,
  domain} (one signature, designated initializers); migrated every call site
  (generated regen + func.cpp + 3 test files); spec.effects still arena-copied.
- semantics.hpp: EvalDomain (10 §15 domains + Unspecified, static_assert-pinned) +
  RealtimeClass (7 §32 classes + Unspecified; NOT a schema field -> no generator
  vocab); RegionExec (domain+realtime) packed via pack/unpack_region_exec (validated);
  effect_legal_in_region (FileIO/NetworkIO illegal in audio-RT / DeviceTime /
  HostAudioTime; the extensible table).
- OpInfo/register_op: per-op-KIND EvalDomain via the spec; Context::op_domain
  (empty!=unknown). OpSchema.domain StringView->EvalDomain.
- Context: set_region_exec/op_region_exec (a packed `region_exec` int attr on the
  region-owning op -- module content, survives round-trip); find_domain_violation
  (Module&) -> DomainViolation{op, region_owner, effect, unknown_kind}, innermost-
  tag-wins, an UNREGISTERED op flagged conservatively, a corrupt tag flagged at the
  owner. Separate from find_mode_violation (mode=session vs tag=content).
- generator (2a schema): op-level `domain` validated against the §15 vocab (EVAL_
  DOMAINS 10-tuple), emitted onto the OpSpec + OpSchema. arith EitherHostOrDevice;
  test.dummy HostFrameTime.
- tests: test_domain.cpp (op_domain, region_exec pack/reject, legality matrix, tag
  survival + verifier re-fire through text AND binary, forbidden-vs-effect-free,
  unregistered-flagged-vs-registered-legal, nested inherit-through-untagged +
  innermost-override + present-empty-unconstrain, corrupt-tag); test_opgen.py +3.

Gated: crd-ceir-tests 115/115 ctest on win-debug + win-asan + linux-gcc-debug +
linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch + opgen drift/validator (43 py)
+ invariants green both OSes. No binary version bump.
```

## CEIR-4d — effect-derived ordering hazards (§26/§116)

**A SCHEMA-QUIET slice.** Hazards are DERIVED from the 4a EffectRecords — no TOML, no generator change, no regen, no new
py tests, no version bump. First of the band; stated so nobody hunts for a missing regen (drift stays green with no
regen — correct here).

**The model.** `hazard.hpp`: `HazardKind{None,War,Raw,Waw}`; `ResourceClass` (14 disjoint classes + `Universe`
overlaps-all + `None` inert); `effect_access(EffectFamily)→{reads,writes,ResourceClass}`, a TOTAL switch over all 27
families with ⛔ NO default (a 28th family is a `-Werror=switch` compile error — the append guard, free). Two ops HAZARD
iff they share a resource (+ overlapping range) with ≥1 write; the kind is by program order (WAW>RAW>WAR, documented
convention). `Context::ops_hazard(before, after)` (pure, directional) + `collect_block_hazards(Block&, Array<Hazard>&)`
(the O(n²) all-pairs correctness REFERENCE over one block in authored-linearization order — 12d builds the indexed
version on the same primitive; no transitive reduction here).

**The judgment calls (fork 2, the crux) — all in-code + here:**
- Memory class holds **Alloc/Dealloc/ResourceResidency (write)** — lifecycle in Memory so `Deallocate(R)`-then-`read(R)`
  use-after-free is VISIBLE; a separate Alloc class would silently miss it.
- **I/O = ONE `Io` class (rw)** — cross-channel reorder conservatively forbidden until a channel model exists.
- **ExternalCall + Synchronization = `Universe` (rw)** — a full barrier (consistent with the 4a func.call barrier).
- ⛔ **`RandomRead` WRITES `Random`** — a PRNG draw advances the stream; the naive "read means read" makes RNG draws
  freely reorderable, silently destroying replay determinism (the product feature). Two draws MUST order.
- **`TimeRead` a pure inert read** — no writer of Time exists; replay-ordering of clock reads is the recorder's concern
  (CEIR-5+). The split matters: Random advances, Time doesn't.
- `Nondeterministic` inert (4b's DeterminismClass is the real channel); Logging/Debug write their own class (stable
  interleave, no conflict with compute).

**Resource identity + the alias contract.** `(class, Value*|null)`: an operand/result `EffectTarget` resolves to the SSA
`Value*`; ambient (`target None`) → `null` = whole class; overlap = Universe-either ∨ (same class ∧ (same Value ∨ either
null)). ⛔ **DISTINCT SSA Values are assumed NON-ALIASING** — vacuously safe today (no view-creation op exists), but a
CEIR-6+ alias model must refine it or 12d inherits an invisible unsoundness (named in the row + header). An out-of-range
index on a malformed instance degrades to whole-class — the conservative direction (more hazards, never fewer).

**Reports everything.** An effect-derived RAW that coincides with an SSA def-use edge is NOT filtered — the primitive's
contract is "is there an effect-derived ordering constraint"; a scheduler union-ing free SSA edges dedups trivially.
EMPTY≠UNKNOWN again: an unregistered op = Universe rw (hazards anything); a registered Pure (zero-effect) op = `None` vs
anything (pure computation touches nothing → correctly reorderable — the payoff pair for 4z).

## Traps + tests

- **The advisor caught two untested contracts at pre-close** (same class as 4c's round-trip miss): the
  `EffectTarget::Result` resolution branch had zero coverage, and the lifecycle-in-Memory ordering was only
  classifier-spot-checked. Added one case: a producer that WRITES its result → a reader of it reports RAW (Result branch
  + report-everything), and `Deallocate` vs a same-buffer read BOTH ways (`read→free = WAR` — the frame-graph
  WAR-needs-lifetime scar as a literal one-liner, the seed for 4z; `free→read = RAW` — the UAF is visible).
- Enabling change: `Operation::result(u32)` loosened to `const` (mirrors `operand`; non-breaking, 4 configs prove it).
- `bugprone-branch-clone` on the identical consecutive classifier arms (Alloc/Dealloc/Residency; File/Network/Device) →
  merged into shared case labels (the 3d shared-label pattern). ASCII test names ("sec 116", no `§`).
- `test_hazard.cpp` (7 cases, 46 assertions). No schema tests — nothing to validate at cook time this slice.

## Gate

crd-ceir-tests **122/122 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** (both OSes via `ctest`) +
LLVM-20 tidy + GCC `-Werror=switch` (the classifier's no-default total switch) + `crd-ceir-opgen-{drift,validator}` (43
py, drift green with NO regen) + invariants green. **No binary version bump.** New surface: `hazard.hpp` (HazardKind,
ResourceClass, EffectAccess, `effect_access`, `range_overlap`, `hazard_rank`), `struct Hazard`, `Context::ops_hazard`/
`collect_block_hazards`, `Operation::result const`. Next = CEIR-4z (BAND-4 GATE): compose `find_mode_violation` +
`find_domain_violation` + `ops_hazard`/`collect_block_hazards` over one curated effect-pair module — every pair classified
reorderable/ordered, the same-range WAR scar as the named test (§26/§168) — then BAND 4 CLOSED → CEIR-5.

## Proposed commit — CEIR-4d (user commits; NO AI trailer)

```
feat(ceir-4d): effect-derived ordering hazards (RAW/WAR/WAW) from 4a effects

- hazard.hpp: HazardKind + ResourceClass (14 classes + Universe + None) +
  effect_access(EffectFamily) -> {reads, writes, ResourceClass}, a TOTAL switch
  over 27 families with NO default (-Werror=switch guards a 28th). Judgment calls:
  Alloc/Dealloc/Residency in Memory (use-after-free visible); I/O one rw class;
  ExternalCall+Synchronization = Universe barrier; RandomRead WRITES (advances the
  stream); TimeRead inert read; Nondeterministic inert. range_overlap + hazard_rank.
- Context::ops_hazard(before, after) -> strongest hazard (WAW>RAW>WAR) over the two
  ops' effect-pairs sharing a resource (class, SSA Value|null-whole-class) +
  overlapping range with >=1 write; distinct Values assumed non-aliasing; reports
  everything (SSA-subsumed edges not filtered); unknown op = Universe rw, Pure = None.
- Context::collect_block_hazards(Block&, Array<Hazard>&) -- O(n^2) all-pairs
  reference over one block in list order; no transitive reduction (scheduler's job).
- ir.hpp: Operation::result(u32) loosened to const (mirrors operand).
- tests: test_hazard.cpp (classifier incl RandomRead-writes, the R/W quartet, range
  + non-aliasing, ambient families, barrier/unknown/Pure, precedence, Result-targeted
  RAW + the Deallocate WAR/UAF lifetime scar, collect_block_hazards edge list).

Schema-quiet: no TOML/generator/regen/py change. Gated: crd-ceir-tests 122/122
ctest on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy +
GCC -Werror=switch + opgen drift/validator (43 py) + invariants. No version bump.
```

## CEIR-4z — the BAND-4 GATE (§26/§168) → BAND 4 CLOSED

**The band's own exit criterion, met verbatim.** The CEIR-4 band contract's gate line is *"the compiler distinguishes
reorderable vs ordered ops correctly."* 4z closes the loop on it over ONE curated module composing all three axes.

**Test-only, like 3z.** Reuses `find_mode_violation` + `find_domain_violation` + `ops_hazard`/`collect_block_hazards`; a
unified `verify` entry stays UNBUILT until a consumer exists (CEIR-6 passes / 12d) — the three predicates have different
lifetimes (mode = session, tag = content, hazards = pairwise-derived) and one entry would blur them. The 4c header comment
"CEIR-4z composes them" was over-readable (the 4a "effect interface" pattern) — corrected in-place to "the 4z GATE TEST
composes them."

**The curated module (staged, per advisor).**
- Baseline block: Certified-clean + domain-clean but hazard-rich (`buf`(Pure producer)/`write`/`read`/`write2`/`rng`/
  `rng2`/`log`). `collect_block_hazards` asserts the EXACT 4-edge forward matrix + a reverse-direction sweep — the row's
  "every pair classified correctly" without a hand-written N×N (every `None` pair pinned by absence + the `REQUIRE`d size).
- ⛔ **The BitExact-baseline trap** (empty≠unknown's 5th appearance): a gate dialect with DEFAULT (Unspecified)
  determinism would make Certified flag the first op, and the mode-axis assertions would test nothing. So EVERY gate op is
  `BitExact` (a seeded PRNG is bit-exact — even `rng`), and `func.call`/unknown ops are kept OUT of the main block.
- **Orthogonality is the composed claim, asserted by FULL edge-list IDENTITY** (endpoints + kind, not just count — the
  advisor caught a size-only check that would pass on a flipped edge): (a) a per-instance `fast_math` numerics knob trips
  Certified → `find_mode_violation` points at that op, hazards identical; (b) an appended Unspecified op trips the
  DETERMINISM half of the mode walk (both sub-axes now fire), hazards identical; (c) a side region tagged audio-RT with a
  `FileIO` op fires `find_domain_violation`, main-block hazards identical.
- Unknown-op-is-a-barrier vs Pure-op-is-reorderable proven in a SIDE block (Universe edges would spray the exact list).

**The WAR-needs-lifetime scar as the named centerpiece.** `read(R)` then `write(R)` over one buffer → WAR **purely from
effects + SSA resource identity**, no declaration-order metadata anywhere, with the different-buffer contrast (→ `None`) so
the test reads as the scar's proof.

**The round-trip Value-identity claim (nothing had fired it).** serialize→deserialize the curated module, re-register the
dialect, re-`collect_block_hazards` → same edge kinds. This proves the shared-operand `Value` identity reconstitutes
through the parser's operand FIXUP pass — effect *re-binding* was 4c; Value-identity-through-fixup is the new composed
claim a band gate is the natural home for.

## Gate

crd-ceir-tests **127/127 ctest** on **win-debug · win-asan · linux-gcc-debug · linux-gcc-asan** (both OSes via `ctest`) +
LLVM-20 tidy + GCC `-Werror=switch` + `crd-ceir-opgen-{drift,validator}` (43 py) + invariants green. **→ BAND 4 CLOSED
(4a–4z).** No new production surface (test-only). Next = CEIR-5 (structured control flow + functions + the FIRST reference
executor — a gear change: region-carrying `ceir.core` ops §13/§14, SSACFG beneath, explicit state §20, the reference
executor §118; accumulated landings to place — the `EffectsFn` hook for func.call, cross-block hazards, nested-region-relax).

## Proposed commit — CEIR-4z (user commits; NO AI trailer)

```
test(ceir-4z): BAND-4 GATE - compose mode/domain/hazard; the WAR-lifetime scar

- tests/ceir/test_band4_gate.cpp [gate4] (5 cases): the band-4 exit criterion
  ("distinguishes reorderable vs ordered ops correctly") over ONE curated module.
  * the WAR-needs-lifetime scar: read(R)-then-write(R) = WAR from effects + SSA
    resource identity alone (no decl-order metadata); different buffer = None.
  * a curated baseline: exact collect_block_hazards edge list (W->R RAW, W->W WAW,
    R->W WAR, rng->rng2 WAW) + reverse-direction sweep; mode-clean under Certified +
    domain-clean. Every gate op is BitExact so the mode axis is a real test (the
    Unspecified-default trap).
  * orthogonality: numerics-trips-Certified, Unspecified-op-trips-determinism, and
    a side audio-RT+FileIO region trips the domain walk -- each leaving the FULL
    hazard edge list (endpoints + kind) identical.
  * unknown-op barrier vs Pure-op reorderable in a side block.
  * hazards survive serialize/deserialize (shared-operand Value identity through the
    parser fixup pass).
- context.hpp: the "CEIR-4z composes them" comment corrected to name the gate test
  (a unified verify entry stays unbuilt until CEIR-6/12d).

Test-only, no production surface. Gated: crd-ceir-tests 127/127 ctest on win-debug
+ win-asan + linux-gcc-debug + linux-gcc-asan + LLVM-20 tidy + GCC -Werror=switch +
opgen drift/validator (43 py) + invariants. -> BAND 4 CLOSED (4a-4z).
```
