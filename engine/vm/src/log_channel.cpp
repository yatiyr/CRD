#include <crd/log/log_level.hpp>
#include <crd/vm/log_channel.hpp>

namespace crd::vm
{
CRD_DEFINE_LOG_CHANNEL(g_log_vm, "Vm", ::crd::log::LogLevel::Info)
} // namespace crd::vm
