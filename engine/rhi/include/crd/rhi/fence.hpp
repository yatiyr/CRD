#pragma once

namespace crd::rhi
{
// Phase 3.0 v1o1 — RHI fence primitive (ADR-0061 §"Layer 1").
//
// Fence is a one-shot GPU → CPU synchronization handle. It is created
// in the unsignalled state, gets signalled when the queue submission it
// was attached to completes, and can be re-armed via reset() for reuse.
//
// Use case: paired with the non-waiting `Queue::submit(CommandBuffer&,
// Fence&)` overload so callers can submit GPU work and poll for its
// completion across frames without blocking. The async GPU upload
// contract (`UploadHandle` in v1o2) is the first consumer.
//
// Lifetime: created via `Device::create_fence()`. The Device owns the
// underlying VkFence (or backend equivalent) and destroys it on the
// returned unique_ptr's destruction. Fences MUST NOT outlive their
// Device.
//
// Threading: not thread-safe. Pair one fence with one in-flight
// submission at a time; do not call wait()/is_signaled()/reset() from
// multiple threads concurrently.
class Fence
{
public:
    virtual ~Fence() = default;

    // Non-blocking. Returns true once the GPU work this fence is
    // attached to has completed (or the fence has never been used).
    [[nodiscard]] virtual bool is_signaled() const noexcept = 0;

    // Blocking. Returns when the fence is signalled. Calling on an
    // unattached / freshly-reset fence will block forever — only call
    // after a `Queue::submit(cmd, *this)` has been issued.
    virtual void wait() = 0;

    // Re-arm the fence for reuse: clears the signalled state. Safe to
    // call regardless of current state.
    virtual void reset() = 0;

    Fence(const Fence&)            = delete;
    Fence& operator=(const Fence&) = delete;
    Fence(Fence&&)                 = delete;
    Fence& operator=(Fence&&)      = delete;

protected:
    Fence() noexcept = default;
};

} // namespace crd::rhi
