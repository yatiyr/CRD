#include <crd/log/log_level.hpp>
#include <crd/platform/log_channel.hpp>

namespace crd::platform
{
CRD_DEFINE_LOG_CHANNEL(g_log_platform, "Platform", ::crd::log::LogLevel::Info)
} // namespace crd::platform
