#pragma once

// crd-crypto tests -- a directory under the system's scratch directory, removed with everything in it at the end of
// its scope: a key store's (test_key_store.cpp) or an installation's policy state (test_policy_state.cpp).

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <atomic>
#include <string>

namespace crd::crypto::test
{
class TempDirectory
{
public:
    explicit TempDirectory(const char* tag)
    {
        static std::atomic<u32> serial{0U};
        containers::String      leaf("crd_crypto_");
        leaf.append(tag);
        leaf.push_back('_');
        leaf.append(std::to_string(platform::threading::current_thread_id()));
        leaf.push_back('_');
        leaf.append(std::to_string(serial.fetch_add(1U)));
        m_path = platform::fs::temp_directory() / containers::StringView(leaf.data(), leaf.size());
        (void)platform::fs::remove_all(m_path);
    }
    TempDirectory(const TempDirectory&)            = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() { (void)platform::fs::remove_all(m_path); }

    [[nodiscard]] containers::StringView    dir() const { return m_path.generic(); }
    [[nodiscard]] const platform::fs::Path& path() const { return m_path; }

private:
    platform::fs::Path m_path;
};
} // namespace crd::crypto::test
