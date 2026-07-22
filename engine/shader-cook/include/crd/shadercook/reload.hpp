// crd-shader-cook — D-007 D5 (ADR-0104): HOT-RELOAD. The last link of the deploy chain — an edit to a shader graph is recooked
// and its live pipeline atomically hot-swapped, in the SAME context, with no restart.
//
// `ReloadableCompute` holds one live compute pipeline. `reload(graph)` recooks and content-hashes the IR (the D2 cache key): an
// UNCHANGED graph is a no-op; a CHANGED one builds the new pipeline from the freshly cooked bytecode and swaps it in, RETIRING
// the previous pipeline for one generation so in-flight GPU work never dangles (a real renderer drains its frames-in-flight
// before the next reload). The pipeline is built through a caller-supplied create-callback, so this stays backend-agnostic
// (the Vulkan/DX12 context provides create_pipeline_from_spirv / _from_dxil).
#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/compute.hpp> // ComputePipeline
#include <crd/kir/ckir.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/shadercook/cook.hpp>

#include <memory>

namespace crd::shadercook
{

// Build a compute pipeline from cooked bytecode (backend-specific): the cooked SPIR-V/DXIL blob + binding count → pipeline.
// `user` is opaque caller context (e.g. the compute context whose create_pipeline_from_spirv is invoked).
using PipelineCreateFn =
    std::unique_ptr<crd::gpu::ComputePipeline> (*)(crd::containers::ConstSpan<crd::u8> code, int n_bindings, void* user);

class ReloadableCompute
{
public:
    explicit ReloadableCompute(crd::memory::IAllocator* a) : m_alloc(a), m_crdr(a) {}

    struct Status
    {
        bool ok      = false; // the recook succeeded
        bool changed = false; // the IR hash differed ⇒ a new pipeline was built + swapped in
    };

    // Recook `g/e` for `backend`; if the content hash changed since the last successful load, build the new pipeline (via
    // `create`, from the bundle's cooked bytecode + IR-reflection binding count) and atomically swap it in.
    [[nodiscard]] Status reload(
        const crd::kir::KGraph& g, const crd::kir::KEntry& e, crd::containers::StringView name, CookBackend backend,
        PipelineCreateFn create, void* user);

    [[nodiscard]] crd::gpu::ComputePipeline* pipeline() const noexcept { return m_current.get(); } // the live pipeline
    [[nodiscard]] crd::u32                   generation() const noexcept { return m_generation; }  // ++ on each swap

private:
    crd::memory::IAllocator*                   m_alloc;
    crd::resources::ResourceId                 m_hash{}; // content hash of the loaded graph (null ⇒ nothing loaded yet)
    std::unique_ptr<crd::gpu::ComputePipeline> m_current;
    std::unique_ptr<crd::gpu::ComputePipeline> m_retired; // previous pipeline, kept one generation (in-flight safety)
    crd::containers::Array<crd::u8>            m_crdr;     // the live cooked bundle (owns the bytecode the pipeline was built from)
    crd::u32                                   m_generation = 0;
};

} // namespace crd::shadercook
