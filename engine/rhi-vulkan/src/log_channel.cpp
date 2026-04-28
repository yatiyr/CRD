#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>

namespace crd::rhi::detail
{
CRD_DEFINE_LOG_CHANNEL(g_log_rhi_vulkan, "RHI-Vulkan", ::crd::log::LogLevel::Info)
}
