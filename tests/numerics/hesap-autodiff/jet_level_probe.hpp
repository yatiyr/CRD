// The jet probe of one CPU level (SIMD-DISPATCH, ADR-0134 §5, §7): kernels/jet_level_probe.cpp is compiled once per
// level by crd_add_cpu_kernels, so each table runs that level's JetPackD and its drivers over the same inputs.
// test_jet_levels.cpp compares every level the host executes with the portable one. The header holds only data
// declarations, so a kernel unit compiles nothing from it.

#pragma once

#include <crd/core/types.hpp>

namespace crd::test::jet_probe
{

// The jet widths the probe runs: one partial, ragged and full Vec4d and Vec8d registers, and several of each.
inline constexpr int kSizes[] = {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32};
inline constexpr crd::usize kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);
inline constexpr int kMaxN = 32;
// The operations jet_probe_ops.hpp runs on each pair of operands.
inline constexpr crd::usize kOpCount = 29;
// Words a probe leaves untouched after each value and partials it writes (a store past the partials changes them).
inline constexpr crd::usize kGuard = 4;
inline constexpr crd::usize kOpWords = kOpCount * ((kSizeCount * (1 + kGuard)) + 117); // 117: the sum of kSizes
inline constexpr crd::usize kDriverWords = 1024;

// The two operands: values and partials (each its own lane pattern).
struct Input
{
    crd::f64 a;
    crd::f64 b;
    crd::f64 s; // the scalar of the mixed operations
    crd::f64 da[kMaxN];
    crd::f64 db[kMaxN];
};

// One JetPackD<kSizes[i]>'s layout at the level.
struct Layout
{
    crd::u32 size;
    crd::u32 align;
    crd::u32 regs;
    crd::u32 lanes;
};

struct Table
{
    // Every operation at every width on `in`: per width and operation the value, then the partials, then kGuard
    // words left as they were. Returns the words it covered (kOpWords).
    crd::usize (*ops)(const Input& in, crd::f64* out) noexcept;
    // The gradient and Jacobian drivers on x[0..kMaxN) at several tile widths and lengths, each output followed by
    // kGuard words left as they were. Returns the words it covered (at most kDriverWords).
    crd::usize (*drivers)(const crd::f64* x, crd::f64* out) noexcept;
    Layout layout[kSizeCount];
};

extern const Table kJetProbePortable;
extern const Table kJetProbeSse2;
extern const Table kJetProbeAvx2;
extern const Table kJetProbeAvx512;

} // namespace crd::test::jet_probe
