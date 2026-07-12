#pragma once

// ---------------------------------------------------------------------------
// crd::rhi::ValidationCapture — RAII per-instance Vulkan validation-layer
// message capture for tests. Phase 3.1.7 v9-prereq-test-harness (2026-05-18).
//
// USE CASE: GPU sanity-check discipline. Tests that exercise compute
// dispatch / barriers / pipelines need to assert validation-layer
// silence (the validation layer is the authoritative oracle for "did
// I do Vulkan correctly"). Manual eyeballing of log output doesn't
// scale across 16 v9 GPU slices. ValidationCapture installs a SECOND
// debug-utils messenger on the Instance, counts errors/warnings/info,
// and lets tests assert exactly what they expect.
//
// Pattern:
//
//   auto capture = crd::rhi::ValidationCapture(*instance);
//   // ... drive the GPU work ...
//   REQUIRE(capture.error_count()   == 0);
//   REQUIRE(capture.warning_count() == 0);
//   // OR for a negative test:
//   REQUIRE(capture.error_count() >= 1);  // proves missing-barrier fires
//
// Lifetime: ctor installs the messenger; dtor removes it. Multiple
// captures on the same instance are supported (each is independent).
// Thread-safe: counters are atomic; the callback may fire from any
// driver thread.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::rhi
{

class Instance;
class Device;

enum class ValidationSeverity : crd::u8
{
    Info,
    Warning,
    Error,
};

struct ValidationMessage
{
    ValidationSeverity      severity         = ValidationSeverity::Info;
    crd::i32                message_id_number = 0; // Vulkan VUID number when present
    crd::containers::String message_text{};
};

class ValidationCapture
{
public:
    // Installs a debug-utils messenger on the underlying VkInstance.
    // The Instance MUST have been created with `enable_validation = true`
    // (otherwise VK_EXT_debug_utils may not be loaded and capture will
    // silently observe nothing).
    explicit ValidationCapture(Instance& instance);
    // D-008 C2-f: attach to the VkInstance behind an ADOPTED device (rhi no longer creates its own instance for tests).
    // The device's gpu-context must have been created with validation enabled (see `create_vulkan_gpu_context`).
    explicit ValidationCapture(Device& device);
    ~ValidationCapture();

    ValidationCapture(const ValidationCapture&)            = delete;
    ValidationCapture& operator=(const ValidationCapture&) = delete;
    ValidationCapture(ValidationCapture&&)                 = delete;
    ValidationCapture& operator=(ValidationCapture&&)      = delete;

    // Severity counts since construction.
    [[nodiscard]] crd::u32 error_count()   const noexcept;
    [[nodiscard]] crd::u32 warning_count() const noexcept;
    [[nodiscard]] crd::u32 info_count()    const noexcept;

    // Convenience: total errors + warnings (info usually noise).
    [[nodiscard]] crd::u32 error_or_warning_count() const noexcept
    {
        return error_count() + warning_count();
    }

    // Captured message records (capped at 256 to bound memory; older
    // messages dropped on overflow with a count-only signal).
    [[nodiscard]] crd::containers::ConstSpan<ValidationMessage> messages() const noexcept;

    // Whitelist a known-benign VUID. Whitelisted messages still appear
    // in `messages()` but do NOT count toward error_count()/warning_count().
    void whitelist(crd::i32 message_id_number);

    // Reset counters + recorded messages (re-use the capture for a
    // second scope of work within the same test).
    void reset() noexcept;

    // Forward-declared opaque impl. Public so the Vulkan messenger
    // callback (a free function in the .cpp) can static_cast its
    // user_data ptr; the definition lives in the .cpp, so this does
    // NOT expose any internals.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::rhi
