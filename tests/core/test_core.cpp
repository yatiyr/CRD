#include <catch2/catch_test_macros.hpp>
#include <crd/core/core.hpp>

TEST_CASE("Type sizes are correct", "[core][types]")
{
    REQUIRE(sizeof(crd::i8)  == 1);
    REQUIRE(sizeof(crd::i16) == 2);
    REQUIRE(sizeof(crd::i32) == 4);
    REQUIRE(sizeof(crd::i64) == 8);

    REQUIRE(sizeof(crd::f32) == 4);
    REQUIRE(sizeof(crd::f64) == 8);
}

TEST_CASE("Platform is detected", "[core][platform]")
{
    // At least one OS must be active
    REQUIRE((CRD_OS_WINDOWS + CRD_OS_LINUX + CRD_OS_MAC) == 1);

    // At least one compiler must be active
    REQUIRE((CRD_COMPILER_MSVC + CRD_COMPILER_GCC + CRD_COMPILER_CLANG) == 1);

    // At least one arch must be active
    REQUIRE((CRD_ARCH_X64 + CRD_ARCH_ARM64) == 1);

    // String functions return non-null
    REQUIRE(crd::platform_name() != nullptr);
    REQUIRE(crd::compiler_name() != nullptr);
    REQUIRE(crd::arch_name() != nullptr);
}