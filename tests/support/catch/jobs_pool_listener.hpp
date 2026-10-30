// The jobs pool listener's one entry point (jobs_pool_listener.cpp, linked into every test executable that links
// crd-jobs). A fixture that keeps one pool up for the rest of its executable's run (a function-static brought up by the
// first case that needs it) calls keep_jobs_pool() once the pool is up: the listener then leaves that pool up after a
// failed case. Without it the listener took the pool for one the failed case had left up and shut it down, and every
// later case of the run met a pool that was down.

#pragma once

namespace crd::test
{
void keep_jobs_pool() noexcept;
} // namespace crd::test
