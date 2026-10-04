#pragma once

// DIAG.7b(c): the removal-time half of DRED. After a device is removed, read what DRED recorded -- the removal HRESULT,
// the device state, the auto-breadcrumb history of in-flight command lists and the page-fault allocation report -- into
// a fixed-capacity Cerid report. No allocation: it runs on the failure path. Each query keeps its own outcome, so a
// failed or unavailable query leaves the rest of the report intact (last-known state survives failed queries), and
// "not available" (DRED off, interface absent, no fault recorded) is never confused with "failed" or with "empty".
// D3D12 types stay behind this module; the report carries Cerid enums, raw HRESULT values and narrow debug names.

#include <crd/core/types.hpp>
#include <crd/gpu/object_identity.hpp> // DIAG.7b(e): Cerid identities parsed from DRED names

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace crd::gpu::detail
{
enum class Dx12DredQuery : u8
{
    NotRun,       // the reader did not reach this query
    Ok,           // the query returned data (possibly an empty list)
    NotAvailable, // DRED off for this device, interface absent, or nothing recorded yet (NOT_CURRENTLY_AVAILABLE)
    Failed,       // any other failure HRESULT (kept in the *_result field)
};

enum class Dx12DredDeviceState : u8
{
    NotQueried, // ID3D12DeviceRemovedExtendedData2 absent
    Unknown,
    Hung,
    Fault,
    PageFault,
};

inline constexpr u32 kDx12DredNameBytes  = 96U;
inline constexpr u32 kDx12DredMaxNodes   = 8U;
inline constexpr u32 kDx12DredMaxAllocs  = 8U;

struct Dx12DredBreadcrumbNode
{
    char command_list[kDx12DredNameBytes]  = {}; // debug name of the command list (a Cerid token when named), truncated
    char command_queue[kDx12DredNameBytes] = {}; // debug name of the queue it was submitted to
    ObjectIdentity list_identity{};  // (e) the Cerid identity in the list name (invalid when the list is unnamed)
    ObjectIdentity queue_identity{}; // (e) the Cerid identity in the queue name
    u32  op_count                          = 0;  // breadcrumb operations recorded for the list
    u32  last_completed                    = 0;  // ops the GPU completed (the value DRED's last breadcrumb holds)
    bool has_last_completed                = false;
};

struct Dx12DredAllocation
{
    char name[kDx12DredNameBytes] = {}; // object debug name (a Cerid token when named), truncated
    u32  type                     = 0;  // D3D12_DRED_ALLOCATION_TYPE value
    ObjectIdentity identity{};          // (e) the Cerid identity in the object name (live or retired in the registry)
};

struct Dx12DredReport
{
    i32                 removal_reason = 0; // HRESULT from GetDeviceRemovedReason (S_OK = not removed)
    Dx12DredDeviceState device_state   = Dx12DredDeviceState::NotQueried;

    Dx12DredQuery          breadcrumbs        = Dx12DredQuery::NotRun;
    i32                    breadcrumbs_result = 0;
    u32                    node_count         = 0; // nodes in DRED's list (may exceed what is stored)
    u32                    nodes_stored       = 0;
    Dx12DredBreadcrumbNode nodes[kDx12DredMaxNodes];

    Dx12DredQuery      page_fault        = Dx12DredQuery::NotRun;
    i32                page_fault_result = 0;
    u64                page_fault_va     = 0;
    u32                existing_count    = 0; // allocations overlapping the faulting VA that still exist
    u32                freed_count       = 0; // ...and that were recently freed
    u32                allocations_stored = 0;
    Dx12DredAllocation allocations[kDx12DredMaxAllocs]; // existing first, then recently freed
};

// The parsing halves of the read, exposed so the mapping from DRED's output lists to the report (names, identities,
// bounds) is testable with constructed lists: a forced RemoveDevice yields empty lists, and real data needs a real
// device fault. Both append to `report` (node/allocation counts keep counting past the stored capacity).
void dx12_dred_fill_breadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE1* head, Dx12DredReport& report) noexcept;
void dx12_dred_fill_page_fault(const D3D12_DRED_PAGE_FAULT_OUTPUT1& output, Dx12DredReport& report) noexcept;

// Read DRED from `device` (null -> an all-NotRun report). Never throws, never allocates; safe after removal.
[[nodiscard]] Dx12DredReport dx12_read_dred(ID3D12Device* device) noexcept;

// DIAG.7b(d): who removed the device. The acceptance separates simulated error-path coverage from an actual device
// loss, so every recorded removal carries its origin instead of being one undifferentiated "device lost".
enum class Dx12RemovalOrigin : u8
{
    None,         // nothing recorded yet
    Observed,     // the runtime/driver had already removed the device when the engine looked (an actual device loss)
    EngineForced, // the engine called RemoveDevice itself after a failed completion (timeout, signal/submit failure)
};

struct Dx12RemovalRecord
{
    Dx12RemovalOrigin origin   = Dx12RemovalOrigin::None;
    u64               sequence = 0; // 1-based count of removals recorded in this process
    Dx12DredReport    dred{};
    // (f) The crash::WriteResult of the removal bundle written for this record (NotInstalled when crash capture is
    // off). Set on the stored record after the write; the copy inside the bundle carries the value before it.
    u32 bundle_result = 0;
};

// DIAG.7b(f): the removal bundle. Every recorded removal is also written, when crash capture is installed, as a live
// dump of kind DeviceRemoved (gpu_*.dmp) whose evidence stream holds this header followed by the Dx12RemovalRecord. The
// reader refuses a stream whose magic, version or size does not match this build, rather than misreading it.
inline constexpr u32 kDx12RemovalBundleMagic   = 0x52445243U; // 'CRDR'
inline constexpr u32 kDx12RemovalBundleVersion = 1U;

struct Dx12RemovalBundleHeader
{
    u32 magic        = kDx12RemovalBundleMagic;
    u32 version      = kDx12RemovalBundleVersion;
    u32 record_bytes = 0;
    u32 reserved     = 0;
};

enum class Dx12BundleRead : u8
{
    Ok,
    NoStream,   // the dump has no Cerid evidence stream (not a removal bundle)
    BadMagic,   // the evidence stream is not a removal bundle (e.g. a hang dump's evidence)
    BadVersion, // a removal bundle of another format version
    BadSize,    // the size does not match this build's record
};

// Read a removal bundle back from a dump file. Never throws; `out` is written only on Ok.
[[nodiscard]] Dx12BundleRead dx12_read_removal_bundle(const wchar_t* dump_path, Dx12RemovalRecord& out) noexcept;

// The completion helpers call this whenever they stop a failed device: read DRED and keep it as the process's
// last-known removal, which survives the provider's shutdown (it is not owned by any context).
void dx12_record_removal(ID3D12Device* device, Dx12RemovalOrigin origin) noexcept;
[[nodiscard]] Dx12RemovalRecord dx12_last_removal() noexcept;
} // namespace crd::gpu::detail
