#include "filesystem_detail.hpp"

#include <crd/containers/checksum.hpp>
#include <crd/core/platform.hpp>
#include <crd/platform/durable_file.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#if CRD_OS_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crd::platform::fs
{
namespace
{
namespace cont = crd::containers;

constexpr u32 kSaveMagic = 0x53445243U;    // "CRDS" in file order
constexpr u32 kJournalMagic = 0x4A445243U; // "CRDJ" in file order
constexpr u16 kFormatVersion = 1U;

// ---- little-endian fields -------------------------------------------------------------------------------------

void put16(u8* p, u16 v) noexcept
{
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8U);
}

void put32(u8* p, u32 v) noexcept
{
    for (u32 i = 0U; i < 4U; ++i)
    {
        p[i] = static_cast<u8>(v >> (8U * i));
    }
}

void put64(u8* p, u64 v) noexcept
{
    for (u32 i = 0U; i < 8U; ++i)
    {
        p[i] = static_cast<u8>(v >> (8U * i));
    }
}

[[nodiscard]] u16 get16(const u8* p) noexcept
{
    return static_cast<u16>(p[0] | (p[1] << 8U));
}

[[nodiscard]] u32 get32(const u8* p) noexcept
{
    u32 v = 0U;
    for (u32 i = 0U; i < 4U; ++i)
    {
        v |= static_cast<u32>(p[i]) << (8U * i);
    }
    return v;
}

[[nodiscard]] u64 get64(const u8* p) noexcept
{
    u64 v = 0U;
    for (u32 i = 0U; i < 8U; ++i)
    {
        v |= static_cast<u64>(p[i]) << (8U * i);
    }
    return v;
}

[[nodiscard]] bool all_zero(const u8* p, usize n) noexcept
{
    for (usize i = 0U; i < n; ++i)
    {
        if (p[i] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] u32 crc_of(const u8* p, usize n) noexcept
{
    return cont::crc32c(cont::ConstSpan<u8>{p, n});
}

[[nodiscard]] Path with_suffix(const Path& path, std::string_view suffix)
{
    cont::String name(path.generic());
    name.append(suffix);
    return Path{std::move(name)};
}

[[nodiscard]] Path slot_path(const Path& path, u32 slot)
{
    char suffix[16];
    const int n = std::snprintf(suffix, sizeof(suffix), ".%u", slot);
    return with_suffix(path, std::string_view(suffix, n > 0 ? static_cast<usize>(n) : 0U));
}

// ---- native durable writes ------------------------------------------------------------------------------------

#if CRD_OS_WINDOWS
[[nodiscard]] HANDLE handle_of(i64 file) noexcept
{
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(file)); // NOLINT(performance-no-int-to-ptr)
}

[[nodiscard]] bool write_all(HANDLE h, cont::ConstSpan<u8> bytes) noexcept
{
    usize done = 0U;
    while (done < bytes.size())
    {
        const usize left = bytes.size() - done;
        const DWORD chunk = left > (1U << 30U) ? (1U << 30U) : static_cast<DWORD>(left);
        DWORD wrote = 0U;
        if (WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) == 0 || wrote == 0U)
        {
            return false;
        }
        done += wrote;
    }
    return true;
}
#else
[[nodiscard]] bool write_all(int fd, cont::ConstSpan<u8> bytes) noexcept
{
    usize done = 0U;
    while (done < bytes.size())
    {
        const ssize_t wrote = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote < 0 && errno == EINTR)
        {
            continue;
        }
        if (wrote <= 0)
        {
            return false;
        }
        done += static_cast<usize>(wrote);
    }
    return true;
}

// Flush a descriptor's data and size to stable storage (macOS's fsync stops at the drive's cache; F_FULLFSYNC does
// not, and falls back to fsync where the file system refuses it).
[[nodiscard]] bool sync_fd(int fd) noexcept
{
#if CRD_OS_MAC
    if (::fcntl(fd, F_FULLFSYNC) == 0)
    {
        return true;
    }
#endif
    return ::fsync(fd) == 0;
}

// Make a rename inside `file`'s directory durable. A file system that cannot sync a directory answers EINVAL.
[[nodiscard]] bool sync_parent(const std::filesystem::path& file) noexcept
{
    try
    {
        std::filesystem::path dir = file.parent_path();
        if (dir.empty())
        {
            dir = ".";
        }
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
        {
            return false;
        }
        const bool ok = ::fsync(fd) == 0 || errno == EINVAL;
        (void)::close(fd);
        return ok;
    }
    catch (...)
    {
        return false;
    }
}
#endif

// Write `head` then `body` to a new `path` and flush them to stable storage before returning.
[[nodiscard]] bool write_durable(const Path& path, cont::ConstSpan<u8> head, cont::ConstSpan<u8> body) noexcept
{
    try
    {
        const std::filesystem::path native = detail::native_path(path);
#if CRD_OS_WINDOWS
        const HANDLE h =
            CreateFileW(native.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        bool ok = write_all(h, head) && write_all(h, body) && FlushFileBuffers(h) != 0;
        ok = (CloseHandle(h) != 0) && ok;
#else
        const int fd = ::open(native.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            return false;
        }
        bool ok = write_all(fd, head) && write_all(fd, body) && sync_fd(fd);
        ok = (::close(fd) == 0) && ok;
#endif
        return ok;
    }
    catch (...)
    {
        return false;
    }
}

// Rename `from` over `to` and make the rename durable.
[[nodiscard]] bool replace_durable(const Path& from, const Path& to) noexcept
{
    try
    {
        const std::filesystem::path source = detail::native_path(from);
        const std::filesystem::path target = detail::native_path(to);
#if CRD_OS_WINDOWS
        return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return ::rename(source.c_str(), target.c_str()) == 0 && sync_parent(target);
#endif
    }
    catch (...)
    {
        return false;
    }
}

// ---- save envelope --------------------------------------------------------------------------------------------

struct SaveHeader
{
    u64 generation = 0U;
    u64 size = 0U;
    u64 hash = 0U;
};

void encode_save_header(u8* h, u64 generation, cont::ConstSpan<u8> payload) noexcept
{
    std::memset(h, 0, kSaveHeaderBytes);
    put32(h, kSaveMagic);
    put16(h + 4, kFormatVersion);
    put16(h + 6, static_cast<u16>(kSaveHeaderBytes));
    put64(h + 8, generation);
    put64(h + 16, payload.size());
    put64(h + 24, cont::xxh3_64(payload));
    put32(h + 36, crc_of(h, 36U));
}

// The header's own checks; `file_bytes` is the file's size.
[[nodiscard]] SaveFault check_save_header(const u8* h, u64 file_bytes, SaveHeader& out) noexcept
{
    if (file_bytes < kSaveHeaderBytes)
    {
        return SaveFault::Truncated;
    }
    if (get32(h + 36) != crc_of(h, 36U) || get32(h) != kSaveMagic || get16(h + 4) != kFormatVersion ||
        get16(h + 6) != kSaveHeaderBytes || !all_zero(h + 32, 4U) || get64(h + 8) == 0U)
    {
        return SaveFault::HeaderCorrupt;
    }
    out.generation = get64(h + 8);
    out.size = get64(h + 16);
    out.hash = get64(h + 24);
    if (file_bytes < kSaveHeaderBytes + out.size)
    {
        return SaveFault::Truncated;
    }
    return file_bytes == kSaveHeaderBytes + out.size ? SaveFault::None : SaveFault::PayloadCorrupt;
}

// Read `path`'s header, once more when the read or a check fails.
[[nodiscard]] SaveFault read_save_header(const Path& path, SaveHeader& out, u32& rereads) noexcept
{
    SaveFault fault = SaveFault::Unreadable;
    for (u32 attempt = 0U; attempt < 2U; ++attempt)
    {
        const u64 bytes = file_size(path);
        if (bytes < kSaveHeaderBytes)
        {
            return SaveFault::Truncated; // the size is the file system's, not a read's: reading again cannot help
        }
        cont::Array<u8> head;
        fault = read_file_range(path, 0U, kSaveHeaderBytes, head) ? check_save_header(head.data(), bytes, out)
                                                                  : SaveFault::Unreadable;
        if (fault == SaveFault::None)
        {
            return fault;
        }
        rereads += attempt == 0U ? 1U : 0U;
    }
    return fault;
}

// Read all of `path` and verify it is generation `generation`; the payload is left in `payload`. Reads once more when
// the read or a check fails.
[[nodiscard]] SaveFault read_save(const Path& path, u64 generation, cont::Array<u8>& payload, u32& rereads) noexcept
{
    SaveFault fault = SaveFault::Unreadable;
    for (u32 attempt = 0U; attempt < 2U; ++attempt)
    {
        SaveHeader header;
        if (!read_file_binary(path, payload))
        {
            fault = SaveFault::Unreadable;
        }
        else
        {
            fault = payload.size() < kSaveHeaderBytes ? SaveFault::Truncated
                                                      : check_save_header(payload.data(), payload.size(), header);
            if (fault == SaveFault::None && header.generation != generation)
            {
                fault = SaveFault::HeaderCorrupt; // not the file whose header was read
            }
            if (fault == SaveFault::None &&
                cont::xxh3_64(cont::ConstSpan<u8>{payload.data() + kSaveHeaderBytes, header.size}) != header.hash)
            {
                fault = SaveFault::PayloadCorrupt;
            }
        }
        if (fault == SaveFault::None)
        {
            std::memmove(payload.data(), payload.data() + kSaveHeaderBytes, payload.size() - kSaveHeaderBytes);
            payload.resize(payload.size() - kSaveHeaderBytes);
            return fault;
        }
        rereads += attempt == 0U ? 1U : 0U;
    }
    payload.clear();
    return fault;
}

enum class SlotKind : u8
{
    Main = 0U,
    Pending = 1U, // `path.tmp`: a save whose rename has not happened
    Older = 2U,
};

struct SaveFile
{
    Path path{};
    SlotKind kind = SlotKind::Main;
    SaveFault fault = SaveFault::None;
    SaveHeader header{};
};

// Every existing file of the save, its header read.
void scan_save(const Path& path, u32 kept, cont::Array<SaveFile>& files, u32& rereads)
{
    const auto add = [&](Path candidate, SlotKind kind)
    {
        if (is_file(candidate))
        {
            SaveFile file;
            file.path = std::move(candidate);
            file.kind = kind;
            file.fault = read_save_header(file.path, file.header, rereads);
            files.push_back(std::move(file));
        }
    };
    add(path, SlotKind::Main);
    add(with_suffix(path, ".tmp"), SlotKind::Pending);
    for (u32 slot = 1U; slot <= kept; ++slot)
    {
        add(slot_path(path, slot), SlotKind::Older);
    }
}

// ---- journal format -------------------------------------------------------------------------------------------

void encode_journal_header(u8* h, u64 base_generation) noexcept
{
    std::memset(h, 0, kJournalHeaderBytes);
    put32(h, kJournalMagic);
    put16(h + 4, kFormatVersion);
    put16(h + 6, static_cast<u16>(kJournalHeaderBytes));
    put64(h + 8, base_generation);
    put32(h + 28, crc_of(h, 28U));
}

void encode_record_header(u8* h, u64 sequence, cont::ConstSpan<u8> record) noexcept
{
    put64(h, sequence);
    put64(h + 8, cont::xxh3_64(record));
    put32(h + 16, static_cast<u32>(record.size()));
    put32(h + 20, crc_of(h, 20U));
}

// Parse `r.bytes` into `r`.
void parse_journal(JournalReplay& r, u64 base_generation) noexcept
{
    r.records.clear();
    r.valid_bytes = 0U;
    r.lost_bytes = 0U;
    const u8* b = r.bytes.data();
    const usize n = r.bytes.size();
    if (n < kJournalHeaderBytes || get32(b + 28) != crc_of(b, 28U) || get32(b) != kJournalMagic ||
        get16(b + 4) != kFormatVersion || get16(b + 6) != kJournalHeaderBytes || !all_zero(b + 16, 12U))
    {
        r.status = JournalStatus::HeaderCorrupt;
        r.base_generation = 0U;
        r.lost_bytes = n;
        return;
    }
    r.base_generation = get64(b + 8);
    if (r.base_generation != base_generation)
    {
        r.status = JournalStatus::OtherBase;
        return;
    }
    usize at = kJournalHeaderBytes;
    while (n - at >= kJournalRecordHeaderBytes)
    {
        const u8* h = b + at;
        const u64 size = get32(h + 16);
        if (get32(h + 20) != crc_of(h, 20U) || get64(h) != r.records.size() + 1U ||
            n - at - kJournalRecordHeaderBytes < size ||
            cont::xxh3_64(cont::ConstSpan<u8>{h + kJournalRecordHeaderBytes, static_cast<usize>(size)}) != get64(h + 8))
        {
            break;
        }
        r.records.push_back(JournalRecord{get64(h), at + kJournalRecordHeaderBytes, static_cast<usize>(size)});
        at += kJournalRecordHeaderBytes + static_cast<usize>(size);
    }
    r.valid_bytes = at;
    r.lost_bytes = n - at;
    r.status = at == n ? JournalStatus::Ok : JournalStatus::TornTail;
}

} // namespace

// ---- atomic replace -------------------------------------------------------------------------------------------

bool write_file_atomic(const Path& path, cont::ConstSpan<u8> contents) noexcept
{
    try
    {
        const Path tmp = with_suffix(path, ".tmp");
        if (write_durable(tmp, contents, {}) && replace_durable(tmp, path))
        {
            return true;
        }
        (void)remove_file(tmp);
        return false;
    }
    catch (...)
    {
        return false;
    }
}

// ---- generational saves ---------------------------------------------------------------------------------------

SaveWrite save_generation(const Path& path, cont::ConstSpan<u8> payload, u32 kept) noexcept
{
    SaveWrite out;
    try
    {
        u32 rereads = 0U;
        cont::Array<SaveFile> files;
        scan_save(path, kept, files, rereads);

        u64 newest = 0U;
        SaveFile* main = nullptr;
        SaveFile* pending = nullptr;
        for (SaveFile& file : files)
        {
            if (file.fault == SaveFault::None && file.header.generation > newest)
            {
                newest = file.header.generation;
            }
            main = file.kind == SlotKind::Main ? &file : main;
            pending = file.kind == SlotKind::Pending ? &file : pending;
        }

        // The main file goes into the ring only when it verifies; a damaged one is kept aside, never in place of a
        // good older generation.
        cont::Array<u8> scratch;
        bool main_good = main != nullptr && main->fault == SaveFault::None &&
                         read_save(main->path, main->header.generation, scratch, rereads) == SaveFault::None;

        // Move the main file out of the way: into the ring's slot of its generation (which holds the oldest kept
        // one) when it verifies, aside to `path.damaged` when it does not.
        const auto set_main_aside = [&]() -> bool
        {
            if (main == nullptr)
            {
                return true;
            }
            if (!main_good)
            {
                return replace_durable(path, with_suffix(path, ".damaged"));
            }
            if (kept == 0U)
            {
                return true;
            }
            return replace_durable(path, slot_path(path, static_cast<u32>(main->header.generation % kept) + 1U));
        };

        // A save interrupted after its bytes were durable and before its rename is completed first, so its
        // generation enters the ring like any other.
        const bool pending_newest = pending != nullptr && pending->fault == SaveFault::None &&
                                    (!main_good || pending->header.generation > main->header.generation);
        if (pending_newest && read_save(pending->path, pending->header.generation, scratch, rereads) == SaveFault::None)
        {
            if (!set_main_aside() || !replace_durable(pending->path, path))
            {
                return out;
            }
            main_good = true;
            main = pending;
        }

        const u64 generation = newest + 1U;
        u8 header[kSaveHeaderBytes];
        encode_save_header(header, generation, payload);
        const Path tmp = with_suffix(path, ".tmp");
        if (!write_durable(tmp, cont::ConstSpan<u8>{header, kSaveHeaderBytes}, payload))
        {
            (void)remove_file(tmp);
            return out;
        }
        if (!set_main_aside())
        {
            (void)remove_file(tmp);
            return out;
        }
        // A failure here leaves the new save durable as `path.tmp`, which a load delivers.
        if (!replace_durable(tmp, path))
        {
            return out;
        }
        out.ok = true;
        out.generation = generation;
        return out;
    }
    catch (...)
    {
        return out;
    }
}

SaveLoad load_generation(const Path& path, u32 kept, cont::Array<u8>& payload) noexcept
{
    SaveLoad out;
    payload.clear();
    try
    {
        cont::Array<SaveFile> files;
        scan_save(path, kept, files, out.rereads);
        if (files.empty())
        {
            return out;
        }
        SaveFault main_fault = SaveFault::Missing;
        u64 main_generation = 0U; // 0: no main file, or its header failed
        for (const SaveFile& file : files)
        {
            if (file.kind == SlotKind::Main)
            {
                main_fault = file.fault;
                main_generation = file.fault == SaveFault::None ? file.header.generation : 0U;
            }
        }
        const bool has_main = main_fault != SaveFault::Missing;
        // Newest first among the files whose header verified.
        for (;;)
        {
            SaveFile* best = nullptr;
            for (SaveFile& file : files)
            {
                if (file.fault == SaveFault::None &&
                    (best == nullptr || file.header.generation > best->header.generation))
                {
                    best = &file;
                }
            }
            if (best == nullptr)
            {
                break;
            }
            best->fault = read_save(best->path, best->header.generation, payload, out.rereads);
            if (best->fault == SaveFault::None)
            {
                // A pending save is the newest work when no main file exists or the main file is older.
                const bool newest =
                    best->kind == SlotKind::Main ||
                    (best->kind == SlotKind::Pending &&
                     (!has_main || (main_generation != 0U && main_generation < best->header.generation)));
                out.status = newest ? SaveStatus::Ok : SaveStatus::Recovered;
                out.generation = best->header.generation;
                out.fault = out.status == SaveStatus::Recovered ? main_fault : SaveFault::None;
                out.source = best->path;
                return out;
            }
            main_fault = best->kind == SlotKind::Main ? best->fault : main_fault;
        }
        out.status = SaveStatus::Corrupt;
        out.fault = main_fault;
        payload.clear();
        return out;
    }
    catch (...)
    {
        payload.clear();
        out.status = SaveStatus::Corrupt;
        out.fault = SaveFault::Unreadable;
        return out;
    }
}

const char* save_fault_name(SaveFault fault) noexcept
{
    switch (fault)
    {
        case SaveFault::None:
            return "none";
        case SaveFault::Missing:
            return "the file is missing";
        case SaveFault::Unreadable:
            return "the file cannot be read";
        case SaveFault::Truncated:
            return "the file is shorter than it should be";
        case SaveFault::HeaderCorrupt:
            return "its header failed its checksum";
        case SaveFault::PayloadCorrupt:
            return "its contents failed their checksum";
    }
    return "unknown";
}

cont::String save_load_message(const Path& path, const SaveLoad& load)
{
    cont::String text;
    char number[32];
    (void)std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(load.generation));
    switch (load.status)
    {
        case SaveStatus::Ok:
            if (load.rereads != 0U)
            {
                text.append(path.generic());
                text.append(": a read failed its checks and was repeated; save ");
                text.append(number);
                text.append(" verified");
            }
            break;
        case SaveStatus::Recovered:
            text.append(path.generic());
            text.append(": ");
            text.append(save_fault_name(load.fault));
            text.append(", even when read again; loaded the previous save ");
            text.append(number);
            text.append(" from ");
            text.append(load.source.generic());
            text.append(" (the newest work since then is lost)");
            break;
        case SaveStatus::NotFound:
            text.append(path.generic());
            text.append(": no saved file");
            break;
        case SaveStatus::Corrupt:
            text.append(path.generic());
            text.append(": ");
            text.append(save_fault_name(load.fault));
            text.append(", and no previous save verifies");
            break;
    }
    return text;
}

// ---- journal --------------------------------------------------------------------------------------------------

JournalReplay replay_journal(const Path& path, u64 base_generation) noexcept
{
    JournalReplay r;
    try
    {
        for (u32 attempt = 0U; attempt < 2U; ++attempt)
        {
            if (read_file_binary(path, r.bytes))
            {
                parse_journal(r, base_generation);
            }
            else
            {
                // One read either way, so a missing journal costs what a present one does.
                r.records.clear();
                r.status = exists(path) ? JournalStatus::Unreadable : JournalStatus::NotFound;
            }
            if (r.status == JournalStatus::Ok || r.status == JournalStatus::OtherBase ||
                r.status == JournalStatus::NotFound || attempt == 1U)
            {
                break;
            }
            ++r.rereads;
        }
        return r;
    }
    catch (...)
    {
        r.records.clear();
        r.status = JournalStatus::Unreadable;
        return r;
    }
}

Journal::~Journal()
{
    close();
}

Journal::Journal(Journal&& other) noexcept
    : m_path(std::move(other.m_path)), m_file(other.m_file), m_bytes(other.m_bytes),
      m_next_sequence(other.m_next_sequence)
{
    other.m_file = kNoFile;
}

Journal& Journal::operator=(Journal&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_path = std::move(other.m_path);
        m_file = other.m_file;
        m_bytes = other.m_bytes;
        m_next_sequence = other.m_next_sequence;
        other.m_file = kNoFile;
    }
    return *this;
}

bool Journal::open(const Path& path, u64 base_generation) noexcept
{
    return open(path, base_generation, replay_journal(path, base_generation));
}

bool Journal::open(const Path& path, u64 base_generation, const JournalReplay& existing) noexcept
{
    close();
    try
    {
        u64 keep = kJournalHeaderBytes;
        u64 next = 1U;
        if (existing.status == JournalStatus::Ok || existing.status == JournalStatus::TornTail)
        {
            keep = existing.valid_bytes;
            next = existing.records.size() + 1U;
        }
        else
        {
            u8 header[kJournalHeaderBytes];
            encode_journal_header(header, base_generation);
            if (!write_file_atomic(path, cont::ConstSpan<u8>{header, kJournalHeaderBytes}))
            {
                return false;
            }
        }
        const bool cut = existing.lost_bytes != 0U;
        const std::filesystem::path native = detail::native_path(path);
#if CRD_OS_WINDOWS
        const HANDLE h = CreateFileW(native.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(keep);
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(h) == 0 ||
            (cut && FlushFileBuffers(h) == 0))
        {
            (void)CloseHandle(h);
            return false;
        }
        m_file = static_cast<i64>(reinterpret_cast<intptr_t>(h));
#else
        const int fd = ::open(native.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0)
        {
            return false;
        }
        if (::ftruncate(fd, static_cast<off_t>(keep)) != 0 || ::lseek(fd, static_cast<off_t>(keep), SEEK_SET) < 0 ||
            (cut && !sync_fd(fd)))
        {
            (void)::close(fd);
            return false;
        }
        m_file = fd;
#endif
        m_path = path;
        m_bytes = keep;
        m_next_sequence = next;
        return true;
    }
    catch (...)
    {
        close();
        return false;
    }
}

bool Journal::append(cont::ConstSpan<u8> record) noexcept
{
    if (!is_open() || record.size() > 0xFFFFFFFFULL)
    {
        return false;
    }
    u8 header[kJournalRecordHeaderBytes];
    encode_record_header(header, m_next_sequence, record);
    const cont::ConstSpan<u8> head{header, kJournalRecordHeaderBytes};
#if CRD_OS_WINDOWS
    const HANDLE h = handle_of(m_file);
    const bool ok = write_all(h, head) && write_all(h, record);
    if (!ok)
    {
        // Cut the partial record, so the next append does not follow bytes replay stops at.
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(m_bytes);
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN) == 0 || SetEndOfFile(h) == 0)
        {
            close(); // the partial record stays: an append after it would follow bytes replay stops at
        }
        return false;
    }
#else
    const int fd = static_cast<int>(m_file);
    const bool ok = write_all(fd, head) && write_all(fd, record);
    if (!ok)
    {
        if (::ftruncate(fd, static_cast<off_t>(m_bytes)) != 0 || ::lseek(fd, static_cast<off_t>(m_bytes), SEEK_SET) < 0)
        {
            close(); // the partial record stays: an append after it would follow bytes replay stops at
        }
        return false;
    }
#endif
    m_bytes += kJournalRecordHeaderBytes + record.size();
    ++m_next_sequence;
    return true;
}

bool Journal::commit() noexcept
{
    if (!is_open())
    {
        return false;
    }
#if CRD_OS_WINDOWS
    return FlushFileBuffers(handle_of(m_file)) != 0;
#else
    return sync_fd(static_cast<int>(m_file));
#endif
}

bool Journal::reset(u64 base_generation) noexcept
{
    if (!is_open())
    {
        return false;
    }
    try
    {
        const Path path = m_path;
        close();
        u8 header[kJournalHeaderBytes];
        encode_journal_header(header, base_generation);
        return write_file_atomic(path, cont::ConstSpan<u8>{header, kJournalHeaderBytes}) && open(path, base_generation);
    }
    catch (...)
    {
        return false;
    }
}

void Journal::close() noexcept
{
    if (m_file != kNoFile)
    {
#if CRD_OS_WINDOWS
        (void)CloseHandle(handle_of(m_file));
#else
        (void)::close(static_cast<int>(m_file));
#endif
        m_file = kNoFile;
    }
    m_bytes = 0U;
    m_next_sequence = 1U;
}

// ---- autosave -------------------------------------------------------------------------------------------------

namespace
{
[[nodiscard]] u64 since(u64 now_ns, u64 then_ns) noexcept
{
    return now_ns > then_ns ? now_ns - then_ns : 0U;
}
} // namespace

Autosave::Autosave(const AutosavePolicy& policy, u64 now_ns) noexcept : m_policy(policy), m_last_snapshot_ns(now_ns) {}

void Autosave::changed(u64 now_ns) noexcept
{
    if (!m_uncommitted)
    {
        m_uncommitted = true;
        m_first_uncommitted_ns = now_ns;
    }
    m_unsnapshotted = true;
}

AutosaveStep Autosave::due(u64 now_ns, u64 journal_bytes) const noexcept
{
    if (m_unsnapshotted && (journal_bytes >= m_policy.snapshot_journal_bytes ||
                            since(now_ns, m_last_snapshot_ns) >= m_policy.snapshot_interval_ns))
    {
        return AutosaveStep::Snapshot;
    }
    if (m_uncommitted && since(now_ns, m_first_uncommitted_ns) >= m_policy.commit_interval_ns)
    {
        return AutosaveStep::Commit;
    }
    return AutosaveStep::None;
}

void Autosave::done(AutosaveStep step, u64 now_ns) noexcept
{
    if (step == AutosaveStep::None)
    {
        return;
    }
    m_uncommitted = false;
    if (step == AutosaveStep::Snapshot)
    {
        m_unsnapshotted = false;
        m_last_snapshot_ns = now_ns;
    }
}

} // namespace crd::platform::fs
