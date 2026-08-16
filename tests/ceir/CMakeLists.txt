# CEIR-2d: the GENERATED per-dialect smoke tests (test_<dialect>_gen_smoke.cpp) live under generated/ and are globbed
# in via the house CONFIGURE_DEPENDS collector — so a NEW dialect's smoke test compiles with ZERO edit here (the §7/§8
# "add an op via a TOML edit alone" property, extended to the test skeleton). ceir_opgen.py owns these files.
crd_collect_sources(CEIR_GENERATED_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/generated")

add_executable(crd-ceir-tests
    test_ir_core.cpp  # CEIR-1a: core IR — arena storage, intrusive def-use, RAUW, O(1) op edit/erase, no-per-op-malloc
    test_func.cpp     # CEIR-1b: SymbolTable + ceir.func (func.func/call/return, visibility, cross-module symbol refs)
    test_attr.cpp     # CEIR-1c: interned typed attributes + per-op AttrDict + source map + SourceLoc provenance
    test_dialect.cpp  # CEIR-1d: dialect registry + op traits/interfaces + verifier + unknown-dialect preservation
    test_print.cpp    # CEIR-1e (part 1): deterministic canonical printer
    test_roundtrip.cpp # CEIR-1e (part 2): textual parser + byte-exact print/parse/print round-trip
    test_binary.cpp   # CEIR-1f: FourCC-chunked binary serialization + byte-exact serialize/deserialize round-trip
    test_builder.cpp  # CEIR-1g: ModuleBuilder fluent API (byte-identical to hand-built; verifier routed, no bypass)
    test_fuzz.cpp     # CEIR-1h: round-trip fuzz (random valid modules via ModuleBuilder) + stable-hash harness
    test_malformed.cpp # CEIR-1h: malformed-input rejection corpus + single-byte-corruption sweep (no crash)
    test_hello.cpp    # CEIR-1z: BAND-1 GATE - typed hello-world round-trips text <=> binary <=> builder byte-identically
    test_arith_gen.cpp # CEIR-2a+2b: the GENERATED arith dialect (ceir_opgen.py) - builders/wrappers/verifier, zero central edits
    test_type.cpp     # CEIR-3a: interned structural types (scalars + aggregates) + canonical print/parse round-trip
    test_band3_gate.cpp # CEIR-3z: BAND-3 GATE - the four pointing diagnostics (dim/broadcast/borrow-escape/constraint)
    test_effect.cpp   # CEIR-4a: the §26 effect system - typed effect records on OpInfo, op_effects, arena copy, empty!=unknown
    test_semantics.cpp # CEIR-4b: §27 determinism classes + compiler modes + the §28 numerical-semantics packed attr
    test_domain.cpp   # CEIR-4c: §15 eval domains + §32 realtime classes + the region_exec tag + domain-legality verifier
    test_hazard.cpp   # CEIR-4d: effect-derived ordering hazards (RAW/WAR/WAW) + the family classifier + block collector
    test_band4_gate.cpp # CEIR-4z: BAND-4 GATE - compose mode/domain/hazard over a curated module; the WAR-lifetime scar
    test_controlflow.cpp # CEIR-5a: ceir.core structured region ops (scope/if/for/foreach/yield) + the constant-cond if fold
    test_structure.cpp # CEIR-5b: the §115 structure verifier - dominance/capture/isolation/terminators/yield-count
    test_calls.cpp    # CEIR-5c: calls - the func.call EffectsFn hook (callee-derived effects) + the recursion-policy verifier
    test_state.cpp    # CEIR-5d: sec 20 explicit-state ops (core.state/delay/history) + the cycles-only-through-state verifier rule
    test_exec.cpp     # CEIR-5z: the sec 118 reference executor - per-op semantics, the state ring, fuel, typed errors, open-world install
    test_band5_gate.cpp # CEIR-5z: BAND-5 GATE - a pinned program (loop + switch + calls + state accumulator) executes byte-identically builder == text-parsed
    test_async.cpp    # CEIR-6a: ceir.async ops (sec 37) + the sec 116 use-once token-misuse verifier + the audio Synchronization flip
    test_task.cpp     # CEIR-11a: the six sec-38 residual ceir.task ops (spawn/continuation/main_thread/worker/fiber_wait/group) + sequential reference semantics + zero-edit token-verifier coverage + the audio flip
    test_plan.cpp     # CEIR-11b: the compiled execution tier - plan compiler (Module -> dense CompiledPlan) + executor + the differential harness (reference vs compiled, pin_values byte-compare); stage 2 = straight-line arith
    test_resource.cpp # CEIR-12a: the ceir.resource dialect (sec 36) - find_resource_misuse verifier (view/export/declare/import typing) + a text/binary round-trip of View/ExternalResource kinds
    test_lifetime.cpp # CEIR-12c: the resource alias/lifetime analysis (sec 78) - compute_block_lifetimes + resources_interfere/may_alias; the WAR-lifetime + slot-SIZE frame-graph scars as IR ctests
    test_planner.cpp  # CEIR-12d: the memory planner (sec 78/162) - plan_block_memory interval-coloring + inspectable MemoryPlan; the transient_physical<logical proof + plan<->may_alias consistency
    test_band12_gate.cpp # CEIR-12z: the BAND-12 composing gate - ONE frame-graph module composes 12a-d in sequence (type + intent verifiers, WAR-lifetime through a view + slot-SIZE scars, the REN-1 physical<logical proof, Latency cross-check, round-trip reproducibility)
    test_compute.cpp  # CEIR-13a: the ceir.compute dialect (sec 42) - dispatch/dispatch_indirect + find_dispatch_misuse (access tokens/arity, binding-is-resource, grid-is-index, args-is-buffer); the conservative GPUCommand+ambient effect baseline (hazards export, extends 12c ranges)
    test_transfer.cpp # CEIR-13b: the ceir.transfer dialect (sec 50) - copy/upload/readback/clear/mip_gen + find_transfer_misuse (transferable, copy src!=dst, clear value fold+image, mip-gen is-image); STATIC per-kind effects (precise upload->readback RAW, no ambient bleed)
    test_render.cpp   # CEIR-14a: the ceir.render dialect (sec 40/41) - render.scope region op + color/depth attachment declare ops (8a attachment type-classes) + find_render_misuse (attachment-is-image, closed-vocab attrs, RAH-1a.1 typed-clear-vs-format, scope-operands-attachment-typed, at-most-one-depth, area/sample bounds); text==builder round-trip
    test_frame.cpp    # CEIR-15a: the ceir.frame dialect (sec 39) - frame.graph region op + frame.pass/draw_list/history (resources REUSE resource.declare/import) + find_frame_misuse (executor-symbol, access tokens+arity, operand-resource-or-draw_list, closed-vocab for_each/queue/cull/sort, the frame.history lifetime=history operand, pass-inside-graph); text==builder round-trip
    test_program_asset.cpp # CEIR-7a: the sec 105-107 program-as-asset metadata - interface hash, dependency record, strict cook-time registration check
    test_extern_type.cpp # CEIR-8a: the open-world type model (ADR-0111) - dialect-defined type-classes behind TypeKind::Extern, zero central-enum edits
    test_extern_attr.cpp # CEIR-8b: the open-world attribute model (ADR-0112) - aggregate/wrapper kinds + dialect attr-classes behind AttrKind::Extern, zero central-enum edits
    test_effect_open.cpp # CEIR-8c: effect family widening (u32->u64) + the open-world effect-location model (ADR-0113) - dialect location-classes behind EffectTarget::Extern
    test_stable_id.cpp   # CEIR-8d: content-independent stable semantic identity (ADR-0114) - per-op StableId, STID chunk, content-hash id-independence, reorder-invariant state schema
    test_interface_typed.cpp # CEIR-8e: trait/interface split (ADR-0115) - closed core traits (kKnownTraitsMask guard) + the typed open-world op-interface surface (CostInterface, FNV InterfaceId)
    test_capability.cpp  # CEIR-8f: the U-57 capability contract (ADR-0116) - CapabilityId, required set on OpInfo, module-wide program set in the interface hash, host-grant check
    test_safety.cpp      # CEIR-8f: the domain/safety split (ADR-0116) - effect_safety total switch + op_safety fold; the two-RT-oracles subset
    test_time_domain.cpp # CEIR-8f: typed time domains (ADR-0116) - the time dialect over 8a type-classes (wall != sim), plugin domain, verify hook, U-56 round-trip
    test_pass_manager.cpp # CEIR-8g: AnalysisManager + PassManager (ADR-0117) - cached analyses + never-stale invalidation via preserved sets
    test_rewrite.cpp     # CEIR-8g: rewrite/conversion skeleton (ADR-0117) - try_apply per-op + ConversionTarget legality (unlisted=Illegal)
    test_diagnostic.cpp  # CEIR-8g: DiagnosticEngine (ADR-0117) - stable codes/severity/SourceLoc/notes/fixits, one surface, text copied
    test_transaction.cpp # CEIR-8i: transactions (ADR-0119) - atomic commit/rollback, byte-identical A/B rollback, id-under-rollback, the 8h dirty seam
    test_band8_gate.cpp  # CEIR-8z: BAND-8 GATE - ONE composed program (extern type+attr+caps+stable-ids) proving open-world round-trip/preserve/unify/transaction, cross-Context interface-hash purity, composed-blob fuzz
    test_notebook.cpp    # CEIR-9a: U1 notebook/incremental PROOF - real arith cells + the 8h IncrementalDag + 8i transactions; exact recompute counts, the section-107 early-cutoff (content=formula/interface=value), no second engine
    test_daw.cpp         # CEIR-9b: U2 DAW/timeline PROOF - a mock audio dialect over 8f time domains + 5d state/feedback + 4c region legality + 8e latency interface; offline=tag-flip, the two-oracles subset, no second engine
    test_dcc.cpp         # CEIR-9c: U3 DCC modifier-graph PROOF - a mock modifier stack; 8h affected_by AS the Blender depsgraph (tag-then-evaluate), precise upstream isolation over a two-branch join, the section-107 early-out
    test_cad.cpp         # CEIR-9d: U4 CAD parametric PROOF - the 8i transaction is the headline (multi-op edits in one tx, byte-identical rollback, a constraint = the commit-verify), over the 8h affected_by depsgraph
    test_eda.cpp         # CEIR-9e: U5 PCB/EDA PROOF - external tools as 8f capability-bearing ops (typed contract + effects/determinism/provenance); U-34 domain-neutral (no GPU/render vocabulary); DRC = the board verifier
    test_ecs.cpp         # CEIR-9f: U6 Game/ECS PROOF - the 4d hazard analysis over 4a EcsRead/EcsWrite effects IS the parallelism oracle; per-component identity = the SSA Value; a whole-store write is the structural barrier
    test_agent.cpp       # CEIR-9g: U7 Agent PROOF - the full agent loop (discover via OpSchema registry -> 8i tx -> author -> module-wide sweep + op-local backstop -> 8g diagnostics -> commit/rollback), ZERO hard-coded op knowledge
    test_plugin.cpp      # CEIR-9h: U8 MANDATORY external-plugin PROOF - a full plugin dialect (type+attr+location+interface+ops+verifier+rewrite+lowering+provider) via public APIs only, ZERO central-enum edits (grep-gated), agent-discoverable
    test_reload_migration.cpp # CEIR-10a: the hot-reload state-migration core - collect_state_schema + contract_hash (the Reject/Migrate decision-table split) + Interpreter snapshot/restore-by-8d-stable-id (the cell value move)
    test_rt.cpp       # CEIR-19a: the ceir.rt dialect (sec 49) - the six RT orchestration ops (blas_build/instance_populate/tlas_build/sbt_build/trace/ray_query) over 8a blas/tlas/sbt Extern type-classes + find_rt_misuse (operand type-chain incl. ray_query-rejects-SBT, geometry_kind closed vocab, instance_count/max_recursion range); host-intrinsic reflection (ADR-0110 provider=host)
    ${CEIR_GENERATED_TESTS} # CEIR-2d: generated per-dialect smoke tests (auto-grow as dialects/ops are added)
)

target_link_libraries(crd-ceir-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-tests)

catch_discover_tests(crd-ceir-tests)

# CEIR-2: gate the op-definition generator (tools/ceir_opgen). The DRIFT test reruns `ceir_opgen.py --check` (the
# committed generated C++ must be a byte-identical regen of the *.ceirop.toml); the VALIDATOR test runs the schema
# validator's unit suite. Both are Python (stdlib only). ⛔ if Python3 is absent the gate FAILS LOUDLY, never skips
# (an ungated generator is the tidy-blind scar) — CI + every dev toolchain has Python.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-ceir-opgen-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/ceir_opgen/ceir_opgen.py" --check)
    add_test(NAME crd-ceir-opgen-validator
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/ceir_opgen/test_opgen.py")
else()
    add_test(NAME crd-ceir-opgen-NEEDS-PYTHON3 COMMAND "${CMAKE_COMMAND}" -E false)
endif()
