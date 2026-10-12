#pragma once

// The diagnostic application's examples (DIAG.11a): how an external application uses the installed diagnostic
// family for allocator selection, jobs, reload and capture. Each is one route of diag-consumer, run in a fresh
// process with a fresh DiagnosticHost, and each says at the top of its file what it shows.

namespace diag_consumer
{

// example_allocator.cpp: diag-consumer allocator <profile.host.toml> <tlsf|diagnostic>
int example_allocator(const char* profile_path, const char* choice);

// example_jobs.cpp: diag-consumer jobs <profile.host.toml>
int example_jobs(const char* profile_path);

// example_reload.cpp: diag-consumer reload <profile.host.toml> <limits.toml> <second capture directory>
int example_reload(const char* profile_path, const char* limits_path, const char* second_capture_dir);

// example_capture.cpp: diag-consumer capture <profile.host.toml>
int example_capture(const char* profile_path);

} // namespace diag_consumer
