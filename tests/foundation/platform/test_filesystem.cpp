#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <limits>
#include <ratio>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] fs::Path make_temp_root()
{
    // Under the system's scratch directory, never the working directory (a checkout when run from one).
    const auto base = fs::temp_directory() / "crd_platform_test_tmp";
    const auto stamp = static_cast<crd::u64>(std::chrono::system_clock::now().time_since_epoch().count());
    crd::containers::String leaf("fs_");
    leaf.append(std::to_string(stamp));
    leaf.push_back('_');
    leaf.append(std::to_string(crd::platform::threading::current_thread_id()));
    return base / leaf.c_str();
}
} // namespace

TEST_CASE("fs::Path normalizes slashes and joins predictably", "[platform][fs]")
{
    const fs::Path a("foo\\bar\\baz/");
    REQUIRE(a.generic() == "foo/bar/baz");

    const fs::Path b = a / "/qux";
    REQUIRE(b.generic() == "foo/bar/baz/qux");
}

TEST_CASE("filesystem: current_working_dir and executable_dir are non-empty directories", "[platform][fs]")
{
    const auto cwd = fs::current_working_dir();
    const auto exe = fs::executable_dir();
    REQUIRE_FALSE(cwd.empty());
    REQUIRE_FALSE(exe.empty());
    REQUIRE(fs::is_directory(cwd));
    REQUIRE(fs::is_directory(exe));
}

TEST_CASE("filesystem: text roundtrip + metadata", "[platform][fs]")
{
    const auto root = make_temp_root();
    REQUIRE(fs::create_directories(root));

    const auto file = root / "hello.txt";
    REQUIRE(fs::write_file_text(file, "hello cerid"));
    REQUIRE(fs::exists(file));
    REQUIRE(fs::is_file(file));
    REQUIRE(fs::file_size(file) == 11U);
    REQUIRE(fs::last_modified_unix_seconds(file) > 0);
    REQUIRE(fs::last_modified_unix_ns(file) / 1000000000 == fs::last_modified_unix_seconds(file));

    crd::containers::String text;
    REQUIRE(fs::read_file_text(file, text));
    REQUIRE(text == "hello cerid");

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("filesystem: binary roundtrip", "[platform][fs]")
{
    const auto root = make_temp_root();
    REQUIRE(fs::create_directories(root));

    const auto file = root / "blob.bin";
    const crd::u8 bytes[] = {0, 1, 2, 3, 4, 255};
    REQUIRE(fs::write_file_binary(file, crd::containers::make_span(bytes)));

    crd::containers::Array<crd::u8> out;
    REQUIRE(fs::read_file_binary(file, out));
    REQUIRE(out.size() == 6U);
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        REQUIRE(out[i] == bytes[i]);
    }

    // A range past the end fails and leaves nothing behind: the readers do not zero the bytes they read into.
    crd::containers::Array<crd::u8> past_end;
    REQUIRE_FALSE(fs::read_file_range(file, 4U, 8U, past_end));
    REQUIRE(past_end.empty());

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("filesystem: list_directory sees created entries", "[platform][fs]")
{
    const auto root = make_temp_root();
    REQUIRE(fs::create_directories(root));

    const auto a = root / "a.txt";
    const auto b = root / "b.txt";
    REQUIRE(fs::write_file_text(a, "a"));
    REQUIRE(fs::write_file_text(b, "b"));

    crd::containers::Array<fs::Path> entries;
    fs::list_directory(root, entries);
    REQUIRE(entries.size() == 2U);

    bool saw_a = false;
    bool saw_b = false;
    for (const auto& entry : entries)
    {
        if (entry == a)
        {
            saw_a = true;
        }
        if (entry == b)
        {
            saw_b = true;
        }
    }
    REQUIRE(saw_a);
    REQUIRE(saw_b);

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("filesystem: the last write time converts exactly and orders files within one second", "[platform][fs]")
{
    using namespace std::chrono;
    const auto root = make_temp_root();
    REQUIRE(fs::create_directories(root));
    const auto older = root / "older.bin";
    const auto newer = root / "newer.bin";
    REQUIRE(fs::write_file_text(older, "o"));
    REQUIRE(fs::write_file_text(newer, "n"));

    // 2026-01-01T00:00:00.1234567Z, a whole number of NTFS's 100 ns ticks, and one millisecond later.
    constexpr crd::i64 unix_ns = 1767225600123456700;
    const sys_time<nanoseconds> at{nanoseconds{unix_ns}};
    const auto stamp = [](const fs::Path& path, sys_time<nanoseconds> time)
    {
        std::error_code ec;
        // Narrow to the file clock's tick before clock_cast (exact for the times below): the Microsoft file clock
        // counts 100 ns ticks from 1601, and shifting a nanosecond count by those 369 years overflows i64 (the cast
        // then hands SetFileTime a negative time). libstdc++'s file clock counts nanoseconds itself.
        const auto file_time = clock_cast<file_clock>(time_point_cast<std::filesystem::file_time_type::duration>(time));
        std::filesystem::last_write_time(std::filesystem::path{path.native().c_str()}, file_time, ec);
        return !ec;
    };
    REQUIRE(stamp(older, at));
    REQUIRE(stamp(newer, at + milliseconds{1}));

    CHECK(fs::last_modified_unix_ns(older) == unix_ns);
    CHECK(fs::last_modified_unix_ns(newer) == unix_ns + 1000000);
    CHECK(fs::last_modified_unix_seconds(older) == 1767225600);
    CHECK(fs::last_modified_unix_seconds(newer) == 1767225600);
    CHECK(fs::last_modified_unix_ns(root / "missing.bin") == 0);
    CHECK(fs::last_modified_unix_seconds(root / "missing.bin") == 0);

    // A file clock coarser than nanoseconds (the Microsoft library's 100 ns ticks from 1601) reaches past 2262, where
    // the nanosecond reading saturates and the seconds stay exact. libstdc++'s file clock counts nanoseconds itself, so
    // it holds no such time.
    if constexpr (std::ratio_greater_v<std::filesystem::file_time_type::period, std::nano>)
    {
        constexpr crd::i64 k2300 = 10413792000; // 2300-01-01T00:00:00Z
        const auto far = clock_cast<file_clock>(sys_seconds{seconds{k2300}});
        std::error_code ec;
        std::filesystem::last_write_time(std::filesystem::path{older.native().c_str()}, far, ec);
        REQUIRE_FALSE(ec);
        CHECK(fs::last_modified_unix_seconds(older) == k2300);
        CHECK(fs::last_modified_unix_ns(older) == std::numeric_limits<crd::i64>::max());
    }

    REQUIRE(fs::remove_all(root));
}

TEST_CASE("filesystem: user_config_dir appends app name", "[platform][fs]")
{
    const auto cfg = fs::user_config_dir("CeridTests");
    REQUIRE_FALSE(cfg.empty());
    REQUIRE(cfg.generic().find("CeridTests") != crd::containers::StringView::npos);
}
