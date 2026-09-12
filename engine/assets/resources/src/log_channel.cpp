#include <crd/resources/log_channel.hpp>
#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>

namespace crd::resources
{
CRD_DEFINE_LOG_CHANNEL(g_log_resources, "Resources", ::crd::log::LogLevel::Info)
} // namespace crd::resources
