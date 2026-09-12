#pragma once

// CEIR-30b-2b-1 — a shared HostKernelResolveFn (and the mold for the CUDA VizDispatch loader): map a compute.dispatch `kernel`
// symbol to its committed assets/ckir/<name>.ckir and `ckir_read` it into a KGraph + KEntry. The ENGINE names no asset path
// (execute_tensor_pipeline_host takes this as a HostRunOptions.kernel callback); the TEST owns the load. `user` = the
// crd::memory::IAllocator* the source buffer + graph are read into. Returns false on an unknown symbol or a bad read
// (⇒ the stage is ExecuteError::UnresolvedKernel). ⛔ paths are compile-time literals (CRD_REPO_DIR is a macro the including TU
// defines) — no runtime path building, no std::string. Symbols mirror the CUDA resolver's set (relu / viz_magnitude / viz_normalize).

#include <crd/kir/ckir.hpp>       // KGraph / KEntry
#include <crd/kir/ckir_asset.hpp> // ckir_read

#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/memory.hpp>

#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "." // NOLINT(cppcoreguidelines-macro-usage): a build-injected path; the fallback keeps the header standalone-parseable
#endif

namespace crd::tests
{
// A HostKernelResolveFn: `user` is a crd::memory::IAllocator*. Loads the authored .ckir for `symbol` into `g` + `entry`.
inline bool resolve_ckir_asset(crd::containers::StringView symbol, crd::kir::KGraph& g, crd::kir::KEntry& entry, void* user)
{
    auto* const alloc = static_cast<crd::memory::IAllocator*>(user);
    const char* path  = nullptr;
    if (symbol == crd::containers::StringView("relu")) { path = CRD_REPO_DIR "/assets/ckir/relu.ckir"; }
    else if (symbol == crd::containers::StringView("viz_magnitude")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_magnitude.ckir"; }
    else if (symbol == crd::containers::StringView("viz_normalize")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_normalize.ckir"; }
    if (path == nullptr) { return false; }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { return false; }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) { return false; }
    f.seekg(0);
    crd::containers::Array<char> src(alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    return crd::kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, entry).ok;
}
} // namespace crd::tests
