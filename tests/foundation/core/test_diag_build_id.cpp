// DIAG.5d(c2): the Linux crash record carries the ELF build-id of the crashing executable (and a bounded module
// list), captured at install() and verified from OUTSIDE the process. The load-bearing check is INDEPENDENT: the
// build-id decoded from the on-disk crash_*.log record is compared against the specimen binary's .note.gnu.build-id
// parsed directly from the file (in-memory-at-install vs on-disk-ELF) -- the identity a later symbolizer matches or
// refuses. Windows RSDS identity is DIAG.5d(c1); turning this record into a SymbolIndex section is a later composer.

#include <crd/diag/specimen_runner.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <filesystem>
#include <string_view>

namespace cont = crd::containers;
namespace cd   = crd::diag;
namespace fs   = std::filesystem;

namespace
{
bool built_with_asan()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

cont::String specimen_path()
{
    return cont::String{CRD_DIAG_CRASH_CAPTURE_SPECIMEN};
}

std::atomic<unsigned> g_uniq{0};

fs::path fresh_dir(const char* mode)
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    std::snprintf(name, sizeof(name), "crd_bid_%s_%u", mode, g_uniq.fetch_add(1U));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

cd::Outcome run_mode(const char* mode, const fs::path& dir)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{mode});
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 15000U;
    return cd::run_specimen(specimen_path(), args, e);
}

fs::path first_crash_record(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".log" && it->path().filename().string().rfind("crash_", 0) == 0)
        {
            return it->path();
        }
    }
    return {};
}

cont::String read_text(const char* path)
{
    cont::String out;
    std::FILE*   f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return out;
    }
    char   buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) != 0)
    {
        out.append(buf, got);
    }
    std::fclose(f);
    return out;
}

int hexval(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

std::size_t hex_decode(std::string_view hex, unsigned char* out, std::size_t cap)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i + 1 < hex.size() + 1 && n < cap; i += 2)
    {
        if (i + 1 >= hex.size())
        {
            break;
        }
        const int hi = hexval(hex[i]);
        const int lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            break;
        }
        out[n++] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return n;
}

// The value token after `key` (which includes its trailing space), up to the next space or newline.
std::string_view value_after(std::string_view sv, const char* key)
{
    const std::size_t pos = sv.find(key);
    if (pos == std::string_view::npos)
    {
        return {};
    }
    const std::size_t start = pos + std::strlen(key);
    std::size_t       end   = sv.find_first_of(" \n", start);
    if (end == std::string_view::npos)
    {
        end = sv.size();
    }
    return sv.substr(start, end - start);
}

// Parse the specimen binary's .note.gnu.build-id straight from the ELF FILE (on-disk, independent of the running
// process). Returns the build-id byte count copied into out (0 if absent).
std::size_t parse_elf_build_id(const char* path, unsigned char* out, std::size_t out_cap)
{
    cont::Array<crd::u8> f;
    {
        std::FILE* fp = std::fopen(path, "rb");
        if (fp == nullptr)
        {
            return 0;
        }
        std::fseek(fp, 0, SEEK_END);
        const long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz > 0)
        {
            f.resize_uninitialized(static_cast<crd::usize>(sz));
            const crd::usize got = std::fread(f.data(), 1, static_cast<crd::usize>(sz), fp);
            f.resize(got);
        }
        std::fclose(fp);
    }
    if (f.size() < sizeof(Elf64_Ehdr))
    {
        return 0;
    }
    Elf64_Ehdr eh{};
    std::memcpy(&eh, f.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
    {
        return 0;
    }
    for (int i = 0; i < eh.e_phnum; ++i)
    {
        const std::size_t poff = static_cast<std::size_t>(eh.e_phoff) + static_cast<std::size_t>(i) * eh.e_phentsize;
        if (poff + sizeof(Elf64_Phdr) > f.size())
        {
            break;
        }
        Elf64_Phdr ph{};
        std::memcpy(&ph, f.data() + poff, sizeof(ph));
        if (ph.p_type != PT_NOTE)
        {
            continue;
        }
        std::size_t off = static_cast<std::size_t>(ph.p_offset);
        std::size_t rem = static_cast<std::size_t>(ph.p_filesz);
        if (off > f.size())
        {
            continue;
        }
        if (off + rem > f.size())
        {
            rem = f.size() - off;
        }
        while (rem >= sizeof(Elf64_Nhdr))
        {
            Elf64_Nhdr nh{};
            std::memcpy(&nh, f.data() + off, sizeof(nh));
            const std::size_t name_pad = (static_cast<std::size_t>(nh.n_namesz) + 3U) & ~static_cast<std::size_t>(3);
            const std::size_t desc_pad = (static_cast<std::size_t>(nh.n_descsz) + 3U) & ~static_cast<std::size_t>(3);
            const std::size_t total    = sizeof(Elf64_Nhdr) + name_pad + desc_pad;
            if (total > rem)
            {
                break;
            }
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4U &&
                std::memcmp(f.data() + off + sizeof(Elf64_Nhdr), "GNU", 4) == 0)
            {
                std::size_t take = nh.n_descsz;
                if (take > out_cap)
                {
                    take = out_cap;
                }
                std::memcpy(out, f.data() + off + sizeof(Elf64_Nhdr) + name_pad, take);
                return take;
            }
            off += total;
            rem -= total;
        }
    }
    return 0;
}
} // namespace

TEST_CASE("crash record (linux): carries the exe ELF build-id matching the on-disk binary", "[core][diag][build-id]")
{
    const fs::path    dir = fresh_dir("bid");
    const cd::Outcome o   = run_mode("av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan owns SIGSEGV on Linux; no crd record is written (asserted, never skipped)
        std::error_code ec;
        fs::remove_all(dir, ec);
        return;
    }

    REQUIRE(o.verdict == cd::Verdict::Crashed);
    const fs::path rec = first_crash_record(dir);
    REQUIRE_FALSE(rec.empty());

    const cont::String     text = read_text(rec.string().c_str());
    const std::string_view sv{text.c_str(), text.size()};

    // The exe build-id line, decoded from the record.
    const std::string_view bid_hex = value_after(sv, "build_id ");
    REQUIRE_FALSE(bid_hex.empty());
    unsigned char    rec_id[64];
    const std::size_t rec_len = hex_decode(bid_hex, rec_id, sizeof(rec_id));
    CHECK(rec_len == 20U); // GNU build-id is SHA-1 (20 bytes) in this toolchain

    // Independently parse the specimen binary's on-disk .note.gnu.build-id and compare.
    unsigned char    disk_id[64];
    const std::size_t disk_len = parse_elf_build_id(specimen_path().c_str(), disk_id, sizeof(disk_id));
    REQUIRE(disk_len > 0U);
    CHECK(rec_len == disk_len);
    CHECK(std::memcmp(rec_id, disk_id, rec_len) == 0); // in-memory-at-install == on-disk-ELF: the real identity

    // A bounded module list is present: at least the exe + libc, and every id token is well-formed hex or "-".
    const std::string_view mod_count_tok = value_after(sv, "modules ");
    REQUIRE_FALSE(mod_count_tok.empty());
    int modules = 0;
    for (char c : mod_count_tok)
    {
        if (c < '0' || c > '9')
        {
            break;
        }
        modules = modules * 10 + (c - '0');
    }
    CHECK(modules >= 2);

    // Walk every "module " line: the id column decodes cleanly (or is "-") -- no half-written identity.
    std::size_t scan = 0;
    int         seen = 0;
    int         with_id = 0;
    for (;;)
    {
        const std::size_t p = sv.find("module ", scan);
        if (p == std::string_view::npos)
        {
            break;
        }
        // The "modules N" header does not match "module " (the 7th char is 's', not a space), so every hit here is a
        // real module line. fields: module <base-hex> <id-hex-or-dash> <basename>
        const std::size_t base_start = p + 7;
        const std::size_t id_start   = sv.find(' ', base_start);
        if (id_start == std::string_view::npos)
        {
            break;
        }
        const std::size_t id_s = id_start + 1;
        std::size_t       id_e = sv.find_first_of(" \n", id_s);
        if (id_e == std::string_view::npos)
        {
            id_e = sv.size();
        }
        const std::string_view id = sv.substr(id_s, id_e - id_s);
        if (id != "-")
        {
            unsigned char    tmp[64];
            const std::size_t dl = hex_decode(id, tmp, sizeof(tmp));
            CHECK(dl * 2U == id.size()); // exact 2 hex digits per byte, nothing dropped
            CHECK(dl > 0U);
            ++with_id;
        }
        ++seen;
        scan = id_e;
    }
    CHECK(seen == modules);  // one "module " line per declared module
    CHECK(with_id >= 1);     // at least one module (the exe) carries a build-id

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif // defined(__linux__)
