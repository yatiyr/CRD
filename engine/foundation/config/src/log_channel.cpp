#include <crd/config/log_channel.hpp>
#include <crd/log/log_level.hpp>

namespace crd::config
{
CRD_DEFINE_LOG_CHANNEL(g_log_config, "Config", ::crd::log::LogLevel::Info)
} // namespace crd::config
