#pragma once

// A test process's own folder for the files its cases write and read back, so they never land in the working
// directory (for a run started in the checkout, the repository root). The folder is <temp>/crd-scratch-<pid>: one
// per process, so cases that ctest runs as parallel processes never share a file, made fresh on first use and
// removed when the process exits, a failed case's process included. A process that dies leaves its files there, under
// the temp directory, never in the checkout. Paths are kept in fixed buffers: taking the folder allocates nothing
// that outlives the call, so it never shows in a case's allocation counts.
//
// A diagnostic service takes root() as its file root, so the relative names its requests carry resolve inside the
// folder; the case's own reads and writes of those names go through scratch_path() or resolve().

#include <crd/containers/string_view.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace crd::test
{
inline constexpr usize kScratchPathBytes = 1024U;

// A path under the scratch folder, NUL-terminated.
struct ScratchPath
{
    char text[kScratchPathBytes] = {};

    [[nodiscard]] const char* c_str() const noexcept { return text; }
    [[nodiscard]] containers::StringView view() const noexcept { return containers::StringView{text}; }
};

class ScratchDir
{
public:
    ScratchDir()
    {
#if defined(_WIN32)
        const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        char leaf[64] = {};
        (void)std::snprintf(leaf, sizeof(leaf), "crd-scratch-%lu", pid);
        const platform::fs::Path temp = platform::fs::temp_directory();
        REQUIRE(!temp.empty()); // an empty root would make a service answer Unavailable, not fail here
        const platform::fs::Path dir = temp / containers::StringView{leaf};
        const containers::StringView path = dir.generic();
        REQUIRE(path.size() < kScratchPathBytes);
        std::memcpy(m_root.text, path.data(), path.size());
        m_root.text[path.size()] = '\0';
        (void)platform::fs::remove_all(dir); // a folder a dead process with this pid left behind
        REQUIRE(platform::fs::create_directories(dir));
    }
    ~ScratchDir() { (void)platform::fs::remove_all(platform::fs::Path{m_root.view()}); }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    // The folder, absolute, with forward slashes: a diagnostic service's file root.
    [[nodiscard]] containers::StringView root() const noexcept { return m_root.view(); }

    // `name`, a path relative to the folder, under it.
    [[nodiscard]] ScratchPath at(containers::StringView name) const
    {
        const containers::StringView root = m_root.view();
        REQUIRE(root.size() + 1U + name.size() < kScratchPathBytes);
        ScratchPath out;
        std::memcpy(out.text, root.data(), root.size());
        out.text[root.size()] = '/';
        std::memcpy(out.text + root.size() + 1U, name.data(), name.size());
        out.text[root.size() + 1U + name.size()] = '\0';
        return out;
    }

private:
    ScratchPath m_root;
};

// This process's scratch folder, made on first use.
inline const ScratchDir& scratch_dir()
{
    static const ScratchDir kScratch;
    return kScratch;
}

// `name` under this process's scratch folder.
inline ScratchPath scratch_path(const char* name)
{
    return scratch_dir().at(containers::StringView{name});
}

// A path a case's own file helpers take: an absolute one (a committed asset under CRD_REPO_DIR) as it is, a relative
// one (a name the case also hands a service rooted at scratch_root()) under this process's scratch folder.
inline ScratchPath resolve(const char* path)
{
    const containers::StringView p{path};
    const bool absolute = (!p.empty() && p[0] == '/') || (p.size() >= 2U && p[1] == ':');
    if (!absolute)
    {
        return scratch_path(path);
    }
    REQUIRE(p.size() < kScratchPathBytes);
    ScratchPath out;
    std::memcpy(out.text, p.data(), p.size());
    out.text[p.size()] = '\0';
    return out;
}

// The scratch folder as a diagnostic service's file root.
inline containers::StringView scratch_root()
{
    return scratch_dir().root();
}
} // namespace crd::test
