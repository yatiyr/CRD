#include <crd/imgui/log_channel.hpp>
#include <crd/log/log_level.hpp>

namespace crd::imgui
{
CRD_DEFINE_LOG_CHANNEL(g_log_imgui, "ImGui", ::crd::log::LogLevel::Info)
} // namespace crd::imgui
