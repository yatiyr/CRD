#include <crd/platform/file_watcher.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>

namespace fs = crd::platform::fs;
using crd::platform::FileWatcher;

namespace
{
[[nodiscard]] fs::Path make_temp_dir()
{
    const auto base  = fs::current_working_dir() / ".crd_platform_test_tmp";
    const auto stamp = static_cast<crd::u64>(std::chrono::system_clock::now().time_since_epoch().count());
    crd::containers::String leaf("fw_");
    leaf.append(std::to_string(stamp));
    leaf.push_back('_');
    leaf.append(std::to_string(crd::platform::threading::current_thread_id()));
    return base / leaf.c_str();
}
} // namespace

TEST_CASE("FileWatcher: no callbacks on unchanged file", "[platform][file_watcher]")
{
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto path = tmp / "unchanged.txt";
    REQUIRE(fs::write_file_text(path, "initial"));

    FileWatcher watcher;
    int fired = 0;
    (void)watcher.add(path, [&](const fs::Path&) { ++fired; });

    watcher.poll(); // first poll after add — file unchanged
    REQUIRE(fired == 0);
    watcher.poll();
    REQUIRE(fired == 0);

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("FileWatcher: callback fires when file content changes", "[platform][file_watcher]")
{
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto path = tmp / "watched.txt";
    REQUIRE(fs::write_file_text(path, "v1"));

    FileWatcher watcher;
    int fired = 0;
    (void)watcher.add(path, [&](const fs::Path&) { ++fired; });

    watcher.poll(); // no change
    REQUIRE(fired == 0);

    // On some file systems mtime granularity is 1 second. Sleep briefly to
    // ensure the new write gets a different timestamp.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(fs::write_file_text(path, "v2"));

    watcher.poll(); // change detected
    REQUIRE(fired == 1);

    watcher.poll(); // no further change
    REQUIRE(fired == 1);

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("FileWatcher: remove stops future callbacks", "[platform][file_watcher]")
{
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto path = tmp / "removable.txt";
    REQUIRE(fs::write_file_text(path, "v1"));

    FileWatcher watcher;
    int fired = 0;
    const crd::u64 handle = watcher.add(path, [&](const fs::Path&) { ++fired; });

    REQUIRE(watcher.count() == 1U);
    watcher.remove(handle);
    REQUIRE(watcher.count() == 0U);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(fs::write_file_text(path, "v2"));
    watcher.poll();
    REQUIRE(fired == 0); // handle was removed; no callback

    REQUIRE(fs::remove_all(tmp));
}

TEST_CASE("FileWatcher: multiple files watched independently", "[platform][file_watcher]")
{
    const auto tmp = make_temp_dir();
    REQUIRE(fs::create_directories(tmp));

    const auto path_a = tmp / "a.txt";
    const auto path_b = tmp / "b.txt";
    REQUIRE(fs::write_file_text(path_a, "a0"));
    REQUIRE(fs::write_file_text(path_b, "b0"));

    FileWatcher watcher;
    int fired_a = 0;
    int fired_b = 0;
    (void)watcher.add(path_a, [&](const fs::Path&) { ++fired_a; });
    (void)watcher.add(path_b, [&](const fs::Path&) { ++fired_b; });

    watcher.poll();
    REQUIRE(fired_a == 0);
    REQUIRE(fired_b == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(fs::write_file_text(path_a, "a1")); // only a changes

    watcher.poll();
    REQUIRE(fired_a == 1);
    REQUIRE(fired_b == 0);

    REQUIRE(fs::remove_all(tmp));
}
