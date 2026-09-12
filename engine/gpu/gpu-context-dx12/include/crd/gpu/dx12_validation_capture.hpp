#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::gpu
{
namespace detail { struct Dx12CaptureState; }

enum class Dx12ValidationReadiness : u8
{
    Ready,
    InvalidCapacity,
    AllocationFailed,
    DebugLayerUnavailable,
    StartedAfterDevice
};

enum class Dx12ValidationSeverity : u8 { Info, Warning, Error };
enum class Dx12ValidationIssue : u8
{
    None, StorageFilter, RetrievalFilter, StartupOverflow, StartupRead, QueryInfoQueue,
    RegisterCallback, UnregisterCallback, DeviceCapacity
};

struct Dx12ValidationMessage
{
    Dx12ValidationSeverity severity = Dx12ValidationSeverity::Info;
    u32 id = 0;
    u32 device = 0; // Registration identity, not an adapter/hardware qualification.
    bool truncated = false;
    char text[1024]{};
};

struct Dx12ValidationReport
{
    Dx12ValidationReadiness readiness = Dx12ValidationReadiness::AllocationFailed;
    u64 info = 0;
    u64 warnings = 0;
    u64 errors = 0;
    u64 dropped = 0;
    u64 truncated = 0;
    u64 instrumentation_failures = 0;
    Dx12ValidationIssue first_issue = Dx12ValidationIssue::None;
    i32 first_issue_result = 0;
    u64 first_issue_detail = 0;
    u64 device_creation_failures = 0;
    u64 execution_failures = 0;
    u64 contexts_started = 0;
    u64 contexts_finished = 0;
    u32 active_contexts = 0;
    u32 messages = 0;

    // A ready but unused capture never qualifies a workload. Inspect after destroying all participating contexts.
    [[nodiscard]] bool complete_and_silent() const noexcept
    {
        return readiness == Dx12ValidationReadiness::Ready && contexts_started != 0U && active_contexts == 0U
            && contexts_started == contexts_finished && warnings == 0U && errors == 0U && dropped == 0U
            && truncated == 0U && instrumentation_failures == 0U && device_creation_failures == 0U
            && execution_failures == 0U;
    }
};

// Construct BEFORE any participating context; destroy contexts/resources before checking the final report.
// Process-local debug enablement persists (D3D12 cannot safely toggle it with live devices). No registry settings
// are changed. All gpu-context-dx12 factories participate. Standalone kir-dx12 and external native devices require
// their own instrument; construct this capture before ANY native D3D12 device (unmanaged devices are not detected).
// Storage uses the supplied allocator only at construction/destruction; callbacks neither allocate nor call D3D.
// Concurrent report/message reads return copies. Separate captures can overlap; a late capture is explicitly ungated.
class Dx12ValidationCapture final
{
public:
    explicit Dx12ValidationCapture(memory::IAllocator* allocator, u32 capacity = 256U);
    ~Dx12ValidationCapture() noexcept;
    Dx12ValidationCapture(const Dx12ValidationCapture&) = delete;
    Dx12ValidationCapture& operator=(const Dx12ValidationCapture&) = delete;
    Dx12ValidationCapture(Dx12ValidationCapture&&) = delete;
    Dx12ValidationCapture& operator=(Dx12ValidationCapture&&) = delete;

    [[nodiscard]] Dx12ValidationReport report() const noexcept;
    [[nodiscard]] bool message(u32 index, Dx12ValidationMessage& output) const noexcept;

private:
    detail::Dx12CaptureState* m_state = nullptr;
    Dx12ValidationReadiness m_readiness = Dx12ValidationReadiness::AllocationFailed;
};
} // namespace crd::gpu
