# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them. (History pruned 2026-08-07 → `docs/sessions/2026-08-07-context-md-history-archive.md`.)

---

## Current focus — **[2026-08-14 ✅ DONE + 4-config gated] ⭐⭐⭐ CEIR-16 §128 EXECUTOR MIGRATION COMPLETE.** Every **composite** raster executor now records through the ONE generic `record_ceir_render` (replaying a per-pass CEIR plan built at frame LOAD via `build_frame_plans`→`build_*_ceir`); every imperative recorder is DELETED (the-deletion-is-the-proof). scene.raster (16d: the `render.scene_draw_list` template + verb ladder + mrt≥2 op-mode + the fs_target 3-mode depth resolver) + fullscreen/mesh/tess/mesh.indirect (16b) + **visbuffer.raster (16z — DISSOLVED into scene.raster** as a DECLARED `geometry=procedural` role + a uint typed clear derived from the R32Uint target format). `record_scene_raster` / `record_visbuffer_raster` + all scar helpers + the `visbuffer.raster` executor DELETED — **14 built-in executors → 13.** GATE: render-pass 46/5 + render-graph 309/15 + gpu [raf7] 363/2 (Vk+DX12) + frame-cook 2751/96 + scene-render 1539/72 (incl. the DEVICE visbuffer id read-back) + `crd-sandbox --smoke-test 2` PASS + gpu-context vulkan/dx12 REN-38-A11 visbuffer cook+device gates; win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean. Detail → `docs/sessions/2026-08-14-ceir-16d-scene-raster-migration.md` + `docs/design/ceir-16-executor-migration.md` STATUS + `docs/detours/D-007-ceir-tracker.md` (CEIR-16 ✅). ⛔ **NEXT = CEIR-17** (master spine `project_ceir_master_spine_locked`; expand its sub-slices explicitly in the tracker at band-open, sized by CEIR-0z). ⚠ all CEIR-16 work UNCOMMITTED (user commits; a fresh session `git status` first).

**Prior (this band):** the CEIR-16 sub-slice journey (16a design → 16b render family → 16c item-view → 16d scene.raster live path incl. the flag dance + mrt≥2 + the-deletion-is-the-proof → 16z visbuffer dissolution) is recorded slice-by-slice in `docs/design/ceir-16-executor-migration.md` STATUS blocks. Every render pass now flows through authored frame graphs → cooked `.crdr` → CEIR. Prior CEIR bands (1–15, the CEIR substrate) + the archived band history are below.

> **CEIR-2 ✅ BAND CLOSED (op-definition generator, §8).** Ops DEFINED in `engine/ceir/ops/*.ceirop.toml` →
> `tools/ceir_opgen/ceir_opgen.py` (stdlib) emits committed C++ (self-registers via the CEIR-1d registry) +
> `.ops.{json,md}` + a gated smoke test — **an op is a TOML edit + regen, ZERO central-enum/switch edits** (proven
> end-to-end by adding the full-surface `test` dialect at 2z). Detail → `docs/sessions/2026-08-08-ceir-2-opgen.md`.
>
> **CEIR-3a ✅ (interned structural types, §16) 2026-08-08.** `TypeId` is now a real interned STRUCTURAL type (scalars +
> the §16 aggregate family): `type.hpp` (`TypeKind`/`FloatKind`/`Type`; **`kind` a FIELD** for I6), `Context::type_*` +
> `type_of` (**asserts, no silent fallback**), a canonical `!`-sigil grammar (`!vec<4x!f32>`, `!struct<P,x:!i32,y:!f32>`,
> `!option<!result<…>>`, …) printer+parser byte-exact, and binary **v2** with a **child-first `TYPE` chunk** (content-pure;
> per-result/arg refs so no future v3 bump). Every `TypeId{n}` magic literal migrated to real factories (a stray one
> aliased to incidental intern state — caught only by the dirty-context purity test). 65/65 × 4 configs + tidy +
> invariants. Detail → `docs/sessions/2026-08-08-ceir-3-types.md`.
> **CEIR-3b ✅ (generics, §16/§98) 2026-08-08.** Appended `TypeParam`/`Trait`/`Callable` kinds (reuse the v2 record —
> **no version bump**); `!param<T,!trait<Ord>>` / `!trait<..>` / `!fn<(P)->(R)>` grammar; a live trait-conformance
> registry (`register_conformance`/`satisfies`, transitive over supertraits) + a memoized, diagnostic-bearing
> `substitute` (unbound params stay generic; a constraint violation reports the (param,trait)). Added `type_is_well_formed`
> — the decoder now rejects structurally-invalid records (a latent 3a gap). 70/70 × 4 configs + tidy + GCC
> `-Werror=switch`.
> **CEIR-3c ✅ (resource + view types, §23) 2026-08-08.** Nine appended kinds (Buffer/Image/Sampler/ResourceTable/
> AccelStruct/VideoFrame/AudioBuffer/ExternalResource/View — **no version bump**, reuse the v2 record). **Interp B**:
> a view TYPE carries the underlying resource + a range-dimension presence MASK (`byte/element/mip/layer/aspect`), the
> range VALUES are runtime (tensor/sparse → 3d; §24 domains = value semantics). Grammar `!buffer<plain,!f32>` /
> `!image<d2,fmt>` / `!view<RES,mip,layer>`; tri-split `view_combination_valid` (parser fails / decoder rejects /
> factory asserts). Fixed a corrupt-input abort (parser fell into the asserting factory on a prior error). 73/73 × 4
> configs + tidy + `-Werror=switch`.
> **CEIR-3d ✅ (shapes + tensors, §21/§35) 2026-08-08.** Four appended kinds (Dim/Shape/Tensor/SparseTensor — **no
> version bump**). Type-level foundation only (the `ceir.shape`/`ceir.tensor` op dialects + layout §22 are CEIR-18;
> tensor/sparse returned from the 3c boundary). `!dim<4|dyn|N>` / `!shape<..>` / `!tensor<e,s>` grammar; tri-state
> `shapes_broadcast` (right-aligned incompatible pos) / `shapes_reshape` (overflow→Unknown). Added `type_is_canonical`
> (decoder rejects non-canonical records — a name on an Int etc. — latent since 3a) + `dyn` reservation tri-split. A
> 3b×3d seam test (substitute a param through a tensor shape). 77/77 × 4 configs + tidy + `-Werror=switch` (31 kinds).
> **CEIR-3e ✅ (physical quantities, §17/§18) 2026-08-08.** One appended kind (`Quantity`, no version bump): tags a
> numeric underlying with an 8-base SI dimension (ADR-0078) **bit-packed into count+cols** (crd::units::Dim is
> compile-time-only → CEIR mirrors it at runtime; a static_assert pins the base order). Grammar `!qty<!f32,L1T-2>` /
> dimensionless `!qty<!f32,1>`; `quantity_dimensions_equal`→first_differing_base (the 3z Length+Time diag), `quantity_dim_mul/div`
> (i8 overflow→failure). ⭐ The **`units.erase` op is the FIRST non-reference dialect through the CEIR-2 generator** (a
> `.ceirop.toml` + regen, zero central edits; drift now 3 dialects × 5 files, smoke auto-generated). 83/83 × 4 configs +
> tidy + `-Werror=switch`.
> **CEIR-3f ✅ (ownership/lifetime qualifiers + escape predicate, §19) 2026-08-08.** One appended kind (`Qualified`, no
> version bump): a WRAPPER — `members[0]`=type, `count`=`OwnershipKind` (9 modes imm/mut/borrow/own/shared/weak/state/ext/
> transient) — chosen over nine kinds (a value has exactly one mode; the wrapper composes over resources too). Grammar
> `!qual<borrow,!buffer<plain,!f32>>`; tri-split `qualified_composition_valid` (rejects qual-of-qual/dim/shape/trait). The
> escape rule splits 3f/3z: 3f ships `value_escapes_region(Value*,Region*)` (def-use walk vs directional region
> containment), 3z composes it over `Qualified<BorrowedView>`. ⛔ Two advisor-caught fixes: **`create_operation` never
> wired `Region::m_parent`** (latent since 1a; the first upward walk reported false-positive escapes) and **`substitute`
> bypassed the tri-split** (retroactively closes qty-of-qty/qual-of-qual via a new `SubstResult::failed_compose`).
> Escape contract = direct-use, type-directed. 87/87 (773 assertions) × 4 configs + tidy + `-Werror=switch` (33 kinds).
> **CEIR-3z ✅ BAND-3 GATE (§16/§17/§19/§21) 2026-08-08.** The four band-3 error checks fire as **discriminating pointing
> diagnostics**: Length+Time (`quantity_dimensions_equal`→first_differing_base, base 0 AND base 2), rank-mismatched
> broadcast (`shapes_broadcast`→position 1 + control), borrowed-view escape (a NEW public `find_borrowed_escape(Module&)`
> →`BorrowEscape{value,escaping_use}` module walk over `!qual<borrow,_>` values — both value kinds; owned-escaper +
> inside-borrow correctly ignored), generic-constraint (`substitute`→failed_param/failed_trait, exact (T,Ord) + accept
> control). Pointing upgrade: `value_escapes_region`(bool) gained `first_escaping_use`→`Operation*` (a bool can't point).
> ⭐ **Honest boundary:** pointing predicates + the one escape walk; the op-level verifier wiring of dim/broadcast/
> constraint onto typed operands composes at **CEIR-4** (no producer op yet — 3e "predicate-now" precedent). `find_borrowed_escape`
> is first-offender, structural (not dominance — CEIR-5b), direct-use+type-directed. Dedicated `test_band3_gate.cpp`
> (`[gate3]`, 4 cases). 91/91 (791 assertions) × 4 configs + tidy + `-Werror=switch`. **→ BAND 3 CLOSED.**
> **CEIR-4a ✅ BAND 4 OPENS (§26 effect vocabulary) 2026-08-09.** `effect.hpp`: `EffectFamily` (all 27 §26 families, u8,
> append-at-end, static_assert-pinned to the generator) + `EffectRecord` POD `{family, target(None/Operand/Result),
> index, range_mask}` (range_mask reuses 3c ViewRange; kind-level — per-instance is 4d, callee-derived EffectsFn is
> CEIR-5). ⭐ Attach point **B1**: effects on `OpInfo` via `register_op(...,effects={})` (arena-COPIED), queried by
> `Context::op_effects` — not a 1d interface, not reflection-only. ⛔ **EMPTY≠UNKNOWN**: an empty span is "provably
> effect-free" only when `op_info!=nullptr`; unregistered = maximally effectful. ⛔ **func.call landmine** (a registered
> op defaulting to effect-free reads as *provably* none) → declares a conservative ExternalCall barrier. Pure⇒zero
> effects at both live arms. 2a schema: bare-family string OR `{family, operand|result, range}` table, generator
> validates vocab+index+range; `OpSchema.effects` StringView[]→EffectRecord[]; `.ops.json` string[]→object[] (schema_v1,
> scaffold field). NOT serialized (no version bump). 98/98 ctest (820 assertions) × 4 configs + tidy + opgen(36 py).
> **CEIR-4b ✅ (§27 determinism + §28 numerics) 2026-08-09.** `semantics.hpp`: `DeterminismClass` (5 §27 tiers +
> `Unspecified=0` default, static_assert-pinned; `External`→`ExternalNondeterminism` reconciled) on `OpInfo` via
> `register_op`, `op_determinism`; `CompilerMode` (Normal/Fast/Deterministic/Certified, session state NOT serialized) +
> `determinism_satisfies_mode` (6×4 matrix). `NumericalSemantics` = all 12 §28 knobs (0=Inherit), per-INSTANCE, ONE
> pack/unpack into a reserved `numerics` int attr (unpack validates bounds); `numerics_satisfies_mode` honors the fmad
> scar (Fast admits FMA/fast-math). `find_mode_violation(Module&)` = first op violating the active mode's §27 class OR §28
> numerics OR a corrupt `numerics` attr (violates every mode). Cook-time native≥op consistency; `OpSchema.native_determinism`
> typed. ⛔ EMPTY≠UNKNOWN (Unspecified fails strict modes). Boundaries: pass-wiring→CEIR-6, replay/alternatives→CEIR-5+,
> kind-vs-instance not cross-checked (CEIR-6). arith int ops = BitExact. 106/106 ctest × 4 configs + tidy + opgen(40 py).
> **CEIR-4c ✅ (§15 eval domains + §32 realtime + domain-legality verifier) 2026-08-09.** ⭐ `register_op` consolidated
> into an `OpSpec{traits,verify,effects,determinism,domain}` descriptor (designated initializers; migrated every site).
> `EvalDomain` (10 §15 domains + Unspecified, static_assert-pinned) per-op-KIND on `OpInfo` via the spec + 2a `domain`
> field + `op_domain`; `OpSchema.domain` StringView→EvalDomain. `RealtimeClass` (7 §32 + Unspecified) — a REGION property,
> NOT a schema field. A region's (domain+realtime) rides ONE packed `region_exec` int attr on the region-owning op (module
> CONTENT — survives round-trip; symmetric with 4b's session-only mode). `find_domain_violation(Module&)→DomainViolation`
> — innermost-tag-wins; seeded §32 rule (FileIO/NetworkIO illegal in AudioRealTime/DeviceTime/HostAudioTime); ⛔ an
> UNREGISTERED op in an audio region is flagged (maximally effectful), a registered effect-free op is legal (empty≠unknown).
> The 4c walk does NOT consume kind-domain (→CEIR-6). arith=EitherHostOrDevice, test=HostFrameTime. 115/115 ctest × 4
> configs + tidy + opgen(43 py).
> **CEIR-4d ✅ (effect-derived ordering hazards §26/§116) 2026-08-09.** A SCHEMA-QUIET slice (hazards DERIVED from 4a
> effects — no TOML/generator/regen/py). `hazard.hpp`: `HazardKind{None,War,Raw,Waw}` + `ResourceClass` (14) +
> `effect_access(EffectFamily)` (TOTAL switch, NO default → -Werror=switch guards a 28th). ⛔ `RandomRead` WRITES (a PRNG
> draw advances the stream — else RNG draws reorder, breaking replay); `TimeRead` inert-read; Alloc/Dealloc/Residency in
> Memory (use-after-free visible); IO one rw class; ExternalCall+Synchronization = Universe barrier. `Context::ops_hazard
> (before,after)` (WAW>RAW>WAR over effect-pairs sharing a resource `(class, Value|null)` + overlapping range with ≥1
> write; distinct Values non-aliasing; reports everything; unknown=Universe rw, Pure=None) + `collect_block_hazards` (O(n²)
> all-pairs reference, one block, list order). `Operation::result` loosened to const. 122/122 ctest × 4 configs + tidy.
> **CEIR-4z ✅ BAND-4 GATE 2026-08-09 → BAND 4 CLOSED (4a–4z).** A TEST-ONLY gate (like 3z) composing find_mode_violation +
> find_domain_violation + ops_hazard/collect_block_hazards over ONE curated module — the band's exit criterion "the
> compiler distinguishes reorderable vs ordered ops correctly" met verbatim. Centerpiece: the **WAR-needs-lifetime scar**
> (read(R)-then-write(R) = WAR from effects+SSA identity, no decl-order; different buffer = None). Exact 4-edge
> collect_block_hazards matrix + reverse sweep; ⛔ every gate op BitExact (the Unspecified-default mode-axis trap);
> orthogonality by FULL edge-list identity (numerics→Certified, Unspecified-op→determinism, side audio-RT+FileIO→domain —
> hazards identical each time); hazards survive serialize/deserialize (Value identity through the parser fixup). `test_band4_gate.cpp`
> (`[gate4]`, 5 cases). 127/127 ctest × 4 configs + tidy. → BAND 4 CLOSED.
> **CEIR-5a ✅ (structured control-flow region ops + the constant-cond if fold) 2026-08-09.** Band 5 opens (a GEAR
> CHANGE). The generator gained a region-SIGNATURE schema + THREE variadic axes (operands/regions/results; arg COUNT is
> the verifier contract, MIN-ARITY builders, full arity via create_operation). `ceir.core` = `scope`/`if` (value-producing,
> variadic results), `for`/`foreach` (typed region arg), `while` (cond+body, structural — cond-yields-1-bool→5b),
> `switch`/`match` (variadic case regions; patterns→CHIR), `yield` (variadic-operand Terminator) — all BitExact + ZERO
> effects. `Context::fold_constant_if` splices the taken branch (THEN and ELSE, both tested) + RAUWs the results with the
> yield's operands + erases; BAILS on non-const/multi-block/no-yield, a `region_exec`-tagged if OR any tagged op INSIDE
> the taken block (⛔ a rewrite must audit the attrs it MOVES), and a yield/result COUNT MISMATCH. Design: HOMOGENEOUS
> result types (placeholders until CEIR-6; the fold replaces them — no create_operation overload), MIN-ARITY builders,
> SKELETON-VERIFIES, loop-carried values→5d. ⚠ This turn disproved 2 checkpoint blocker-guesses (builder-count-param,
> create_operation-overload) — mark checkpoint blockers unverified unless the code was read. 138/138 ctest × 4 + tidy +
> opgen(49 py). NEXT = **CEIR-5b** (SSACFG verifier: dominance/terminators/block-args — the general yield contract + the
> 3f back-link earn their keep, §13/§115) → 5c (calls + EffectsFn) → 5d (state/delay) → 5z (executor gate).

> **CEIR-2 generator (reusable for every future band):** `tools/ceir_opgen/ceir_opgen.py` reads
> `engine/ceir/ops/<dialect>.ceirop.toml` → validates (structure+vocab of every §8 + ADR-0110 field) → emits from ONE
> model: `engine/ceir/generated/crd/ceir/gen/<d>_ops.{hpp,cpp}` + `<d>.ops.{json,md}` + `tests/ceir/generated/test_<d>_gen_smoke.cpp`
> (3 TEST_CASEs, globbed via CONFIGURE_DEPENDS). `--check` drift ctest guards all 5 files/dialect; `test_opgen.py` = 26
> validator/emitter unit tests. **Adding a CEIR op = a TOML edit + regen, zero central-enum/switch edits.**

**⛔ THE LIVE TRACKER IS `docs/detours/D-007-ceir-tracker.md`** (CEIR bands 0–32 + the RAH parallel track). CEIR — the
Cerid Execution IR — is the new master architectural spine (user-directed 2026-08-07): every reusable algorithm becomes
a versioned, inspectable, hot-reloadable **program asset**; native C++ only for genuinely new host/hardware capability.
**THE LAW:** `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` (§0–§185). Mantra: *ALGORITHMS ARE
PROGRAM ASSETS · CAPABILITIES ARE NATIVE PRIMITIVES · COMPILERS CHOOSE LOWERINGS · BACKENDS EXECUTE.* The old post-RAF
4-track table in `D-007-gpu-program-system.md` is **re-hung under the CEIR bands** and preserved as history/contracts
(A/RPL→CEIR-15 · C/MLR→CEIR-21 · hesap-GPU→CEIR-19 · frame+executor→CEIR-12/13 · B/I2D→CEIR-28 · D/D7E→CEIR-30).

**CEIR-0 DESIGN PHASE COMPLETE + ACCEPTED (2026-08-07/08):** 0a inventory (headline: RAF already did the atomic-vs-
composite split → CEIR is a promotion, not a rewrite) · **ADR-0108** (owned language stack; C++ no longer the *only*
authorable program — supersedes ADR-0081 §9) · **ADR-0109** (CEIR/CHIR/CKIR one-way layer contract + `crd-ceir`
host-only module + `crd-ceir-host`/`crd-ceir-gpu` dependency-inversion bridges + I3/I4/I5; **binding for CEIR-1**) ·
**ADR-0110** (native-intrinsic schema + plugin levels) · 0e CHIR-0 note · 0g two-axis maturity model · 0h deletion
ledger · 0z §184 report + sizing (CEIR-1…13 ≈ 34–55 KLOC, ~4–8 mo). CEIR-0f (D-007 restructure) executing.

**HOW WE WORK (user-directed):** **strict band order** CEIR-0→32; each band closes at its gate before the next. **RAH
runs in PARALLEL** (the binding/attachment vocabulary CEIR-9/11 lower onto). ⛔ Everything else PAUSED (§176: only bug
fixes, CKIR fixes, tests, docs, RAH, CEIR). ⛔⛔ Foundational/critical-path work done DIRECTLY, never delegated. ⛔⛔
Implementation forks require `isolation:"worktree"` + a tight mandate ([[feedback_implementation_forks_need_worktree_isolation]]).
**User controls commits (no AI trailer).**

## Active state

- **CEIR (spine) — CEIR-1a ✅ CLOSED (4-config per-slice sweep PASS, 2026-08-08).** `crd-ceir` module +
  `Context/Module/Operation/Value/Block/Region` + intrusive in-arena def-use + `crd::memory::GrowableLinearAllocator`
  (moved to crd-memory) + `crd-ceir-invariants` I3/I5 gates — all green across debug/asan/shipping-LTCG/tidy. The full
  sweep peeled **7 pre-existing cross-band blockers** (RAF/REN/CKIR bands never passed shipping-LTCG/asan-complete/tidy);
  all fixed gold-standard (2 real engine bugs: DX12+Vulkan RT pipeline-cache keyed by pointer/handle → content-hash).
  Log: `docs/sessions/2026-08-08-ceir-1a-and-preexisting-fixes.md`.
- **CEIR-1b ✅ CLOSED (2026-08-08).** `SymbolTable` (per-Module, arena-backed HashMap; duplicate-reject) + `Visibility`
  + the `ceir.func` dialect (`func.func`/`func.call`/`func.return`, cross-module resolution by name) — all on the
  generic Context factories (open-world). `tests/ceir` 12/12. **Gated across all 4 configs on crd-ceir** (a complete
  gate — crd-ceir has zero downstream consumers, grep-proven; full-tree sweep re-earns its keep at the band close).
- **CEIR-1c ✅ CLOSED (2026-08-08).** Interned typed attribute VALUES (`AttrValue`/`AttrId`, dedup) + a per-op
  AttrDict (`op->attr(name)` / `Context::set_attr`) + the source map (`register_file`→`file_id`, `file_path`) so
  every op's `SourceLoc` provenance is real (§111, no retrofit). Dissolved the 1b interim: `func.call`'s callee is
  now a `SymbolRef` attribute. `tests/ceir` 18/18. Gated all 4 configs (scoped-complete).
- **CEIR-1d ✅ CLOSED (2026-08-08).** Open-world **dialect registry** + op **traits/interfaces** + **verifier**
  dispatch — analyses query traits/interfaces, the core NEVER switches on op.kind (new **I6** grep-gate proves it,
  bites on `switch(op.kind())`). Unknown-dialect ops survive opaquely; the `func` dialect self-registers.
  `tests/ceir` 22/22. Gated all 4 configs (scoped-complete).
- **CEIR-1e ✅ CLOSED (2026-08-08).** Deterministic textual **printer** (IR→canonical MLIR-flavored text; pre-order SSA
  numbering + name-sorted attrs → byte-identical; floats keep a `.`/`e` marker; unknown-dialect opaque; **no layout**,
  §10) + recursive-descent **parser** (`parse→ParseResult`; use-before-def fixup pass, strings unescaped-before-intern,
  balanced-brace region count skipping string literals, malformed input rejected w/ byte offset). **`print(parse(x))==x`
  byte-exact.** MLIR-faithful symbol identity (advisor): func name/visibility now ride ON the op as `sym_name`/
  `sym_visibility` attrs (SymbolTable = an INDEX over `sym_name`), so identity round-trips through the generic attr
  machinery and the parser rebuilds the module table. `tests/ceir` **31/31**. Gated across the 5-config contract.
- **CEIR-1f ✅ CLOSED (2026-08-08).** **Binary serial form** (`binary.hpp`/`binary.cpp`: `serialize`/`deserialize`) —
  CRDR-shaped (ADR-0038): magic `'CEIR'` + version + FourCC/length chunks a reader **skips by length** when unknown
  (`STRP`/`SRCM`/`ATTR`/`BODY`). ⛔⛔ field-by-field LE (self-contained `put_u*` + `.ok` `Cursor`; can't link crd-kir).
  **⭐ Content-pure:** pools built from the module WALK, BODY holds pool INDICES not Context ids → the blob is a pure
  function of module content (dirty-context byte-equality proven). Carries `Region::kind` (closes the 1e divergence, via
  new `Context::set_region_kind`); `SourceLoc` survives by PATH; symbol identity via the shared `detail::register_symbol`
  (extracted from the parser). `serialize∘deserialize∘serialize` byte-exact; agrees with the text form. Malformed input
  rejected w/ byte offset (bad magic/version/truncation/trailing-junk/oob index). `tests/ceir` **37/37** (`build_rich`
  now a shared `rich_graph.hpp`). Gated across the 5-config contract; invariants I3/I5/I6 green both OSes.
- **CEIR-1g ✅ CLOSED (2026-08-08).** **`ModuleBuilder` fluent API** (`builder.hpp`/`builder.cpp`: `ModuleBuilder` +
  `OpBuilder` proxy + `InsertionGuard`). ⛔⛔ NO privileged bypass — every op routes through `Context::create_operation`
  + shared `detail::register_symbol`, so a builder module is **byte-identical to the hand-built one** (proven).
  `verify(&failing)` dispatches the REAL per-kind `Context::verify` (rejection test proves it, no stub); `build()`
  returns nullptr on a duplicate `sym_name` (op erased, no silent overwrite). `tests/ceir` **41/41**. Gated across the
  5-config contract.
- **CEIR-1h ✅ CLOSED (2026-08-08).** **The permanent harness, seeded** (§119/§167): round-trip fuzz (random valid
  modules via `ModuleBuilder`, fixed xorshift64 seeds — text+binary byte-exact), a `stable_hash` (FNV-1a over the 1f
  content-pure blob — NEW surface, deterministic + content-derived), and a malformed corpus + single-byte-corruption
  SWEEP (no crash; ASan is the proof). ⛔⛔ **The fuzz caught 2 real OOM crashes on day one** in code that had passed 4
  slices of gates — a huge textual def-id and a corrupt binary count; BOTH loaders hardened (text bound by input
  length; binary counts bounded by chunk length / `kMaxDecodeCount`). `tests/ceir` **46/46**. Gated across the 5-config
  contract.
- **CEIR-1z ✅ CLOSED (2026-08-08) — BAND-1 GATE.** A typed hello-world (func + const + call + return) round-trips
  **text ⇄ binary ⇄ builder byte-identically** and its callee symbol resolves after all three forms
  (`tests/ceir/test_hello.cpp`). `tests/ceir` **49/49**. Gated across the 5-config contract. ⭐⭐ **BAND 1 CLOSED
  (1a..1z).** Already committed this session: 1a core (`5f81ce8`) + the 7 pre-existing fixes & 1a docs (`6e6f183 "CEIR-1a
  finished"`). ⛔ NOW: (A) the USER commits+pushes the remaining **CEIR-1b..1z** batch (~38 files: `engine/ceir/**` +
  `tests/ceir/**` + `scripts/check_ceir_invariants.{ps1,sh}` + docs) — ONE commit
  `feat(ceir): band 1 core IR substrate (CEIR-1b..1z)` with the per-slice breakdown in the body (slices overlap in
  files → not per-slice stageable). (B) then make **GitHub CI GREEN** (whole-repo net; fix reds gold-standard, user
  commits fix batches). (C) MEMORY.md compaction to <17.1KB during the wait. Only after CI green → **CEIR-2**.
- **RAH (parallel track) — front = RAH-1a.2.** ✅ RAH-1a.1 (visbuffer fold) DONE + gated (REN-38-F6, 97 asserts, both
  backends). **NEXT = RAH-1a.2 (DELETE, user-chosen):** retire `IGBufferTarget`+`draw_gbuffer`+`create_gbuffer_target`
  (both backends) + `RenderingDesc.gbuffer`; migrate ~8 test sites to the `color`-span MRT path; needs a
  plain-vertex-MRT-color-span path + regular-target readback first. Then RAH-1a-close → RAH-2 (unblocks CEIR-11/B).
- **PAUSED (parked, not dropped):** B/I2D+SPR (ADR-0107 review pending) · C/CGP selector + HGP/MLR · D/MED codecs
  (animated GIF→TIFF→JPEG; real-GIF external-oracle corpus owed) · main roadmap (hesap v18, eylem v1c+). Resume paths:
  the CEIR tracker's "Paused" table.

## Recently landed

- **2026-08-08** — **CEIR-1a CLOSED** (4-config per-slice sweep PASS). The global close peeled 7 pre-existing
  cross-band blockers (RAF/REN/CKIR left them: shipping-LTCG/asan-complete/tidy had never run to completion) — all
  fixed gold-standard, incl. two real engine bugs (DX12+Vulkan RT pipeline caches keyed by pointer/handle →
  fnv1a_64 content hash; DX12 anyhit flake 200/200 after), the RAF-10 catch_discover_tests ENVIRONMENT split, the
  AS-4 ASan timing guard, the C4743 stale-obj wipe, and 37 clang-tidy errors across 12 files. Uncommitted (19 files;
  user commits — proposed message in the log). Log: `docs/sessions/2026-08-08-ceir-1a-and-preexisting-fixes.md`.
- **2026-08-07 (later)** — repository-wide **documentation hygiene pass** (uncommitted): context.md → dashboard
  (history archived), ROADMAP/systems/debt/AGENTS/READMEs refreshed to honest state, retired-module overviews
  DELETED (user direction; git history keeps them), research outcome stamps, ADR index + link fixes. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`.
- **2026-08-07/08** — **CEIR pivot + CEIR-0 design phase COMPLETE:** CEIR becomes the master spine; the live tracker
  `docs/detours/D-007-ceir-tracker.md` created; CEIR-0a inventory + ADRs 0108/0109/0110 + the 0e/0g/0h/0z design notes
  all accepted; D-007 restructured (CEIR-0f). (uncommitted at time of writing — user commits.)
- **2026-08-07** — post-RAF 4-track kickoff: RAH-1a.1 + CGP-0/CUDA + MED-1 (`c116e98`); D-007 §POST-RAF + §UI/2D
  programmes + four-track tracker (`e3f8e5e`). Log: `docs/sessions/2026-08-07-post-raf-tracks-rah1-cuda.md`.
- **2026-08-06** — **RAF band COMPLETE** (`af3e04c`): `FramePassKind` retired, ADR-0106 closed.
  Log: `docs/sessions/2026-08-06-raf12-3-retire-framepasskind.md`.
- **2026-08-03…05** — RAF-0…12: substrate → one-submission frame graph → executors → engine-default assets →
  app-custom renderer → hot reload → legacy deletion. Logs: `docs/sessions/2026-08-0{3,4,5}-raf*.md`.

## Open questions / risks

- **Per-slice gate — RATIFIED (2026-08-08):** each slice closes on **2 Windows + 2 Linux configs + tidy**, all
  clean (win-debug + win-asan + linux-debug + linux-asan + tidy; Linux via WSL), **scoped to the changed module**
  (crd-ceir has zero downstream, grep-proven → crd-ceir-tests across those configs is complete). **No whole-repo
  suite per slice** (too slow). **GitHub CI is the whole-repo safety net and must stay GREEN.** See
  `project_ceir_autonomous_loop_grant`. ⛔ **Between CEIR-1 and CEIR-2: fix CI green** (it has real build/test reds).
  ⛔ **At CEIR-14: expand its subslices explicitly in the tracker.** GOAL = all bands 1→32 closed.
- **Pending user review:** RAH-0 audit (`docs/systems/rah-0-canonical-model-audit.md`) + ADR-0107
  (`docs/decisions/0107-ui-2d-architecture.md`). Track B code is blocked on the ADR-0107 review.
- `MEMORY.md` ≈ 19.9 KB (hard read limit 24.4 KB) — deeper cull deferred, entries must be MERGED/DROPPED not just
  hook-trimmed.
- The integrated CUDA fork worktree `.claude/worktrees/agent-af34b487c5544c8fa` can be removed.

## Gates that matter

Per-slice DoD: `scripts/per-slice-check.ps1` (+ `-IncludeRelease` for GPU/LTCG slices); cluster close =
`scripts/full-sweep.ps1` (18-config). **Run `ctest`, never the bare test binary** (guards are ctest-only). GPU slices:
`ValidationCapture` + both backends. Tidy per touched file via `scripts/tidy-files.ps1`, never accumulated.

## Active detour

**D-007 (merged with D-008 on 2026-07-11) — the GPU program system.** ACTIVE; grew out of hesap v17 (2026-07-07).
CKIR IR + gpu-context convergence + the full visual frontier + RAF (all ✅) → now **CEIR is the master spine** (2026-08-07;
the live tracker is `docs/detours/D-007-ceir-tracker.md`, the landed-history ledger is `D-007-gpu-program-system.md`).
Everything above is D-007 state. Queue rules: `docs/detours/README.md`.

## Recent milestones (one line each; details in session logs + `docs/bench/`)

- **2026-08-06 — RAF complete:** engine renderers are ordinary assets; one backend-neutral command model; executor
  registry; hot reload; legacy paths deleted (ADR-0106).
- **2026-07-21…08-03 — REN-36…41:** authored frame graphs/techniques/materials (`.crdm/.crdt/.crdv/.crdl/.frame.toml`),
  bindless+multi-draw (38-G1 119 fps), indexed-pull reuse, O(chunks) extract, soft shadows (PCSS/EVSM/MSM), velocity +
  TAA, Nanite-class cluster LOD start.
- **2026-07-13…16 — the GPU compute crush campaigns:** 2D FFT 1.16–1.20× cuFFT bit-exact; reduction beats CUB; radix
  sort 0.73× CUB (bit-exact, 8.4× session gain); NRC fused MLP 2.37× cuBLAS; B14 SVGF/DDGI/ReSTIR/NRC + B15
  atmosphere/clouds + B16 FFT ocean — all gold-standard CKIR. (Narrative: the context-history archive; boards:
  `docs/bench/`.)
- **2026-07-10…12 — D-007 device+IR convergence:** one `VkDevice`, I1/I2 leak gates closed, oracle rounds per-op, CUDA
  fan-out bit-exact.
- **2026-07-23 — RET band: crd-rhi/rhi-vulkan/renderer/shader DELETED** (ADR-0105); gpu-context IS the graphics layer.
- **2026-07-02 — hesap v13 close:** interpolation/quadrature/differentiation/motion — full peer-board crush (scipy/
  MATLAB/Boost/GSL/Ruckig).
- **Earlier (hesap v0→v12, geometry, units, scene/ECS):** see `docs/phases/` + the archive.

## Paused main-roadmap work

- **Phase 3.1.6 hesap:** paused mid-v17 (GPU compute) — v17's substrate is being built AS D-007; hesap-GPU is the
  detour's last stop. v14 tensors ✅ (2026-07-05) · v15 forward AD ✅ · v16 reverse AD ✅ (2026-07-07, ADR-0097).
  `docs/phases/phase-3.1.6-hesap.md`.
- **Phase 3.1 eylem:** ⏸ paused at v1b close (ADR-0076 §12 sequencing); resumes v1c+ after the detour + hesap.
  `docs/phases/phase-3.1-eylem.md`.

For the full doc map: `docs/README.md`. ADR index: `docs/decisions/README.md`. Open debt: `docs/debt.md`.
