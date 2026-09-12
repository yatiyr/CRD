// crd-shader-cook — D-007 D11 (ADR-0104): ASYNC PIPELINE WARMUP — the render-thread half of the deploy chain.
//
// Turning cooked bytecode into a live GPU pipeline is the driver's SPIR-V→ISA compile — the expensive step, even from a warm
// D4 VkPipelineCache. Done lazily on the render thread at first-use it stalls the frame (the infamous "shader compilation
// hitch"). This warms a SET of pipelines OFF the render thread on the engine's OWN fiber scheduler (crd-jobs — we reuse the
// job system, we do NOT spin a bespoke pool): `submit()` kicks ONE background job that builds every queued pipeline via the
// caller's create-callback (serialized — so the driver's shared pipeline cache needs no extra synchronisation), the caller
// keeps working, then `wait()`s. On completion the pipelines are hot and index-retrievable; a `VkPipeline` is not thread-
// affine, so one built on a worker binds on the render thread.
//
// Pairs with the rest of the chain: warm the D4 VkPipelineCache FIRST (⇒ the background compile is a cache hit), feed it the
// D8 variant container's unique bytecodes (the shipping form — each unique blob → one warm pipeline), key each request so a
// runtime lookup maps a variant key → its ready pipeline. Requires `jobs::init()`.
#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/gpu/compute.hpp>       // ComputePipeline
#include <crd/jobs/jobs.hpp>         // the fiber scheduler (parallel_for / wait)
#include <crd/memory/allocator.hpp>
#include <crd/shadercook/reload.hpp> // PipelineCreateFn (shared with the D5 hot-reloader)

#include <memory>

namespace crd::shadercook
{

// Warm a batch of compute pipelines on a background crd-jobs worker. Add requests, submit() (non-blocking), do other work,
// wait(). The stored `code` spans must OUTLIVE wait() (they are viewed, not copied — point them at the cooked bundle bytes /
// the variant container, which the caller already owns).
class AsyncPipelineWarmer
{
public:
    explicit AsyncPipelineWarmer(crd::memory::IAllocator* a) : m_code(a), m_nbind(a), m_key(a), m_pipes(a) {}
    ~AsyncPipelineWarmer()
    {
        if (m_counter != nullptr) { crd::jobs::wait(m_counter); } // never leak an unwaited job (jobs.hpp contract)
        for (crd::usize i = 0; i < m_pipes.size(); ++i) { delete m_pipes[i]; }
    }
    AsyncPipelineWarmer(const AsyncPipelineWarmer&)            = delete;
    AsyncPipelineWarmer& operator=(const AsyncPipelineWarmer&) = delete;
    AsyncPipelineWarmer(AsyncPipelineWarmer&&)                 = delete;
    AsyncPipelineWarmer& operator=(AsyncPipelineWarmer&&)      = delete;

    // Queue a cooked-SPIR-V blob to warm into a pipeline (n storage-buffer bindings). `key` is an opaque tag for lookup.
    void add(crd::containers::ConstSpan<crd::u8> code, int n_bindings, crd::u32 key = 0U)
    {
        m_code.push_back(code);
        m_nbind.push_back(n_bindings);
        m_key.push_back(key);
    }

    // Kick the background warm on crd-jobs (non-blocking). ONE job builds every queued pipeline in order via `create` — a
    // single worker touches the driver pipeline cache, so no extra sync is needed. Requires jobs::init() already called.
    void submit(PipelineCreateFn create, void* user)
    {
        m_create = create;
        m_user   = user;
        m_warmed = 0U;
        m_pipes.resize(m_code.size(), nullptr);
        const crd::u32 count = static_cast<crd::u32>(m_code.size());
        if (count == 0U) { m_counter = nullptr; return; }
        m_counter = crd::jobs::parallel_for(count, 1U, [self = this](crd::u32 b, crd::u32 e) {
            for (crd::u32 i = b; i < e; ++i)
            {
                std::unique_ptr<crd::gpu::ComputePipeline> p = self->m_create(self->m_code[i], self->m_nbind[i], self->m_user);
                if (p != nullptr) { self->m_pipes[i] = p.release(); ++self->m_warmed; } // disjoint index i — single job, no race
            }
        });
    }

    void wait() // block until the batch is hot (the crd-jobs happens-before makes the results visible on this thread)
    {
        if (m_counter != nullptr) { crd::jobs::wait(m_counter); m_counter = nullptr; }
    }
    [[nodiscard]] bool     in_flight() const noexcept { return m_counter != nullptr; }
    [[nodiscard]] crd::u32 count() const noexcept { return static_cast<crd::u32>(m_pipes.size()); }
    [[nodiscard]] crd::u32 warmed() const noexcept { return m_warmed; } // # successfully built (valid after wait())
    [[nodiscard]] crd::gpu::ComputePipeline* pipeline(crd::u32 i) const noexcept
    {
        return i < m_pipes.size() ? m_pipes[i] : nullptr;
    }
    // The ready pipeline for a queued `key` (linear scan — a warmup set is small), or nullptr.
    [[nodiscard]] crd::gpu::ComputePipeline* pipeline_for_key(crd::u32 key) const noexcept
    {
        for (crd::usize i = 0; i < m_key.size(); ++i) { if (m_key[i] == key) { return m_pipes[i]; } }
        return nullptr;
    }

private:
    crd::containers::Array<crd::containers::ConstSpan<crd::u8>> m_code;  // cooked bytecode per request (viewed, not owned)
    crd::containers::Array<int>                                m_nbind; // binding count per request
    crd::containers::Array<crd::u32>                           m_key;   // lookup tag per request
    crd::containers::Array<crd::gpu::ComputePipeline*>         m_pipes; // the warmed pipelines (owned; deleted in dtor)
    PipelineCreateFn                                           m_create  = nullptr;
    void*                                                      m_user    = nullptr;
    crd::jobs::Counter*                                        m_counter = nullptr;
    crd::u32                                                   m_warmed  = 0U;
};

} // namespace crd::shadercook
