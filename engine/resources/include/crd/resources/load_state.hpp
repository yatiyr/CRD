#pragma once

#include <crd/core/types.hpp>

namespace crd::resources
{

enum class LoadState : crd::u8
{
    Unloaded,    // default-constructed handle; load has not been requested
    Queued,      // submitted to the load queue, not started (v1d+)
    Loading,     // loader job is running (v1d+)
    Ready,       // payload valid; get() returns non-null
    Placeholder, // loader returned a soft fallback after failure
    Failed,      // hard failure; get() returns nullptr
};

} // namespace crd::resources
