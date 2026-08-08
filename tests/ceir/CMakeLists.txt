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
)

target_link_libraries(crd-ceir-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-tests)

catch_discover_tests(crd-ceir-tests)
