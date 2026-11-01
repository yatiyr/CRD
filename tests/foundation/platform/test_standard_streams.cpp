// crd-platform -- the standard streams of a byte protocol (crd/platform/standard_streams.hpp). The test points the
// process's stdin at an empty file (whatever input the runner gave it) and its stdout and stderr at two files, claims
// the protocol streams, then writes a protocol line with a newline and a 0x1A byte to the claimed stream and a stray
// line to stdout: the output file must hold exactly the protocol's bytes (no "\r\n", nothing stray), the error file
// the stray line. ceridc's stdio case (tests/tools/ceridc/test_ceridc.cpp)
// and its conformance run show the same through a real process.

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/platform/standard_streams.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
#if defined(_WIN32)
int descriptor(std::FILE* stream)
{
    return _fileno(stream);
}
int duplicate(int fd)
{
    return _dup(fd);
}
bool point(int from, int to)
{
    return _dup2(from, to) == 0;
}
void close_fd(int fd)
{
    (void)_close(fd);
}
std::FILE* open_file(const std::filesystem::path& path, const char* mode)
{
    std::FILE* f = nullptr;
    return _wfopen_s(&f, path.c_str(), mode[0] == 'r' ? L"rb" : L"wb") == 0 ? f : nullptr;
}
#else
int descriptor(std::FILE* stream)
{
    return ::fileno(stream);
}
int duplicate(int fd)
{
    return ::dup(fd);
}
bool point(int from, int to)
{
    return ::dup2(from, to) == to;
}
void close_fd(int fd)
{
    (void)::close(fd);
}
std::FILE* open_file(const std::filesystem::path& path, const char* mode)
{
    return std::fopen(path.c_str(), mode);
}
#endif

[[nodiscard]] crd::containers::String read_all(const std::filesystem::path& path)
{
    crd::containers::String text;
    if (std::FILE* f = open_file(path, "rb"))
    {
        char buf[256];
        for (std::size_t n = std::fread(buf, 1, sizeof(buf), f); n > 0U; n = std::fread(buf, 1, sizeof(buf), f))
        {
            text.append(buf, n);
        }
        (void)std::fclose(f);
    }
    return text;
}
} // namespace

TEST_CASE("platform: the protocol owns its output stream and stray writes go to stderr", "[platform][streams]")
{
    const std::filesystem::path dir      = std::filesystem::temp_directory_path();
    const std::filesystem::path out_path = dir / "crd_platform_streams_out.bin";
    const std::filesystem::path err_path = dir / "crd_platform_streams_err.bin";
    const std::filesystem::path in_path  = dir / "crd_platform_streams_in.bin";
    std::FILE* const            created  = open_file(in_path, "wb"); // an empty input: a runner may give no stdin
    REQUIRE(created != nullptr);
    (void)std::fclose(created);
    std::FILE* const in_file  = open_file(in_path, "rb");
    std::FILE* const out_file = open_file(out_path, "wb");
    std::FILE* const err_file = open_file(err_path, "wb");
    REQUIRE(in_file != nullptr);
    REQUIRE(out_file != nullptr);
    REQUIRE(err_file != nullptr);

    (void)std::fflush(stdout);
    (void)std::fflush(stderr);
    const int saved_in  = duplicate(descriptor(stdin));
    const int saved_out = duplicate(descriptor(stdout));
    const int saved_err = duplicate(descriptor(stderr));
    REQUIRE(saved_out != -1);
    REQUIRE(saved_err != -1);
    const bool redirected = point(descriptor(in_file), descriptor(stdin)) &&
                            point(descriptor(out_file), descriptor(stdout)) &&
                            point(descriptor(err_file), descriptor(stderr));

    std::FILE* const protocol = redirected ? crd::platform::claim_protocol_streams() : nullptr;
    if (protocol != nullptr)
    {
        (void)std::fputs("{\"line\":1}\n\x1A{\"line\":2}\n", protocol);
        (void)std::fflush(protocol);
        (void)std::fclose(protocol);
    }
    (void)std::fputs("stray\n", stdout);
    (void)std::fflush(stdout);
    (void)std::fflush(stderr);

    // Restore the test runner's own streams before any assertion reports.
    const bool restored = point(saved_out, descriptor(stdout)) && point(saved_err, descriptor(stderr)) &&
                          (saved_in == -1 || point(saved_in, descriptor(stdin)));
    if (saved_in != -1)
    {
        close_fd(saved_in);
    }
    close_fd(saved_out);
    close_fd(saved_err);
    (void)std::fclose(in_file);
    (void)std::fclose(out_file);
    (void)std::fclose(err_file);
    REQUIRE(redirected);
    REQUIRE(restored);
    REQUIRE(protocol != nullptr);

    const crd::containers::String out_text = read_all(out_path);
    const crd::containers::String err_text = read_all(err_path);
    CHECK(crd::containers::StringView{out_text.data(), out_text.size()} ==
          crd::containers::StringView{"{\"line\":1}\n\x1A{\"line\":2}\n"}); // the protocol's bytes, exactly
    CHECK(crd::containers::StringView{err_text.data(), err_text.size()}.rfind("stray", 0) == 0U); // off the protocol
    std::error_code ignored;
    std::filesystem::remove(out_path, ignored);
    std::filesystem::remove(err_path, ignored);
    std::filesystem::remove(in_path, ignored);
}
