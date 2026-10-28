# 2026-10-28 — SIMD-DISPATCH: the -Wpsabi notes at their cause, and the gnr/dmr clause

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `0553e7a9`. CI read for this batch:
[eylem zmm body](2026-10-28-simd-dispatch-avx512-eylem.md#ci-first).

## The notes

The build logs of complete run 40649898318 (`3e849d96`) hold 40 "the ABI for passing parameters with N-byte alignment
has changed in GCC 4.6" notes on linux-gcc-debug-sse2, 17 on linux-gcc-debug and 28 on linux-gcc-release. Every
32-byte note is in a portable or sse2 unit (or general code on the sse2 lane) that passes `Vec8f` or `Vec4d` by value:
`vec8f.hpp`, `vec4d.hpp`, `vector_steps.hpp`, `batch8_core.hpp`, `small_n_codelets.hpp`, `jet_simd.hpp`, the level
probe. avx2 units pass the same types and stay silent. The one 64-byte note is `World::add_component` in AVX2 general
code, instantiated with the 64-aligned `Wide` test component.

So the cause is GCC's x86-64 rule: an argument passed by value whose type is aligned above the unit's widest vector
register goes on a realigned stack, and GCC notes the ABI change once per unit. The types were `alignas(32)` and
`alignas(64)` on every level, also where they are split into 128-bit halves. Passing by reference only moves the note
to the next by-value function. `-Wno-psabi` would hide it and also silence GCC's real warning about AVX vector
arguments without AVX enabled, a free cross-ISA tripwire.

## The fix

- **Types:** `backend.hpp` gives `kNativeRegisterBytes` (64 at avx512, 32 at avx2, else 16, NEON included) and
  `simd_alignment(bytes)`. `Vec8f`, `Vec8i`, `Vec4d`, `Vec8d`, `Vec16f` and `Vec16i` lose their type-level `alignas`,
  so their members carry it: 16 below avx2, 32 at avx2, 64 for the 512-bit types at avx512. Vec4d's lane array is
  `alignas(16)`, the Soa floor. Each header asserts `alignof == simd_alignment(sizeof)`. Sizes are unchanged, and so
  are the bits: alignment is not arithmetic.
- **Scene sinks:** `World::add_component`, `Commands::add_component` and `Commands::set_component` take
  `const T&` (one copy into a local the storage moves from) or `T&&` constrained to a non-reference `T` (the
  storage's or payload's move source itself). Explicit-T and deduced lvalues bind the first, rvalues and braced lists
  the second. An rvalue now makes one move fewer, and a move-only component still works.

## Tests

- `test_simd.cpp`: the alignment of all six types and `kNativeRegisterBytes`, pinned per backend.
- `test_schedule.cpp`: World and Commands sinks with explicit-T and deduced lvalues (one copy, source unchanged),
  temporaries, braced lists and `std::move` (no copy), a move-only component and a 64-aligned component stored
  aligned with its bytes.
- win-debug: crd-math-tests `[simd]` 762 assertions in 74 cases, `[simd-levels]` 534,378 in 3 (portable to avx2);
  SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`: 667,778 in 3; crd-scene-tests 35,862 in 299.
- Hosted: the GCC lanes' logs should hold no "has changed in GCC" line. A note prints once per unit, so a fixed site
  can reveal another; the next batch reads the logs.

## gnr and dmr

No body selects on AVX10 or APX: the only feature variants are avxvnni and avx512vnni (`CrdSimd.cmake`), and no
kernel or dispatcher reads the AVX10 or APX bits. ADR-0134 §1 adds a variant only for a measured kernel, and no
AVX10 or APX hardware is here. So no gnr or dmr arm applies. When one does, detection is already in
`cpu_features.cpp`, the variant goes in `_CRD_KERNEL_FEATURE_LEVEL_<f>`, and the CPU in that suite's `SDE_CPUS`.
