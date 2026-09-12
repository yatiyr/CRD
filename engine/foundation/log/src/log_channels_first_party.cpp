// First-party log channels owned by other engine modules whose own .cpp
// files cannot reasonably define them (because doing so would create a
// circular module dependency on crd-log).
//
// The channel SYMBOLS are declared in their respective module headers
// (e.g. <crd/containers/log_channel.hpp> declares `g_log_containers`). The
// actual `CRD_DEFINE_LOG_CHANNEL(...)` lives here so the symbol is owned
// by crd-log itself.
//
// Dependency direction is now one-way:
//   crd-log -> crd-containers  (we include their declaration headers)
//   crd-containers -> crd-log  (DOES NOT EXIST — only header-only references
//                                to the macro, no link-time symbol)
//
// Add new entries below as more modules need their own channels.

#include <crd/containers/log_channel.hpp>
#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>

namespace crd::containers
{
CRD_DEFINE_LOG_CHANNEL(g_log_containers, "Containers", ::crd::log::LogLevel::Info)
} // namespace crd::containers

namespace crd::log
{
int force_link_first_party_channels() noexcept
{
    return 0;
}
} // namespace crd::log
