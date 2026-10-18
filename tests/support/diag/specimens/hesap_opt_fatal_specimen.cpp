// hesap-opt contract fatals (crd-hesap-opt-fatal-* CTests, tests/numerics/hesap-opt). A Model stores its objective
// and each constraint as a type-erased function constructed in a block of the caller's allocator; one that refuses it
// (a sub-budget allocator returns nullptr) leaves the model out of memory, fatal in every build, never a null function
// stored and called by solve() (DIAG.3a; the null result was stored before). One mode per run (argv[1]); see
// expected_fatal.hpp for the exit protocol:
//   model-objective-refused  Model::minimize: the objective's function is refused
//   model-eq-refused         Model::subject_to_eq: the constraint's function is refused
//   model-ineq-refused       Model::subject_to_ge: the constraint's function is refused
// The model's variables are granted first; the refusing allocator (refusing_allocator.hpp) then refuses every request,
// and the function is the call's first.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/hesap/opt/model.hpp>

#include <cstring>

namespace
{
namespace opt = crd::hesap::opt;

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    const bool objective = is(mode, "model-objective-refused");
    const bool eq = is(mode, "model-eq-refused");
    const bool ineq = is(mode, "model-ineq-refused");
    if (!objective && !eq && !ineq)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, opt::detail::kModelOutOfMemory);
    crd_diag_fatal::RefusingAllocator alloc("hesap-opt-fatal-refusing");
    opt::Model<crd::f64> model(&alloc);
    (void)model.add_variables(2U, /*start=*/0.5);
    alloc.refuse_all();
    if (objective)
    {
        model.minimize([](const auto& x) { return x[0] * x[0] + x[1] * x[1]; });
    }
    else if (eq)
    {
        model.subject_to_eq([](const auto& x) { return x[0] - x[1]; });
    }
    else
    {
        model.subject_to_ge([](const auto& x) { return x[0] + x[1]; });
    }
    return true;
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
