#pragma once

// CEIR-30c-2 — a shared TEST helper: read a whole committed asset file into a char Array (REQUIRE-fails on a bad path,
// the authored-asset PRESENCE gate). Hoisted from test_band24_gate.cpp's anon-namespace `slurp` (CEIR-29a-3b-2) so the
// committed-asset reading gates in test_band24_gate / test_sharding / test_ceir_pipeline_cuda share ONE reader (no drift).
// The caller passes a Context whose allocator owns the returned bytes; ceir::parse() then consumes them.

#include <crd/ceir/context.hpp>
#include <crd/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <ios>

namespace crd::ceir::test_support
{
inline crd::containers::Array<char> slurp_asset(const char* path, Context& ctx)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    crd::containers::Array<char> s(ctx.allocator());
    s.resize(static_cast<crd::usize>(sz), '\0');
    f.read(s.data(), sz);
    return s;
}
} // namespace crd::ceir::test_support
