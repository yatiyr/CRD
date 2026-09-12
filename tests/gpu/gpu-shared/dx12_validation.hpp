#pragma once

#include <crd/gpu/dx12_validation_capture.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

namespace crd::gpu_test
{
inline void print_dx12_validation(const gpu::Dx12ValidationCapture& capture)
{
    const auto report = capture.report();
    std::printf("DX12 validation: ready=%u info=%llu warnings=%llu errors=%llu dropped=%llu truncated=%llu "
                "instrumentation=%llu execution=%llu contexts=%llu/%llu active=%u\n",
                static_cast<unsigned>(report.readiness), static_cast<unsigned long long>(report.info),
                static_cast<unsigned long long>(report.warnings), static_cast<unsigned long long>(report.errors),
                static_cast<unsigned long long>(report.dropped), static_cast<unsigned long long>(report.truncated),
                static_cast<unsigned long long>(report.instrumentation_failures),
                static_cast<unsigned long long>(report.execution_failures),
                static_cast<unsigned long long>(report.contexts_finished),
                static_cast<unsigned long long>(report.contexts_started), report.active_contexts);
    for (u32 index = 0; index < report.messages; ++index)
    {
        gpu::Dx12ValidationMessage message;
        if (capture.message(index, message) && message.severity != gpu::Dx12ValidationSeverity::Info)
        {
            std::printf("DX12 validation device=%u id=%u: %s\n", message.device, message.id, message.text);
        }
    }
}

// The callback owns every context/resource, so its return or exception unwinds them before inspection.
// Preserve diagnostics on failed REQUIRE and SKIP; a skipped workload remains skipped, never qualified.
template <class Workload>
void qualify_dx12_workload(memory::IAllocator* allocator, const Workload& workload)
{
    gpu::Dx12ValidationCapture capture(allocator, 4096U);
    REQUIRE(capture.report().readiness == gpu::Dx12ValidationReadiness::Ready);
    try { workload(); }
    catch (...)
    {
        print_dx12_validation(capture);
        throw;
    }
    print_dx12_validation(capture);
    REQUIRE(capture.report().complete_and_silent());
}
} // namespace crd::gpu_test
