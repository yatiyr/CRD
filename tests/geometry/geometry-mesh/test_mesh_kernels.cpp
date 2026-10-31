// The geometry-mesh kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the portable
// level's hit, t and barycentric bits for the BVH raycast, over a triangle soup with leaves of one to many chunks of
// eight, duplicated triangles (equal t, the lower index wins) and rays with zero direction components, from outside
// and inside, culled and not; the public mesh_raycast_simd returns the selected level's hit.
// crd-geometry-mesh-kernels-at-<level> runs these [kernels] cases under each forced level.

#include "kernel_levels.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/geometry/mesh/mesh.hpp>
#include <crd/geometry/mesh/mesh_dispatch.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace gm = crd::geometry::mesh;
namespace gp = crd::geometry::primitives;
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::Vec3f;
using gm::MeshKernels;

namespace
{

using crd::test::kernel_level_runs_here;

struct Lcg
{
    u64 s;
    f32 uni(f32 lo, f32 hi) noexcept
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return lo + static_cast<f32>(static_cast<u32>(s >> 40)) * (1.0F / 16777216.0F) * (hi - lo);
    }
};

struct Soup
{
    crd::containers::Array<Vec3f> vertices;
    crd::containers::Array<u32> indices;
    explicit Soup(crd::memory::IAllocator* a) : vertices(a), indices(a) {}
};

// Small triangles spread over [-2, 2]^3, every seventh one repeated (the same vertices under a later index).
Soup make_soup(crd::memory::IAllocator* a, u32 triangles)
{
    Soup m{a};
    Lcg rng{0x7E57ULL};
    for (u32 t = 0; t < triangles; ++t)
    {
        const u32 base = static_cast<u32>(m.vertices.size());
        if (t % 7U == 6U)
        {
            const u32 prev = m.indices[m.indices.size() - 3U];
            m.indices.push_back(prev);
            m.indices.push_back(prev + 1U);
            m.indices.push_back(prev + 2U);
            continue;
        }
        const Vec3f c{rng.uni(-2.0F, 2.0F), rng.uni(-2.0F, 2.0F), rng.uni(-2.0F, 2.0F)};
        for (u32 k = 0; k < 3U; ++k)
        {
            m.vertices.push_back(
                Vec3f{c.x + rng.uni(-0.6F, 0.6F), c.y + rng.uni(-0.6F, 0.6F), c.z + rng.uni(-0.6F, 0.6F)});
        }
        m.indices.push_back(base);
        m.indices.push_back(base + 1U);
        m.indices.push_back(base + 2U);
    }
    return m;
}

struct Hit
{
    bool found;
    gm::MeshRayHitData data;
};

bool same(const Hit& a, const Hit& b)
{
    if (a.found != b.found)
    {
        return false;
    }
    if (!a.found)
    {
        return true;
    }
    return std::bit_cast<u32>(a.data.t) == std::bit_cast<u32>(b.data.t) && a.data.triangle == b.data.triangle &&
           std::bit_cast<u32>(a.data.bary[0]) == std::bit_cast<u32>(b.data.bary[0]) &&
           std::bit_cast<u32>(a.data.bary[1]) == std::bit_cast<u32>(b.data.bary[1]) &&
           std::bit_cast<u32>(a.data.bary[2]) == std::bit_cast<u32>(b.data.bary[2]);
}

// Rays from outside towards the soup and from inside it; every fourth one has zero direction components.
gp::Ray3<f32> make_ray(Lcg& rng, usize i)
{
    const f32 reach = i % 3U == 0U ? 1.0F : 6.0F;
    const Vec3f o{rng.uni(-reach, reach), rng.uni(-reach, reach), rng.uni(-reach, reach)};
    Vec3f d{rng.uni(-1.0F, 1.0F) - o.x * 0.1F, rng.uni(-1.0F, 1.0F) - o.y * 0.1F, rng.uni(-1.0F, 1.0F) - o.z * 0.1F};
    if (i % 4U == 1U)
    {
        d.y = 0.0F;
    }
    if (i % 8U == 3U)
    {
        d.x = 0.0F;
        d.z = -0.0F;
    }
    return gp::Ray3<f32>{o, d};
}

} // namespace

TEST_CASE("mesh kernels: the archive holds portable and each body level the build compiles", "[geometry][kernels]")
{
    const auto levels = gm::mesh_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == crd::CpuLevel::Portable);
    bool avx2 = false;
    bool avx512 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        avx2 = avx2 || levels[i].level == crd::CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == crd::CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(crd::CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(crd::CpuLevel::Avx512)));
}

TEST_CASE("mesh kernels: every level the host runs gives the portable raycast bits", "[geometry][kernels]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const Soup soup = make_soup(&alloc, 420U);
    const gm::TriangleMeshViewf view{crd::containers::ConstSpan<Vec3f>{soup.vertices.data(), soup.vertices.size()},
                                     crd::containers::ConstSpan<u32>{soup.indices.data(), soup.indices.size()}};
    const gm::TriangleMeshBvh bvh = gm::build_triangle_mesh_bvh(view, &alloc);
    const gm::MeshBvhView mesh{reinterpret_cast<const unsigned char*>(view.vertices.data()), view.indices.data(),
                               reinterpret_cast<const unsigned char*>(bvh.tree.nodes().data()),
                               bvh.tree.prim_indices().data(), bvh.tree.root()};
    const MeshKernels& ref = *gm::mesh_kernel_levels()[0].table;
    usize ran = 0;
    for (const KernelLevel<MeshKernels>& level : gm::mesh_kernel_levels())
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        Lcg rng{0xBEEFULL};
        usize bad = 0;
        usize hits = 0;
        for (usize i = 0; i < 2000U; ++i)
        {
            const gp::Ray3<f32> ray = make_ray(rng, i);
            const gp::RaySlab slab = gp::ray_slab(ray, gp::precompute_ray_aabb(ray));
            const f32 tmax = i % 5U == 0U ? 3.0F : std::numeric_limits<f32>::infinity();
            const bool cull = i % 2U == 0U;
            Hit got{};
            Hit want{};
            got.found = level.table->raycast_mt8(mesh, slab, tmax, cull, got.data);
            want.found = ref.raycast_mt8(mesh, slab, tmax, cull, want.data);
            bad += same(got, want) ? 0U : 1U;
            hits += want.found ? 1U : 0U;
        }
        CHECK(bad == 0U);
        CHECK(hits > 200U); // the corpus hits often enough to compare
    }
    CHECK(ran >= 1U); // portable runs everywhere

    // The public raycast runs the selected level.
    Lcg rng{0xC0DEULL};
    usize public_bad = 0;
    for (usize i = 0; i < 200U; ++i)
    {
        const gp::Ray3<f32> ray = make_ray(rng, i);
        gm::MeshRayHitData data{};
        const bool found = gm::mesh_kernels().table->raycast_mt8(mesh, gp::ray_slab(ray, gp::precompute_ray_aabb(ray)),
                                                                 std::numeric_limits<f32>::infinity(), false, data);
        const auto hit = gm::mesh_raycast_simd(view, bvh, ray);
        public_bad += found == hit.has_value() ? 0U : 1U;
        if (found && hit.has_value())
        {
            public_bad += std::bit_cast<u32>(hit->t) == std::bit_cast<u32>(data.t) ? 0U : 1U;
            public_bad += hit->payload.tri == data.triangle ? 0U : 1U;
        }
    }
    CHECK(public_bad == 0U);
}

TEST_CASE("mesh kernels: the process runs the level its CPU selection resolves", "[geometry][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const KernelLevel<MeshKernels>& chosen = gm::mesh_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::select_kernel_level<MeshKernels>(gm::mesh_kernel_levels(), s.selected, s.kernel_features,
                                                            crd::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
}
