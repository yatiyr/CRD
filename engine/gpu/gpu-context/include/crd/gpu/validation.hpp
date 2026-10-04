#pragma once

// DIAG.7a(b): the common (backend-agnostic) GPU-validation vocabulary.
//
// ADR-0133 DG11/DG12: there is ONE validation collector. The DX12 and Vulkan captures both project their native
// severities and counters onto THIS contract, and DIAG.7b/7c EXTEND it rather than forking a parallel collector.
// No vendor types (`D3D12_*` / `Vk*`) appear here or in any public module -- each backend translates in its own .cpp
// (Vulkan's `to_severity`, DX12's `to_common`).

#include <crd/core/types.hpp>

namespace crd::gpu
{

// Common validation severity. This is the canonical home for the name `crd::gpu::ValidationSeverity` (the Vulkan
// capture used to define it locally and now includes this header instead -- one name, one definition). The three
// levels every debug layer reports.
enum class ValidationSeverity : crd::u8
{
    Info,
    Warning,
    Error,
};

[[nodiscard]] constexpr const char* to_string(ValidationSeverity s) noexcept
{
    switch (s)
    {
    case ValidationSeverity::Error:   return "error";
    case ValidationSeverity::Warning: return "warning";
    case ValidationSeverity::Info:    return "info";
    }
    return "info";
}

// A backend-agnostic validation report: the message-accounting core both captures project onto, so a gate can judge
// "did this GPU work validate cleanly" without knowing which backend produced it.
//
// clean() encodes the DIAG.7a acceptance rule **"dropped validation messages are not a clean run"**: a run is clean
// ONLY if it saw no error, no warning, AND lost nothing (dropped/truncated == 0) and the instrument itself did not
// fail. A capture that overflowed and dropped messages must NOT read as clean -- the lost messages might have been
// the errors. (Lifecycle terms such as DX12's contexts_started == contexts_finished belong to sub-unit (e); this
// struct is the accounting core that (b) unifies. Cerid resource/program/pass identity arrives with (c)/(d).)
struct ValidationReport
{
    crd::u64 info                     = 0;
    crd::u64 warning                  = 0;
    crd::u64 error                    = 0;
    crd::u64 dropped                  = 0; // messages the capture could not store (capacity overflow) -- lost
    crd::u64 truncated                = 0; // messages stored but with their text cut -- partially lost
    crd::u64 instrumentation_failures = 0; // the capture / layer itself failed (e.g. startup overflow)

    [[nodiscard]] bool clean() const noexcept
    {
        return error == 0 && warning == 0 && dropped == 0 && truncated == 0 && instrumentation_failures == 0;
    }
};

// DIAG.7a(f): per-mode validation ACTIVATION. The design (runtime-diagnostics.md:353) requires "actual activation and
// unsupported reasons for core, synchronization and GPU-assisted modes without importing vendor types into public
// modules." These three modes and their activation state are the vendor-free vocabulary; each backend maps them onto its
// native mechanism in its own .cpp (Vulkan: VkValidationFeaturesEXT; DX12: the debug layer + GPU-based validation).
enum class ValidationMode : crd::u8
{
    Core            = 0, // the base validation layer / debug layer
    Synchronization = 1, // hazard (WAR/RAW/WAW) validation
    GpuAssisted     = 2, // shader-instrumented (bounds / descriptor) validation
};
inline constexpr crd::usize kValidationModeCount = 3U;

// Why a requested mode is not active. `None` == active (or, paired with a mode that was never requested, simply nothing
// to explain -- see `NotRequested`). A backend that has no such mode reports `BackendHasNoEquivalent` (e.g. DX12 has no
// synchronization-validation mode) -- a CORRECT report, not a gap.
enum class ValidationUnsupportedReason : crd::u8
{
    None = 0,               // the mode is active (reason to explain: none)
    NotRequested,           // the caller did not ask for this mode
    LayerAbsent,            // the validation/debug layer is not present
    ExtensionAbsent,        // the enabling extension is unavailable (e.g. VK_EXT_validation_features)
    FeatureAbsent,          // a required device feature is missing (e.g. GPU-assisted needs stores/atomics)
    BackendHasNoEquivalent, // this backend has no such mode
    DeviceRejected,         // creation was rejected with the mode requested
};

[[nodiscard]] constexpr const char* to_string(ValidationUnsupportedReason r) noexcept
{
    switch (r)
    {
    case ValidationUnsupportedReason::None:                   return "none";
    case ValidationUnsupportedReason::NotRequested:           return "not-requested";
    case ValidationUnsupportedReason::LayerAbsent:            return "layer-absent";
    case ValidationUnsupportedReason::ExtensionAbsent:        return "extension-absent";
    case ValidationUnsupportedReason::FeatureAbsent:          return "feature-absent";
    case ValidationUnsupportedReason::BackendHasNoEquivalent: return "backend-has-no-equivalent";
    case ValidationUnsupportedReason::DeviceRejected:         return "device-rejected";
    }
    return "none";
}

// The per-mode activation report a context exposes (IGpuContext::validation_activation()). Indexed by ValidationMode.
// `requested` = the caller asked for it; `active` = it is really on; `reason` = why it is off. The invariants
// `consistent()` pins are the (f) unit's device-free oracle.
struct ValidationActivation
{
    bool                        requested[kValidationModeCount] = {false, false, false};
    bool                        active[kValidationModeCount]    = {false, false, false};
    ValidationUnsupportedReason reason[kValidationModeCount]    = {ValidationUnsupportedReason::NotRequested,
                                                                  ValidationUnsupportedReason::NotRequested,
                                                                  ValidationUnsupportedReason::NotRequested};

    [[nodiscard]] bool is_requested(ValidationMode m) const noexcept { return requested[static_cast<crd::usize>(m)]; }
    [[nodiscard]] bool is_active(ValidationMode m) const noexcept { return active[static_cast<crd::usize>(m)]; }
    [[nodiscard]] ValidationUnsupportedReason unsupported_reason(ValidationMode m) const noexcept
    {
        return reason[static_cast<crd::usize>(m)];
    }

    // active => requested; active => reason None; !requested => reason NotRequested. A report violating these is a bug in
    // the backend that filled it, not a device state -- so the (f) test asserts this holds for every real context.
    [[nodiscard]] bool consistent() const noexcept
    {
        for (crd::usize i = 0; i < kValidationModeCount; ++i)
        {
            if (active[i] && !requested[i])
            {
                return false;
            }
            if (active[i] && reason[i] != ValidationUnsupportedReason::None)
            {
                return false;
            }
            if (!requested[i] && reason[i] != ValidationUnsupportedReason::NotRequested)
            {
                return false;
            }
        }
        return true;
    }
};

} // namespace crd::gpu
