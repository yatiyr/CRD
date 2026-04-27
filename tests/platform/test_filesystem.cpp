#include <crd/platform/filesystem.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace fs = crd::platform::fs;

namespace
{
[[nodiscard]] fs::Path make_temp_root()
{
    const auto base = fs::current_working_dir() / ".crd_platform_test_tmp";
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
    REQUIRE(fs::file_size(file) == 11u);
    REQUIRE(fs::last_modified_unix_seconds(file) > 0);

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
    REQUIRE(out.size() == 6u);
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        REQUIRE(out[i] == bytes[i]);
    }

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
    REQUIRE(entries.size() == 2u);

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

TEST_CASE("filesystem: user_config_dir appends app name", "[platform][fs]")
{
    const auto cfg = fs::user_config_dir("CeridTests");
    REQUIRE_FALSE(cfg.empty());
    REQUIRE(cfg.generic().find("CeridTests") != crd::containers::StringView::npos);
}
