# The allocator operation-sequence fuzz target (DIAG.10a): every production allocator -- TLSF, pool, linear, stack,
# the growable TLSF, pool and linear, the offset allocator, the epoch ring, the DiagnosticAllocator, the
# generation-checked SlotMap, the VirtualMemoryAllocator and the StreamingAllocator (small reservations released per
# input) -- driven by a decoded operation stream against an independent reference model. The
# committed corpus is replayed on every lane; linux-clang-fuzz builds the libFuzzer executable from the same source.
# Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
crd_fuzz_target(crd-fuzz-memory-allocators
    SOURCES fuzz_allocator_ops.cpp LIBRARIES crd-memory crd-containers CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
