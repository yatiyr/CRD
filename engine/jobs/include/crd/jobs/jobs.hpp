#pragma once

// crd-jobs public umbrella header.
// Full job system API (run/wait/parallel_for) ships in v1h.
// See docs/phases/phase-2.5-jobs.md for the complete design.

#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/jobs/job_decl.hpp>
