// Anchor TU for crd-hesap-sched.
//
// crd-hesap-sched is header-only as of v0d-foundation (TaskGraph +
// parallel_tiles_for are all `inline` templates). Without a single .cpp,
// CMake's STATIC library doesn't produce a .lib on MSVC (the empty
// archive is skipped). This anchor provides one symbol so the .lib is
// created and downstream test/runtime targets can link against it.
//
// Real schedulers (formal task-DAG, work-stealing tile scheduler) land in
// the v0d-formal-dag follow-on; they'll move the implementation here.

#include <crd/hesap/sched/task_graph.hpp>

namespace crd::hesap::sched
{
// Anchor symbol — referenced from downstream consumers (in practice, the
// linker just needs ONE concrete symbol in the .obj for the .lib to exist).
void task_graph_anchor() noexcept
{
}
} // namespace crd::hesap::sched
