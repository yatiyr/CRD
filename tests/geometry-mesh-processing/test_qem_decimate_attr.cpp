// test_qem_decimate_attr.cpp — REN-40-C1: decimation that KEEPS its appearance
// attributes (`qem_decimate_attr`, driven by the Hoppe attribute quadric).

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/attribute_quadric.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace mp = crd::geometry::mesh_processing;
using V3     = crd::math::Vec3<crd::f32>;

// ⭐⭐ THE GATE THAT JUSTIFIES THE WHOLE METRIC, built as a dichotomy.
//
// The mesh is a FLAT grid, so the geometric quadric is identically zero — a
// position-only decimator is completely blind here and every collapse looks free
// to it. The attribute field is deliberately NON-LINEAR (u = x²), so no
// piecewise-linear model reproduces it and the attribute term is the only thing
// with an opinion about which edges to collapse and where the merged vertex goes.
//
// The assertion is not "it ran". It is that the surviving UVs still lie on the
// TRUE field — which is what "the texture does not swim" means, stated as a
// number — and that the same measurement on the position-only-then-transfer path
// is strictly worse.
TEST_CASE("REN-40-C1 GATE: attribute-aware decimation keeps UVs on the true field where position-only is blind",
          "[geometry][mesh-processing][qem][ren40][lod]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32               n = 13U; // 13x13 vertices -> 288 triangles
    crd::containers::Array<V3>       pos(&alloc);
    crd::containers::Array<crd::f32> uv(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            const auto x = static_cast<crd::f32>(i) / static_cast<crd::f32>(n - 1U);
            const auto y = static_cast<crd::f32>(j) / static_cast<crd::f32>(n - 1U);
            pos.push_back(V3{x, y, 0.0F});
            uv.push_back(x * x); // the NON-LINEAR channel — no linear model reproduces it
            uv.push_back(y);
        }
    }
    for (crd::u32 j = 0; j + 1U < n; ++j)
    {
        for (crd::u32 i = 0; i + 1U < n; ++i)
        {
            const crd::u32 v0 = (j * n) + i;
            idx.push_back(v0);
            idx.push_back(v0 + 1U);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0 + n);
        }
    }

    mp::HalfEdgeMesh<crd::f32> in(&alloc);
    REQUIRE(in.build_from(crd::containers::ConstSpan<V3>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()})
            == mp::BuildStatus::Ok);
    const crd::u32 faces_in = in.face_count();

    mp::QemDecimateOptions<crd::f32> opts{};
    opts.target_face_count = faces_in / 4U;
    opts.output_allocator  = &alloc;

    // ── arm A: attributes IN the metric ──
    crd::containers::Array<crd::f32> uv_out(&alloc);
    mp::HalfEdgeMesh<crd::f32>       out = mp::qem_decimate_attr<crd::f32, 2U>(in, uv.data(), opts, &uv_out, nullptr);
    CHECK(out.face_count() < faces_in); // it actually simplified

    crd::containers::Array<V3>       pos_out(&alloc);
    crd::containers::Array<crd::u32> idx_out(&alloc);
    out.to_indexed(pos_out, idx_out);
    REQUIRE(uv_out.size() == pos_out.size() * 2U);

    // MEASURE OVER THE SURFACE, NOT AT THE VERTICES.
    // The first version of this gate compared UVs at the output VERTICES, and the
    // position-only arm WON - 8.5e-06 against 1.2e-03. Not because it was better:
    // on a flat grid the surviving vertices barely move, so a nearest-input lookup
    // returns a near-exact ORIGINAL UV. The gate was measuring the lookup, not the
    // LOD. Texture swimming is an error in the INTERPOLATED field ACROSS the
    // simplified triangles, and it is the choice of WHICH vertices survive that
    // decides whether a piecewise-linear field can still track the true one -
    // exactly the choice the attribute term buys.
    const auto surface_error = [](const crd::containers::Array<V3>&       vp,
                                  const crd::containers::Array<crd::u32>& vi,
                                  const crd::containers::Array<crd::f32>& vuv) {
        crd::f32 worst = 0.0F;
        // barycentric probes: the centroid and the three edge midpoints
        const crd::f32 bary[4][3] = {{1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F},
                                     {0.5F, 0.5F, 0.0F},
                                     {0.0F, 0.5F, 0.5F},
                                     {0.5F, 0.0F, 0.5F}};
        for (crd::usize t = 0; t + 2U < vi.size(); t += 3U)
        {
            const crd::u32 a = vi[t];
            const crd::u32 b = vi[t + 1U];
            const crd::u32 c = vi[t + 2U];
            for (const auto& w : bary)
            {
                const crd::f32 x = (w[0] * vp[a].x) + (w[1] * vp[b].x) + (w[2] * vp[c].x);
                const crd::f32 u = (w[0] * vuv[a * 2U]) + (w[1] * vuv[b * 2U]) + (w[2] * vuv[c * 2U]);
                const crd::f32 e = crd::math::abs(u - (x * x)); // against the exact field
                if (e > worst) { worst = e; }
            }
        }
        return worst;
    };
    const crd::f32 worst_attr = surface_error(pos_out, idx_out, uv_out);

    // -- arm B: position-only, attributes TRANSFERRED afterwards from the nearest
    //    input vertex - the approach this metric exists to beat, measured the
    //    SAME way so the comparison is about the LOD and nothing else --
    mp::HalfEdgeMesh<crd::f32>       out_b = mp::qem_decimate(in, opts, nullptr);
    crd::containers::Array<V3>       pos_b(&alloc);
    crd::containers::Array<crd::u32> idx_b(&alloc);
    out_b.to_indexed(pos_b, idx_b);
    crd::containers::Array<crd::f32> uv_b(&alloc);
    uv_b.resize(pos_b.size() * 2U, 0.0F);
    for (crd::usize v = 0; v < pos_b.size(); ++v)
    {
        crd::f32   best_d2 = 1.0e30F;
        crd::usize best_k  = 0;
        for (crd::usize k = 0; k < pos.size(); ++k)
        {
            const crd::f32 dx = pos[k].x - pos_b[v].x;
            const crd::f32 dy = pos[k].y - pos_b[v].y;
            const crd::f32 d2 = (dx * dx) + (dy * dy);
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best_k  = k;
            }
        }
        uv_b[v * 2U]        = uv[best_k * 2U];
        uv_b[(v * 2U) + 1U] = uv[(best_k * 2U) + 1U];
    }
    const crd::f32 worst_transfer = surface_error(pos_b, idx_b, uv_b);

    INFO("attribute-aware worst |u - x^2| = " << worst_attr << " vs transfer " << worst_transfer);
    CHECK(worst_attr < 0.02F);          // the INTERPOLATED field tracks the true one
    CHECK(worst_attr < worst_transfer); // ...and strictly better than fixing up afterwards
}

// The chain is only usable if it REPRODUCES: an asset cooked twice must produce
// the same LOD, or every content hash downstream is a lie.
TEST_CASE("REN-40-C1 GATE: attribute-aware decimation is byte-identical across repeats",
          "[geometry][mesh-processing][qem][ren40][lod]")
{
    crd::memory::TlsfAllocator       alloc(16U << 20U);
    constexpr crd::u32               n = 9U;
    crd::containers::Array<V3>       pos(&alloc);
    crd::containers::Array<crd::f32> uv(&alloc);
    crd::containers::Array<crd::u32> idx(&alloc);
    for (crd::u32 j = 0; j < n; ++j)
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            const auto x = static_cast<crd::f32>(i) * 0.125F;
            const auto y = static_cast<crd::f32>(j) * 0.125F;
            pos.push_back(V3{x, y, 0.15F * x * y}); // gently curved, so geometry has an opinion too
            uv.push_back(x * x);
            uv.push_back(y * y);
        }
    }
    for (crd::u32 j = 0; j + 1U < n; ++j)
    {
        for (crd::u32 i = 0; i + 1U < n; ++i)
        {
            const crd::u32 v0 = (j * n) + i;
            idx.push_back(v0);
            idx.push_back(v0 + 1U);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0);
            idx.push_back(v0 + n + 1U);
            idx.push_back(v0 + n);
        }
    }
    mp::HalfEdgeMesh<crd::f32> in(&alloc);
    REQUIRE(in.build_from(crd::containers::ConstSpan<V3>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<crd::u32>{idx.data(), idx.size()})
            == mp::BuildStatus::Ok);

    mp::QemDecimateOptions<crd::f32> opts{};
    opts.target_face_count = in.face_count() / 3U;
    opts.output_allocator  = &alloc;

    const auto run = [&](crd::containers::Array<crd::f32>& out_uv, crd::containers::Array<V3>& out_pos) {
        crd::containers::Array<crd::u32> tmp_idx(&alloc);
        auto                             m = mp::qem_decimate_attr<crd::f32, 2U>(in, uv.data(), opts, &out_uv, nullptr);
        m.to_indexed(out_pos, tmp_idx);
    };
    crd::containers::Array<crd::f32> uv1(&alloc);
    crd::containers::Array<V3>       p1(&alloc);
    crd::containers::Array<crd::f32> uv2(&alloc);
    crd::containers::Array<V3>       p2(&alloc);
    run(uv1, p1);
    run(uv2, p2);

    REQUIRE(uv1.size() == uv2.size());
    REQUIRE(p1.size() == p2.size());
    CHECK(std::memcmp(static_cast<const void*>(uv1.data()), static_cast<const void*>(uv2.data()),
                      uv1.size() * sizeof(crd::f32))
          == 0);
    CHECK(std::memcmp(static_cast<const void*>(p1.data()), static_cast<const void*>(p2.data()),
                      p1.size() * sizeof(V3))
          == 0);
}
