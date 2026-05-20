#pragma once

#include <crd/jobs/jobs.hpp>

// Shared jobs-init/shutdown fixture for hesap-dense tests. Function-static
// inside an inline function → single storage across TUs. The first test
// that calls hesap_jobs_listener() constructs the listener, which calls
// crd::jobs::init() once. The destructor at program exit calls
// crd::jobs::shutdown().
//
// Required because crd::jobs only supports ONE init() per binary
// (CLAUDE.md "jobs::init() in test binaries" — double-init crashes).
// test_blas3_parallel.cpp and test_lu.cpp both need workers; both
// include this header.

namespace crd_hesap_dense_tests
{
struct HesapJobsListener
{
    HesapJobsListener() { crd::jobs::init(); }
    ~HesapJobsListener() { crd::jobs::shutdown(); }
    HesapJobsListener(const HesapJobsListener&) = delete;
    HesapJobsListener& operator=(const HesapJobsListener&) = delete;
    HesapJobsListener(HesapJobsListener&&) = delete;
    HesapJobsListener& operator=(HesapJobsListener&&) = delete;
};

inline HesapJobsListener& hesap_jobs_listener()
{
    static HesapJobsListener listener;
    return listener;
}
} // namespace crd_hesap_dense_tests
