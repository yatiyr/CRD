#include <crd/core/core.hpp>

#include <iostream>
#include <format>

int main()
{
    const crd::u32 version_major = CRD_VERSION_MAJOR;
    const crd::u32 version_minor = CRD_VERSION_MINOR;
    const crd::u32 version_patch = CRD_VERSION_PATCH;

    const std::string message = std::format("CRD Engine v{:d}.{:d}.{:d}", version_major, version_minor, version_patch);
    std::cout << message << '\n';

    std::cout << crd::arch_name() << '\n';
    std::cout << crd::compiler_name() << '\n';
    std::cout << crd::platform_name() << '\n';
    std::cout << CRD_VERSION_STRING << '\n';
    // CRD_ASSERT(1 == 2);
    // CRD_ASSERT_MSG(1 == 2, "math is broken");

    return 0;
}