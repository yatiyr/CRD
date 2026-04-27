#include <crd/platform/dynamic_library.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_OS_WINDOWS
#include <windows.h>
#endif

TEST_CASE("DynamicLibrary: invalid default instance resolves nothing", "[platform][dynlib]")
{
    crd::platform::DynamicLibrary lib;
    REQUIRE_FALSE(lib.is_valid());
    REQUIRE(lib.resolve("nope") == nullptr);
}

TEST_CASE("DynamicLibrary: open system library and resolve known symbol", "[platform][dynlib]")
{
#if CRD_OS_WINDOWS
    auto lib = crd::platform::DynamicLibrary::open(crd::platform::fs::Path("kernel32.dll"));
    REQUIRE(lib.is_valid());
    using Fn = unsigned long(__stdcall*)();
    const auto fn = lib.resolve_as<Fn>("GetCurrentThreadId");
    REQUIRE(fn != nullptr);
    REQUIRE(static_cast<crd::u32>(fn()) != 0u);
#elif CRD_OS_LINUX
    auto lib = crd::platform::DynamicLibrary::open(crd::platform::fs::Path("libm.so.6"));
    REQUIRE(lib.is_valid());
    using Fn = double (*)(double);
    const auto fn = lib.resolve_as<Fn>("cos");
    REQUIRE(fn != nullptr);
    REQUIRE(fn(0.0) == 1.0);
#else
    SUCCEED("System-library resolution test not defined on this OS yet");
#endif
}

TEST_CASE("DynamicLibrary: move transfers ownership", "[platform][dynlib]")
{
#if CRD_OS_WINDOWS
    auto a = crd::platform::DynamicLibrary::open(crd::platform::fs::Path("kernel32.dll"));
#elif CRD_OS_LINUX
    auto a = crd::platform::DynamicLibrary::open(crd::platform::fs::Path("libm.so.6"));
#else
    crd::platform::DynamicLibrary a;
#endif
    REQUIRE(a.is_valid());
    crd::platform::DynamicLibrary b(std::move(a));
    REQUIRE(b.is_valid());
    REQUIRE_FALSE(a.is_valid());
}
