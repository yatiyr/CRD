// The replay driver of every fuzz target (REPO.DEV.9; docs/design/test-instruments.md).
//
//   <target> <corpus directory or file>...     replay every file through LLVMFuzzerTestOneInput; exit 1 on a failure
//   <target> --seed <directory>                write the target's deterministic seed inputs as corpus files
//   <target> --verbose ...                     one line per input
//
// A crash, a fatal, a sanitizer report or an oracle failure ends the process, which is the failure the CTest sees;
// a target returning non-zero counts as "rejected by the target" and is reported, never hidden. Timing per input is
// measured and the slowest input is named, so a slow loader path is visible before it becomes a libFuzzer timeout.
#include <crd/fuzz/harness.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

struct Outcome
{
    std::size_t inputs   = 0U;
    std::size_t bytes    = 0U;
    std::size_t rejected = 0U;
    double      slowest_ms = 0.0;
    std::string slowest_name;
};

[[nodiscard]] std::vector<std::uint8_t> read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void replay_one(const fs::path& path, bool verbose, Outcome& outcome)
{
    const std::vector<std::uint8_t> bytes = read_file(path);
    const auto                      started = std::chrono::steady_clock::now();
    const int result = LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    const double ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    outcome.inputs += 1U;
    outcome.bytes += bytes.size();
    if (result != 0)
    {
        outcome.rejected += 1U;
    }
    if (ms > outcome.slowest_ms)
    {
        outcome.slowest_ms   = ms;
        outcome.slowest_name = path.filename().string();
    }
    if (verbose)
    {
        std::printf("%-44s %8zu bytes %8.2f ms%s\n", path.filename().string().c_str(), bytes.size(), ms,
                    result != 0 ? "  (rejected by the target)" : "");
    }
}

class FileSink final : public crd::fuzz::SeedSink
{
public:
    explicit FileSink(fs::path directory) : m_directory(std::move(directory)) {}
    void add(const char* name, const crd::u8* data, crd::usize size) override
    {
        // Content-hash file names (FNV-1a 64) so a seed's identity is its bytes; libFuzzer -merge renames to SHA-1.
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (crd::usize i = 0U; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ULL;
        }
        char digits[17];
        std::snprintf(digits, sizeof(digits), "%016llx", static_cast<unsigned long long>(hash));
        const fs::path path = m_directory / (std::string("seed-") + digits);
        std::ofstream  stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        std::printf("seed %-28s %6zu bytes  %s\n", name, static_cast<std::size_t>(size), path.filename().string().c_str());
        m_written += 1U;
    }
    [[nodiscard]] std::size_t written() const noexcept { return m_written; }

private:
    fs::path    m_directory;
    std::size_t m_written = 0U;
};
} // namespace

int main(int argc, char** argv)
{
    bool                  verbose = false;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--verbose") == 0)
        {
            verbose = true;
        }
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
        {
            const fs::path directory(argv[++i]);
            fs::create_directories(directory);
            FileSink sink(directory);
            crd_fuzz_seeds(sink);
            std::printf("fuzz seeds: %zu written to %s\n", sink.written(), directory.string().c_str());
            return 0;
        }
        else
        {
            inputs.emplace_back(argv[i]);
        }
    }
    if (inputs.empty())
    {
        std::fprintf(stderr, "usage: %s [--verbose] <corpus directory or file>... | --seed <directory>\n", argv[0]);
        return 2;
    }
    Outcome outcome;
    for (const fs::path& input : inputs)
    {
        if (fs::is_directory(input))
        {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(input))
            {
                if (entry.is_regular_file())
                {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files)
            {
                replay_one(file, verbose, outcome);
            }
        }
        else if (fs::is_regular_file(input))
        {
            replay_one(input, verbose, outcome);
        }
        else
        {
            std::fprintf(stderr, "fuzz replay: %s is neither a directory nor a file\n", input.string().c_str());
            return 2;
        }
    }
    if (outcome.inputs == 0U)
    {
        std::fprintf(stderr, "fuzz replay: the corpus is empty; a corpus test with nothing to replay proves nothing\n");
        return 1;
    }
    std::printf("fuzz replay: %zu inputs, %zu bytes, %zu rejected by the target, slowest %.2f ms (%s)\n",
                outcome.inputs, outcome.bytes, outcome.rejected, outcome.slowest_ms, outcome.slowest_name.c_str());
    return 0;
}
