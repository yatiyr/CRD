#include <crd/core/types.hpp>

#include <format>
#include <iostream>
#include <string>

int main()
{
    const crd::u32 version_major = 0;
    const crd::u32 version_minor = 1;
    const crd::u32 version_patch = 0;

    const std::string message = std::format("CRD Engine v{:d}.{:d}.{:d}", version_major, version_minor, version_patch);
    std::cout << message << '\n';

    return 0;
}