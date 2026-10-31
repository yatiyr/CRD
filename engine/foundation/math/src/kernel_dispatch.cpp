// The process's kernel determinism mode (SIMD-DISPATCH, ADR-0134 §7).

#include <crd/math/kernel_dispatch.hpp>

#include <atomic>

namespace crd::math
{

namespace
{
// Relaxed: the store happens before every plan build the setting thread orders after it (its own, or a job it then
// submits); a build racing the store may read either mode.
std::atomic<DeterminismMode> g_kernel_determinism{DeterminismMode::Default};
} // namespace

DeterminismMode kernel_determinism() noexcept
{
    return g_kernel_determinism.load(std::memory_order_relaxed);
}

void set_kernel_determinism(DeterminismMode mode) noexcept
{
    g_kernel_determinism.store(mode, std::memory_order_relaxed);
}

} // namespace crd::math
