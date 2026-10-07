#include "bounded_file.hpp"

#include <cstdio>

namespace crd::ceir::cook::detail
{
namespace
{
[[nodiscard]] std::FILE* open_binary(const char* path) noexcept
{
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, "rb") != 0)
    {
        return nullptr;
    }
#else
    fp = std::fopen(path, "rb");
#endif
    return fp;
}

// The file's size without reading it, or -1.
[[nodiscard]] crd::i64 size_of(std::FILE* fp) noexcept
{
#if defined(_MSC_VER)
    if (_fseeki64(fp, 0, SEEK_END) != 0)
    {
        return -1;
    }
    const crd::i64 size = _ftelli64(fp);
    if (_fseeki64(fp, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#else
    if (fseeko(fp, 0, SEEK_END) != 0)
    {
        return -1;
    }
    const auto size = static_cast<crd::i64>(ftello(fp));
    if (fseeko(fp, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#endif
    return size;
}
} // namespace

void append_decimal(containers::String& out, crd::u64 v)
{
    char       buf[20];
    crd::usize n = 0U;
    do
    {
        buf[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U)
    {
        out.push_back(buf[--n]);
    }
}

perf::DiagStatus read_bounded_file(containers::StringView path, crd::u64 max_bytes, containers::Array<crd::u8>& out,
                                   containers::String& reason, std::atomic<crd::u64>& bytes_read)
{
    const containers::String name(path.data(), path.size(), out.allocator());
    std::FILE* const         fp = open_binary(name.c_str());
    if (fp == nullptr)
    {
        reason.append("cannot open the program");
        return perf::DiagStatus::Failed;
    }
    const crd::i64 size = size_of(fp);
    if (size < 0)
    {
        (void)std::fclose(fp);
        reason.append("cannot size the program");
        return perf::DiagStatus::Failed;
    }
    // The bound is checked on the file's size, before a byte is read.
    if (static_cast<crd::u64>(size) > max_bytes)
    {
        (void)std::fclose(fp);
        reason.append("the program is ");
        append_decimal(reason, static_cast<crd::u64>(size));
        reason.append(" bytes; the host's limit is ");
        append_decimal(reason, max_bytes);
        return perf::DiagStatus::Oversized;
    }

    out.resize(static_cast<crd::usize>(size));
    const crd::usize read = size > 0 ? std::fread(out.data(), 1U, out.size(), fp) : 0U;
    (void)std::fclose(fp);
    bytes_read.fetch_add(read, std::memory_order_relaxed);
    if (read != out.size())
    {
        reason.append("short read of the program");
        return perf::DiagStatus::Failed;
    }
    return perf::DiagStatus::Ok;
}
} // namespace crd::ceir::cook::detail
