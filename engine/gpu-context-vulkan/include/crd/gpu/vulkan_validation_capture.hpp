#pragma once

// ---------------------------------------------------------------------------
// crd::gpu::ValidationCapture — RET-4 (ADR-0105): the validation-layer message
// capture, PORTED from crd-rhi onto the ONE graphics layer (the rhi original
// dies at RET-8). RAII: installs a SECOND debug-utils messenger on the
// context's VkInstance, counts errors/warnings/info, and lets tests assert
// exactly what they expect — the validation layer is the authoritative oracle
// for "did I do Vulkan correctly", and gates assert on COUNTERS, never on
// eyeballed log output.
//
// Pattern:
//   crd::gpu::ValidationCapture capture(*vk_ctx);
//   // ... drive the GPU work ...
//   CHECK(capture.error_count()   == 0);
//   CHECK(capture.warning_count() == 0);
//
// The context must have been created with `enable_validation = true` (the
// instance then enables VK_EXT_debug_utils; without it capture stays silent).
// Thread-safe: counters are atomic; the callback may fire from any driver
// thread. Multiple captures on one instance are independent.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::gpu
{

class VulkanGpuContext;

enum class ValidationSeverity : crd::u8
{
    Info,
    Warning,
    Error,
};

struct ValidationMessage
{
    ValidationSeverity      severity          = ValidationSeverity::Info;
    crd::i32                message_id_number = 0; // Vulkan VUID number when present
    crd::containers::String message_text{};
};

class ValidationCapture
{
public:
    explicit ValidationCapture(VulkanGpuContext& ctx);
    ~ValidationCapture();

    ValidationCapture(const ValidationCapture&)            = delete;
    ValidationCapture& operator=(const ValidationCapture&) = delete;
    ValidationCapture(ValidationCapture&&)                 = delete;
    ValidationCapture& operator=(ValidationCapture&&)      = delete;

    // Severity counts since construction.
    [[nodiscard]] crd::u32 error_count() const noexcept;
    [[nodiscard]] crd::u32 warning_count() const noexcept;
    [[nodiscard]] crd::u32 info_count() const noexcept;

    // Convenience: total errors + warnings (info usually noise).
    [[nodiscard]] crd::u32 error_or_warning_count() const noexcept { return error_count() + warning_count(); }

    // Captured message records (capped at 256 to bound memory; overflow drops with a count-only signal).
    [[nodiscard]] crd::containers::ConstSpan<ValidationMessage> messages() const noexcept;

    // Whitelist a known-benign VUID: still recorded, never counted.
    void whitelist(crd::i32 message_id_number);

    // Reset counters + records (re-use the capture for a second scope of work).
    void reset() noexcept;

    // Opaque impl; public so the messenger callback (a free function in the .cpp) can cast its user_data.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
