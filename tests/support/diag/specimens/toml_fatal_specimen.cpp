// TOML contract fatal (crd-toml-fatal-* CTests, tests/foundation/toml). node::child_at is the child lookup for a walk
// below size() (table_range's iterator): an index past the last child is fatal in every build, never a reference made
// from a null pointer. One mode per run (argv[1]); see expected_fatal.hpp for the exit protocol:
//   child-at-past-end   child_at(size()) on a table with one entry

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "specimen_common.hpp"

#include <crd/containers/string_view.hpp>
#include <crd/toml/toml.hpp>

#include <cstring>

namespace
{
// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (std::strcmp(mode, "child-at-past-end") == 0)
    {
        crd_diag_fatal::arm(mode, "toml::node::child_at: the index is past the last child");
        crd::toml::table t;
        t.insert_or_assign(crd::containers::StringView{"only"}, crd::i64{1});
        return t.child_at(t.size()).is_integer();
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
