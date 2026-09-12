#pragma once

#include <crd/platform/filesystem.hpp>

#include <utility>

// Own test output even when Catch2 unwinds after a failed REQUIRE.
class ScopedTestFile
{
public:
    explicit ScopedTestFile(crd::platform::fs::Path path) : m_path(std::move(path)) {}
    ScopedTestFile(const ScopedTestFile&) = delete;
    ScopedTestFile& operator=(const ScopedTestFile&) = delete;
    ~ScopedTestFile() noexcept { (void)crd::platform::fs::remove_file(m_path); }

    [[nodiscard]] const crd::platform::fs::Path& path() const noexcept { return m_path; }

private:
    crd::platform::fs::Path m_path;
};
