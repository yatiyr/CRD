#include "bounded_file.hpp"

#include <cstdio>

#if defined(_MSC_VER)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
// Open `path` for writing only if it does not exist yet (O_CREAT | O_EXCL), so no existing file is ever truncated.
[[nodiscard]] std::FILE* create_exclusive(const char* path) noexcept
{
#if defined(_MSC_VER)
    int fd = -1;
    if (_sopen_s(&fd, path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0)
    {
        return nullptr;
    }
    std::FILE* const fp = _fdopen(fd, "wb");
    if (fp == nullptr)
    {
        (void)_close(fd);
    }
    return fp;
#else
    const int fd = ::open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
    {
        return nullptr;
    }
    std::FILE* const fp = ::fdopen(fd, "wb");
    if (fp == nullptr)
    {
        (void)::close(fd);
    }
    return fp;
#endif
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
                                   containers::String& reason, std::atomic<crd::u64>& bytes_read,
                                   containers::StringView what)
{
    const containers::String name(path.data(), path.size(), out.allocator());
    std::FILE* const         fp = open_binary(name.c_str());
    if (fp == nullptr)
    {
        reason.append("cannot open ");
        reason.append(what);
        return perf::DiagStatus::Failed;
    }
    const crd::i64 size = size_of(fp);
    if (size < 0)
    {
        (void)std::fclose(fp);
        reason.append("cannot size ");
        reason.append(what);
        return perf::DiagStatus::Failed;
    }
    // The bound is checked on the file's size, before a byte is read.
    if (static_cast<crd::u64>(size) > max_bytes)
    {
        (void)std::fclose(fp);
        reason.append(what);
        reason.append(" is ");
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
        reason.append("short read of ");
        reason.append(what);
        return perf::DiagStatus::Failed;
    }
    return perf::DiagStatus::Ok;
}
bool file_exists(containers::StringView path)
{
    const containers::String name(path.data(), path.size());
    std::FILE* const         fp = open_binary(name.c_str());
    if (fp == nullptr)
    {
        return false;
    }
    (void)std::fclose(fp);
    return true;
}

perf::DiagStatus write_new_file(containers::StringView path, containers::ConstSpan<crd::u8> bytes,
                                containers::String& reason)
{
    const containers::String name(path.data(), path.size(), reason.allocator());
    if (file_exists(path))
    {
        reason.append("refusing to overwrite an existing file");
        return perf::DiagStatus::Failed;
    }
    std::FILE* const fp = create_exclusive(name.c_str());
    if (fp == nullptr)
    {
        reason.append("cannot create the file (it may exist, or its folder may not)");
        return perf::DiagStatus::Failed;
    }
    const crd::usize written = bytes.empty() ? 0U : std::fwrite(bytes.data(), 1U, bytes.size(), fp);
    const bool       closed  = std::fclose(fp) == 0;
    if (written != bytes.size() || !closed)
    {
        (void)std::remove(name.c_str());
        reason.append("short write of the file");
        return perf::DiagStatus::Failed;
    }
    return perf::DiagStatus::Ok;
}
} // namespace crd::ceir::cook::detail
