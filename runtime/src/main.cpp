#include <crd/core/types.hpp>

#include <cstdio>

int main()
{
    crd::u32 version_major = 0;
    crd::u32 version_minor = 1;
    crd::u32 version_patch = 0;

    std::printf("CRD Engine v%u.%u.%u\n", version_major, version_minor, version_patch);

    return 0;
}