#pragma once

// Umbrella header. Include this from user code:
//
//     #include <crd/log/log.hpp>
//     CRD_DECLARE_LOG_CHANNEL(g_log_my_subsystem);  // optional in headers
//     // ... in one .cpp:
//     CRD_DEFINE_LOG_CHANNEL(g_log_my_subsystem, "MySubsystem", crd::log::LogLevel::Info);
//
//     CRD_LOG_INFO(g_log_my_subsystem, "loaded {} items in {} ms", n, ms);

#include <crd/log/log_channel.hpp>
#include <crd/log/log_level.hpp>
#include <crd/log/log_macros.hpp>
#include <crd/log/log_record.hpp>
#include <crd/log/log_sink.hpp>
#include <crd/log/logger.hpp>
#include <crd/log/sinks/console_sink.hpp>
#include <crd/log/sinks/debugger_sink.hpp>
#include <crd/log/sinks/file_sink.hpp>
#include <crd/log/sinks/null_sink.hpp>
#include <crd/log/sinks/ring_buffer_sink.hpp>

namespace crd::log
{
// A built-in channel for log-system-internal messages and tests.
// Subsystems should declare their own channels rather than reuse this one.
extern Channel g_log_default;
} // namespace crd::log
