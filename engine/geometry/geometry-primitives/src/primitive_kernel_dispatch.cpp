// The geometry-primitives kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/geometry/primitives/primitive_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::geometry::primitives
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_PRIMITIVES: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody =
    (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_PRIMITIVES & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, PORTABLE), "the portable level is always built");

constexpr KernelLevel<PrimitiveKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kPrimitiveKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, SSE2)
    {crd::CpuLevel::Sse2, &kPrimitiveKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_PRIMITIVES, AVX2)
    {crd::CpuLevel::Avx2, &kPrimitiveKernelsAvx2, {}},
#endif
};

} // namespace

std::span<const KernelLevel<PrimitiveKernels>> primitive_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<PrimitiveKernels>& primitive_kernels() noexcept
{
    static const KernelLevel<PrimitiveKernels>& selected = crd::math::process_kernel_level<PrimitiveKernels>(kLevels);
    return selected;
}

RaySlab ray_slab(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre) noexcept
{
    RaySlab s{};
    s.origin[0] = ray.origin.x;
    s.origin[1] = ray.origin.y;
    s.origin[2] = ray.origin.z;
    s.direction[0] = ray.direction.x;
    s.direction[1] = ray.direction.y;
    s.direction[2] = ray.direction.z;
    s.inv_dir[0] = pre.inv_dir.x;
    s.inv_dir[1] = pre.inv_dir.y;
    s.inv_dir[2] = pre.inv_dir.z;
    for (crd::usize axis = 0; axis < 3U; ++axis)
    {
        s.sign[axis] = pre.sign[axis] != 0U ? 1U : 0U;
    }
    s.pad = ray_aabb_robust_pad<crd::f32>();
    return s;
}

} // namespace crd::geometry::primitives
