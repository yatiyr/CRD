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
    test_program_asset.cpp # CEIR-7a: the sec 105-107 program-as-asset metadata - interface hash, dependency record, strict cook-time registration check
    test_extern_type.cpp # CEIR-8a: the open-world type model (ADR-0111) - dialect-defined type-classes behind TypeKind::Extern, zero central-enum edits
    test_extern_attr.cpp # CEIR-8b: the open-world attribute model (ADR-0112) - aggregate/wrapper kinds + dialect attr-classes behind AttrKind::Extern, zero central-enum edits
    test_effect_open.cpp # CEIR-8c: effect family widening (u32->u64) + the open-world effect-location model (ADR-0113) - dialect location-classes behind EffectTarget::Extern
    test_stable_id.cpp   # CEIR-8d: content-independent stable semantic identity (ADR-0114) - per-op StableId, STID chunk, content-hash id-independence, reorder-invariant state schema
    test_interface_typed.cpp # CEIR-8e: trait/interface split (ADR-0115) - closed core traits (kKnownTraitsMask guard) + the typed open-world op-interface surface (CostInterface, FNV InterfaceId)
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
