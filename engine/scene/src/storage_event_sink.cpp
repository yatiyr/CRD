#include <crd/scene/storage_event_sink.hpp>

namespace crd::scene
{

IStorageEventSink* NullStorageEventSink::instance() noexcept
{
    // Block-scoped static — initialised once at first call, persists for the
    // life of the process. A single shared no-op sink is the cheapest possible
    // default: every storage that hasn't been wired with a real sink simply
    // dispatches into here.
    static NullStorageEventSink s;
    return &s;
}

} // namespace crd::scene
