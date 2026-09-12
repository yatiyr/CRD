#pragma once

// CEIR-30b-2b-2b — the ONE two-class-sandwich oracle body, shared by every gate that runs `gemm(x,W0) → relu-mlp(.;W1,W2) →
// gemm(.,W3)`: the CUDA three-way gate (test_ceir_pipeline_cuda.cpp), the device-free runner gate (test_two_class.cpp), and the
// Host executor gate (test_host_exec.cpp). Fills x/W0/W1/W2/W3 with a fixed seed pattern + computes the CPU oracle z, ALL
// sequential-k prod-then-add (mirrors emit_contract_cuda AND kir::eval_cpu) so an fmad=false device output OR the f32-faithful CPU
// eval is BIT-EXACT vs it. ⛔ every width must be ≤ 64 (the fixed row scratch); W0 is square [d0,d0]. Row-major throughout.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp> // crd::math::max — the relu

namespace crd::tests
{
// x[mrows,d0], W0[d0,d0], W1[d0,d1], W2[d1,d2], W3[d2,d3] → oracle z[mrows,d3] = gemm(relu(gemm(gemm(x,W0),W1)),W2) @ W3.
inline void fill_two_class_sandwich(crd::u32 mrows, crd::u32 d0, crd::u32 d1, crd::u32 d2, crd::u32 d3, float* x_in, float* w0_in,
                                    float* w1_in, float* w2_in, float* w3_in, float* oracle)
{
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d0; ++i) { w0_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 6) - 2); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    for (crd::u32 i = 0; i < d2 * d3; ++i) { w3_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 4) - 1); }
    for (crd::u32 mm = 0; mm < mrows; ++mm)
    {
        float xp[64]; // x' row = gemm(x,W0) [width d0], plain (no activation)
        for (crd::u32 nn = 0; nn < d0; ++nn)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { const float prod = x_in[mm * d0 + kk] * w0_in[kk * d0 + nn]; acc = acc + prod; }
            xp[nn] = acc;
        }
        float h1[64]; // relu(x' @ W1) [width d1]
        for (crd::u32 nn = 0; nn < d1; ++nn)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { const float prod = xp[kk] * w1_in[kk * d1 + nn]; acc = acc + prod; }
            h1[nn] = crd::math::max(acc, 0.0F);
        }
        float yy[64]; // y = h1 @ W2 [width d2]
        for (crd::u32 j = 0; j < d2; ++j)
        {
            float acc = 0.0F;
            for (crd::u32 nn = 0; nn < d1; ++nn) { const float prod = h1[nn] * w2_in[nn * d2 + j]; acc = acc + prod; }
            yy[j] = acc;
        }
        for (crd::u32 l = 0; l < d3; ++l) // z = y @ W3 [width d3]
        {
            float acc = 0.0F;
            for (crd::u32 j = 0; j < d2; ++j) { const float prod = yy[j] * w3_in[j * d3 + l]; acc = acc + prod; }
            oracle[mm * d3 + l] = acc;
        }
    }
}
} // namespace crd::tests
