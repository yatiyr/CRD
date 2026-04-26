#include <crd/containers/log_channel.hpp>

// NOTE: the actual `CRD_DEFINE_LOG_CHANNEL(g_log_containers, ...)` lives in
// crd-log (see engine/log/src/log_channels_first_party.cpp). Moving it there
// breaks what would otherwise be a circular module dependency:
//
//     crd-log -> crd-containers (for RingBufferSink storage migration in v1d)
//     crd-containers -> crd-log (for g_log_containers definition)
//
// Headers in crd-containers still DECLARE the channel (extern), and any TU
// that includes <crd/containers/containers.hpp> can log to it. The symbol is
// resolved at link time by whoever pulls in crd-log.
//
// This file remains in crd-containers solely to anchor the per-TU
// force-link variable defined in containers.hpp. Same trick we use for
// string.cpp.

namespace crd::containers
{
int force_link_log_channel() noexcept
{
    return 0;
}
} // namespace crd::containers
