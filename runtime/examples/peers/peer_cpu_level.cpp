// The configure-time probe of crd_peer_isa_flags (cmake/CrdSimd.cmake): prints the CPU level crd::detect_cpus()
// reports on the configuring host, the level cl builds the board peers at (it has no host-native ISA flag).

#include <crd/core/cpu_features.hpp>

#include <cstdio>

int main()
{
    std::printf("%s\n", crd::cpu_level_name(crd::detect_cpus().detected));
    return 0;
}
