// DIAG.7a(b): the common (backend-agnostic) GPU-validation vocabulary contract.
//
// Device-free. Pins the one rule 7a's acceptance turns on -- "dropped validation messages are not a clean run" --
// at the common-contract level, so BOTH backends inherit it by projecting onto ValidationReport. The per-backend
// projections are exercised where their types live (DX12: test_dx12_validation.cpp; Vulkan report(): built here,
// live injection is sub-unit (g)).

#include <catch2/catch_test_macros.hpp>

#include <crd/gpu/validation.hpp>

#include <cstring>

using crd::gpu::ValidationReport;
using crd::gpu::ValidationSeverity;

TEST_CASE("ValidationReport::clean requires no errors/warnings and nothing lost", "[gpu][diag][validation][vocab]")
{
    ValidationReport ok;
    REQUIRE(ok.clean()); // all zero -> clean

    // The acceptance rule: any lost/failed message makes the run NOT clean -- the missing messages might be errors.
    {
        ValidationReport r;
        r.dropped = 1;
        REQUIRE_FALSE(r.clean());
    }
    {
        ValidationReport r;
        r.truncated = 1;
        REQUIRE_FALSE(r.clean());
    }
    {
        ValidationReport r;
        r.instrumentation_failures = 1;
        REQUIRE_FALSE(r.clean());
    }
    {
        ValidationReport r;
        r.error = 1;
        REQUIRE_FALSE(r.clean());
    }
    {
        ValidationReport r;
        r.warning = 1;
        REQUIRE_FALSE(r.clean());
    }
    {
        ValidationReport r;
        r.info = 1000; // info alone is not a failure
        REQUIRE(r.clean());
    }
}

TEST_CASE("common ValidationSeverity to_string is stable", "[gpu][diag][validation][vocab]")
{
    REQUIRE(std::strcmp(crd::gpu::to_string(ValidationSeverity::Error), "error") == 0);
    REQUIRE(std::strcmp(crd::gpu::to_string(ValidationSeverity::Warning), "warning") == 0);
    REQUIRE(std::strcmp(crd::gpu::to_string(ValidationSeverity::Info), "info") == 0);
}

// DIAG.7a(f): the per-mode validation ACTIVATION report's vendor-free invariants (device-free half of (f)). A backend
// fills this struct from its native probe; `consistent()` is the contract every real report must satisfy, so a backend
// that mis-fills it is a bug caught here rather than a confusing device state.
TEST_CASE("DIAG.7a(f): ValidationActivation consistency invariants", "[gpu][diag][validation][vocab]")
{
    using crd::gpu::ValidationActivation;
    using crd::gpu::ValidationMode;
    using crd::gpu::ValidationUnsupportedReason;

    // Default: nothing requested, nothing active, every reason NotRequested -> consistent.
    ValidationActivation def;
    REQUIRE(def.consistent());
    for (crd::usize i = 0; i < crd::gpu::kValidationModeCount; ++i)
    {
        CHECK_FALSE(def.requested[i]);
        CHECK_FALSE(def.active[i]);
        CHECK(def.reason[i] == ValidationUnsupportedReason::NotRequested);
    }

    // A well-formed mixed report: Core active, Synchronization requested-but-unsupported, GpuAssisted not requested.
    ValidationActivation ok;
    ok.requested[0] = true; ok.active[0] = true;  ok.reason[0] = ValidationUnsupportedReason::None;
    ok.requested[1] = true; ok.active[1] = false; ok.reason[1] = ValidationUnsupportedReason::ExtensionAbsent;
    REQUIRE(ok.consistent());
    CHECK(ok.is_active(ValidationMode::Core));
    CHECK_FALSE(ok.is_active(ValidationMode::Synchronization));
    CHECK(ok.unsupported_reason(ValidationMode::Synchronization) == ValidationUnsupportedReason::ExtensionAbsent);
    CHECK(ok.unsupported_reason(ValidationMode::GpuAssisted) == ValidationUnsupportedReason::NotRequested);

    // Each invariant, violated in isolation, is caught:
    ValidationActivation bad1; bad1.active[0] = true;                       // active but not requested
    CHECK_FALSE(bad1.consistent());
    ValidationActivation bad2; bad2.requested[0] = true; bad2.active[0] = true;
    bad2.reason[0] = ValidationUnsupportedReason::LayerAbsent;              // active but reason != None
    CHECK_FALSE(bad2.consistent());
    ValidationActivation bad3; bad3.reason[0] = ValidationUnsupportedReason::LayerAbsent; // !requested but reason != NotRequested
    CHECK_FALSE(bad3.consistent());
}

TEST_CASE("DIAG.7a(f): ValidationUnsupportedReason to_string is stable", "[gpu][diag][validation][vocab]")
{
    using R = crd::gpu::ValidationUnsupportedReason;
    REQUIRE(std::strcmp(crd::gpu::to_string(R::None), "none") == 0);
    REQUIRE(std::strcmp(crd::gpu::to_string(R::NotRequested), "not-requested") == 0);
    REQUIRE(std::strcmp(crd::gpu::to_string(R::BackendHasNoEquivalent), "backend-has-no-equivalent") == 0);
    REQUIRE(std::strcmp(crd::gpu::to_string(R::FeatureAbsent), "feature-absent") == 0);
}
