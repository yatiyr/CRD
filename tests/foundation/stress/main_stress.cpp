// Stress-test binary entry point.
//
// Owns process-wide setup the stress harness relies on:
//   - crd::crash::install — minidumps + stderr exception info, so a stress
//     failure in CI leaves a .dmp behind (cdb-readable) instead of a bare crash.
//   - crd::jobs::init / shutdown — exactly once for the whole binary. The fiber
//     run-mode of the harness uses the scheduler; double-init crashes, so no
//     individual test may call jobs::init().
#define CATCH_CONFIG_RUNNER
#include <crd/core/crash.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    crd::crash::install("./crashes");

    crd::jobs::Config jobs_cfg;
    jobs_cfg.frame_alloc_bytes = 8U << 20; // 8 MB/thread — stress tests do many parallel_for/parallel_reduce calls
    crd::jobs::init(jobs_cfg);             // num_threads = hardware_concurrency()
    const int rc = Catch::Session().run(argc, argv);
    crd::jobs::shutdown();

    return rc;
}
