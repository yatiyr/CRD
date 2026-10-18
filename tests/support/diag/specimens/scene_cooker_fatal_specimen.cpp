// Scene and öbek cooker contract fatals (crd-scene-cooker-fatal-* CTests, tests/assets/scene_cooker). Each cooker
// constructs its reader registry in a block of the caller's allocator; one that refuses it (a sub-budget allocator
// returns nullptr) leaves the cooker out of memory, fatal in every build, never a null registry constructed into
// (DIAG.3a; it was constructed into the refused pointer before). One mode per run (argv[1]); see expected_fatal.hpp
// for the exit protocol:
//   scene-cooker-refused  SceneCooker(alloc): its registry is refused
//   obek-cooker-refused   ObekCooker(alloc): its registry is refused
// The registry is the constructor's first allocation, so the refusing allocator (refusing_allocator.hpp) refuses
// every request from the construction on.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/cooker/obek_cooker.hpp>
#include <crd/cooker/scene_cooker.hpp>

#include <cstring>

namespace
{
namespace cooker = crd::cooker;

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    crd_diag_fatal::RefusingAllocator alloc("scene-cooker-fatal-refusing");
    if (std::strcmp(mode, "scene-cooker-refused") == 0)
    {
        crd_diag_fatal::arm(mode, cooker::detail::kSceneCookerOutOfMemory);
        alloc.refuse_all();
        const cooker::SceneCooker c(&alloc);
        return true;
    }
    if (std::strcmp(mode, "obek-cooker-refused") == 0)
    {
        crd_diag_fatal::arm(mode, cooker::detail::kObekCookerOutOfMemory);
        alloc.refuse_all();
        const cooker::ObekCooker c(&alloc);
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
