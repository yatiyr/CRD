// crd-platform -- owner-only directories and file locks (the key store's, NET.2; docs/design/network-foundation.md
// #key-store, decisions K3 and K6): create_private_directory checked against the operating system's own record of who
// may enter (the DACL on Windows, the mode on POSIX), and FileLock holders that never overlap. Apart from
// test_filesystem.cpp because it reads <windows.h>, whose macros that file's names would collide with.

#include <crd/core/platform.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] fs::Path make_temp_root()
{
    static std::atomic<unsigned> serial{0U};
    crd::containers::String      leaf("crd_private_dir_");
    leaf.append(std::to_string(crd::platform::threading::current_thread_id()));
    leaf.push_back('_');
    leaf.append(std::to_string(serial.fetch_add(1U)));
    const fs::Path root = fs::temp_directory() / crd::containers::StringView(leaf.data(), leaf.size());
    (void)fs::remove_all(root);
    return root;
}

// Whether only the calling user may enter `path`, and every file created in it inherits that.
[[nodiscard]] bool owner_only(const fs::Path& path)
{
#if CRD_OS_WINDOWS
    const crd::containers::StringView text = path.generic();
    crd::containers::Array<wchar_t>   native;
    native.resize(text.size() + 1U, L'\0');
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), native.data(),
                                         static_cast<int>(text.size()));
    native[static_cast<crd::usize>(wide > 0 ? wide : 0)] = L'\0';
    PACL                 dacl       = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS)
    {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control  = 0U;
    DWORD                       revision = 0U;
    bool ok = GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
              (control & SE_DACL_PROTECTED) != 0U && dacl != nullptr && dacl->AceCount == 1U;
    void* ace = nullptr;
    ok        = ok && GetAce(dacl, 0U, &ace) != 0;
    if (ok)
    {
        auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        HANDLE      token   = nullptr;
        BYTE        user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD       bytes = 0U;
        ok = allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
             (allowed->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ==
                 (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
             allowed->Mask == FILE_ALL_ACCESS && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0;
        if (ok)
        {
            ok = GetTokenInformation(token, TokenUser, user, sizeof(user), &bytes) != 0 &&
                 EqualSid(&allowed->SidStart, reinterpret_cast<TOKEN_USER*>(user)->User.Sid) != 0;
            CloseHandle(token);
        }
    }
    LocalFree(descriptor);
    return ok;
#else
    struct stat info{};
    return ::stat(path.native().c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           (info.st_mode & 0777U) == S_IRWXU && info.st_uid == ::geteuid();
#endif
}
} // namespace

TEST_CASE("filesystem: create_private_directory admits only its owner", "[platform][fs]")
{
    const fs::Path root = make_temp_root();
    const fs::Path dir  = root / "a" / "b";
    REQUIRE(fs::create_private_directory(dir));
    CHECK(fs::is_directory(dir));
    CHECK(owner_only(dir));

    // A file written in it, and the directory again: still the owner's alone.
    REQUIRE(fs::write_file_text(dir / "inside.txt", "x"));
    REQUIRE(fs::create_private_directory(dir));
    CHECK(owner_only(dir));

    // An existing, open directory is restricted.
    const fs::Path open_dir = root / "open";
    REQUIRE(fs::create_directories(open_dir));
#if !CRD_OS_WINDOWS
    REQUIRE(::chmod(open_dir.native().c_str(), 0755) == 0);
    CHECK_FALSE(owner_only(open_dir));
#endif
    REQUIRE(fs::create_private_directory(open_dir));
    CHECK(owner_only(open_dir));

    // A file in the way is not a directory.
    REQUIRE(fs::write_file_text(root / "file", "x"));
    CHECK_FALSE(fs::create_private_directory(root / "file"));
    REQUIRE(fs::remove_all(root));
}

TEST_CASE("filesystem: FileLock orders holders across objects and threads", "[platform][fs]")
{
    const fs::Path root = make_temp_root();
    REQUIRE(fs::create_directories(root));
    const fs::Path path = root / "lock";

    fs::FileLock first;
    CHECK_FALSE(first.held());
    REQUIRE(first.acquire(path));
    CHECK(first.held());
    CHECK(fs::is_file(path));

    // A second holder in another thread waits until the first releases.
    std::atomic<bool> second_holds{false};
    std::thread       waiter(
        [&]()
        {
            fs::FileLock second;
            if (second.acquire(path))
            {
                second_holds.store(true);
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(second_holds.load());
    first.release();
    CHECK_FALSE(first.held());
    waiter.join();
    CHECK(second_holds.load());

    // Re-acquired after release, and released by destruction; the counter shows no two holders at once.
    std::atomic<int> inside{0};
    std::atomic<int> overlaps{0};
    std::thread      workers[4];
    for (std::thread& worker : workers)
    {
        worker = std::thread(
            [&]()
            {
                for (int i = 0; i < 25; ++i)
                {
                    fs::FileLock lock;
                    if (!lock.acquire(path))
                    {
                        overlaps.fetch_add(1);
                        return;
                    }
                    if (inside.fetch_add(1) != 0)
                    {
                        overlaps.fetch_add(1);
                    }
                    std::this_thread::yield();
                    inside.fetch_sub(1);
                }
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    CHECK(overlaps.load() == 0);

    // A lock file that cannot be opened: its directory does not exist.
    fs::FileLock missing;
    CHECK_FALSE(missing.acquire(root / "absent" / "lock"));
    CHECK_FALSE(missing.held());
    REQUIRE(fs::remove_all(root));
}
