#pragma once

#include <crd/jobs/jobs.hpp>

// Single jobs init/shutdown for the crd-hesap-sparse test binary. crd::jobs
// supports ONE init() per binary (CLAUDE.md "jobs::init() in test binaries").
// Function-static => single storage across TUs; constructed on first use.

namespace crd_hesap_sparse_tests
{
struct SparseJobsListener
{
    SparseJobsListener() { crd::jobs::init(); }
    ~SparseJobsListener() { crd::jobs::shutdown(); }
    SparseJobsListener(const SparseJobsListener&) = delete;
    SparseJobsListener& operator=(const SparseJobsListener&) = delete;
    SparseJobsListener(SparseJobsListener&&) = delete;
    SparseJobsListener& operator=(SparseJobsListener&&) = delete;
};

inline SparseJobsListener& sparse_jobs_listener()
{
    static SparseJobsListener listener;
    return listener;
}
} // namespace crd_hesap_sparse_tests
