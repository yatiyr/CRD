// crd-draw -- thread-local active buffer (Phase 3.1 v1a-draw d2-curbuf,
// ADR-0066 sec 19.3).
//
// Single thread-local pointer slot. Inert when nullptr. Each thread has
// its own slot; setting in thread A doesn't affect thread B.

#include <crd/draw/active_buffer.hpp>

namespace crd::draw
{
namespace
{
thread_local RenderBuffer* tl_active_buffer = nullptr;
}

void set_active_buffer(RenderBuffer* buf) noexcept
{
    tl_active_buffer = buf;
}

RenderBuffer* active_buffer() noexcept
{
    return tl_active_buffer;
}

} // namespace crd::draw
