#include <crd/core/core.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/memory.hpp>

#include <iostream>

CRD_DEFINE_LOG_CHANNEL(g_log_runtime, "Runtime", crd::log::LogLevel::Trace)

namespace
{
// The real body. `main` below is a catch-all wrapper: an exception escaping main is std::terminate with
// no diagnostic, and the sink construction / iostream writes here can all throw. Exit non-zero instead.
// (bugprone-exception-escape.)
int run()
{
    const crd::u32 version_major = CRD_VERSION_MAJOR;
    const crd::u32 version_minor = CRD_VERSION_MINOR;
    const crd::u32 version_patch = CRD_VERSION_PATCH;

    crd::log::LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.flush_on_critical = true;
    crd::log::init(cfg);

    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::add_sink(
        std::make_unique<crd::log::FileSink>("engine.log", /*max_bytes*/ 1ULL * 1024ULL * 1024ULL, /*max_files*/ 3));

    CRD_LOG_INFO(g_log_runtime, "CRD Engine v{}.{}.{} startup skeleton", version_major, version_minor, version_patch);
    CRD_LOG_INFO(g_log_runtime, "platform={} compiler={} arch={}", crd::platform_name(), crd::compiler_name(),
                 crd::arch_name());

    auto* heap = crd::memory::default_allocator();
    CRD_LOG_INFO(g_log_runtime, "default allocator='{}' ptr={:p}", heap->name(), static_cast<void*>(heap));
    CRD_LOG_INFO(g_log_runtime, "run smoke examples via smoke_log / smoke_memory / smoke_containers");

    crd::log::flush();
    crd::log::shutdown();

    std::cout << "(runtime exiting cleanly)\n";
    return 0;
}
} // namespace

int main()
{
    try
    {
        return run();
    }
    catch (...)
    {
        return 1;
    }
}
