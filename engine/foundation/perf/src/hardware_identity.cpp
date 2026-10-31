// crd-perf -- the hardware identity of a diagnostic bundle (HW-RESILIENCE.2). See hardware_identity.hpp.

#include "payload_codec.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/core/platform.hpp>
#include <crd/perf/hardware_identity.hpp>

#include <cstdio>
#include <initializer_list>
#include <thread>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif CRD_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#elif CRD_OS_MAC
#include <sys/sysctl.h>
#endif

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;
using detail::Cursor;
using detail::put_u16;
using detail::put_u32;
using detail::put_u64;
using detail::put_u8;

[[nodiscard]] cont::StringView view(const cont::String& s) noexcept
{
    return cont::StringView{s.data(), s.size()};
}

// `text` without leading and trailing blanks, clipped to kHardwareFieldBytes (SMBIOS pads its strings with spaces).
void assign_trimmed(cont::String& out, const char* text, crd::usize n)
{
    const auto blank = [](char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };
    crd::usize begin = 0U;
    while (begin < n && blank(text[begin]))
    {
        ++begin;
    }
    while (n > begin && blank(text[n - 1U]))
    {
        --n;
    }
    const crd::usize count = n - begin < kHardwareFieldBytes ? n - begin : kHardwareFieldBytes;
    out.clear();
    out.append(cont::StringView{text + begin, count});
}

void assign_trimmed(cont::String& out, const char* text)
{
    crd::usize n = 0U;
    while (text[n] != '\0')
    {
        ++n;
    }
    assign_trimmed(out, text, n);
}

// One SMBIOS structure: its formatted area and its string set, both inside the table.
struct SmbiosStructure
{
    const crd::u8* formatted = nullptr;
    crd::usize length = 0U; // the formatted area's bytes, header included
    const crd::u8* strings = nullptr;
    crd::usize strings_bytes = 0U; // the string set without its final NUL pair

    [[nodiscard]] crd::u8 byte(crd::usize at) const noexcept { return at < length ? formatted[at] : 0U; }
    [[nodiscard]] crd::u32 word(crd::usize at) const noexcept
    {
        return at + 2U <= length
                   ? static_cast<crd::u32>(formatted[at]) | (static_cast<crd::u32>(formatted[at + 1U]) << 8U)
                   : 0U;
    }
    [[nodiscard]] crd::u32 dword(crd::usize at) const noexcept
    {
        return at + 4U <= length ? word(at) | (word(at + 2U) << 16U) : 0U;
    }

    // String `at`'s number (1-based; 0 names none) into `out`; false when it is absent.
    bool string(crd::usize at, cont::String& out) const
    {
        const crd::u8 number = byte(at);
        if (number == 0U)
        {
            return false;
        }
        crd::u8 index = 1U;
        crd::usize start = 0U;
        for (crd::usize i = 0U; i <= strings_bytes; ++i)
        {
            if (i == strings_bytes || strings[i] == 0U)
            {
                if (index == number)
                {
                    assign_trimmed(out, reinterpret_cast<const char*>(strings + start), i - start);
                    return true;
                }
                ++index;
                start = i + 1U;
            }
        }
        return false;
    }
};

// A module's speed: the 16-bit field, or the 32-bit extended one it defers to with 0xFFFF.
crd::u32 module_speed(const SmbiosStructure& s, crd::usize field, crd::usize extended)
{
    const crd::u32 speed = s.word(field);
    return speed == 0xFFFFU ? s.dword(extended) : speed;
}

void add_module(const SmbiosStructure& s, HardwareIdentity& out)
{
    const crd::u32 size = s.word(0x0CU);
    if (s.length < 0x15U || size == 0U)
    {
        return; // too short to describe a module, or an empty slot
    }
    if (out.modules.size() == kMaxMemoryModules)
    {
        out.flags &= ~kHardwareModulesKnown;
        return;
    }
    MemoryModule m(out.modules.allocator());
    if (size == 0x7FFFU)
    {
        m.size_mib = s.dword(0x1CU) & 0x7FFFFFFFU; // extended size, MiB
    }
    else if (size != 0xFFFFU)
    {
        m.size_mib = (size & 0x8000U) != 0U ? ((size & 0x7FFFU) + 1023U) / 1024U : size; // bit 15: KiB, rounded up
    }
    m.form_factor = s.byte(0x0EU);
    m.type = s.byte(0x12U);
    m.rated_mts = module_speed(s, 0x15U, 0x54U);
    m.configured_mts = module_speed(s, 0x20U, 0x58U);
    (void)s.string(0x10U, m.locator);
    (void)s.string(0x17U, m.manufacturer);
    (void)s.string(0x1AU, m.part_number);
    out.modules.push_back(static_cast<MemoryModule&&>(m));
}

#if CRD_OS_LINUX
// A small file's bytes into `out` (NUL-terminated); 0 when unreadable.
crd::usize read_file(const char* path, char* out, crd::usize cap)
{
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return 0U;
    }
    crd::usize got = 0U;
    while (got + 1U < cap)
    {
        const ssize_t n = ::read(fd, out + got, cap - 1U - got);
        if (n <= 0)
        {
            break;
        }
        got += static_cast<crd::usize>(n);
    }
    (void)::close(fd);
    out[got] = '\0';
    return got;
}

// The firmware strings an unprivileged process can read when the SMBIOS table is root-only.
void read_dmi_strings(HardwareIdentity& h)
{
    struct Attribute
    {
        const char* name;
        cont::String* field;
    };
    const Attribute attributes[] = {// NOLINT(modernize-avoid-c-arrays)
                                    {"bios_vendor", &h.bios_vendor}, {"bios_version", &h.bios_version},
                                    {"bios_date", &h.bios_date},     {"board_vendor", &h.board_vendor},
                                    {"board_name", &h.board_name},   {"sys_vendor", &h.system_vendor},
                                    {"product_name", &h.system_name}};
    char path[64] = {};  // NOLINT(modernize-avoid-c-arrays)
    char text[128] = {}; // NOLINT(modernize-avoid-c-arrays)
    for (const Attribute& a : attributes)
    {
        (void)std::snprintf(path, sizeof(path), "/sys/class/dmi/id/%s", a.name);
        const crd::usize n = read_file(path, text, sizeof(text));
        if (n > 0U)
        {
            assign_trimmed(*a.field, text, n);
        }
    }
    if (!h.bios_vendor.empty() || !h.bios_version.empty())
    {
        h.flags |= kHardwareFirmwareKnown;
    }
}
#endif

void read_firmware(HardwareIdentity& h)
{
#if CRD_OS_WINDOWS
    constexpr DWORD rsmb = 0x52534D42U; // 'RSMB'
    const UINT need = ::GetSystemFirmwareTable(rsmb, 0U, nullptr, 0U);
    if (need <= 8U)
    {
        return;
    }
    cont::Array<crd::u8> raw(h.modules.allocator());
    raw.resize(need);
    if (::GetSystemFirmwareTable(rsmb, 0U, raw.data(), need) != need)
    {
        return;
    }
    // RawSMBIOSData: calling method, major and minor version, DMI revision, then the table's u32 length.
    const crd::usize length = static_cast<crd::usize>(raw[4]) | (static_cast<crd::usize>(raw[5]) << 8U) |
                              (static_cast<crd::usize>(raw[6]) << 16U) | (static_cast<crd::usize>(raw[7]) << 24U);
    parse_smbios(cont::ConstSpan<crd::u8>{raw.data() + 8, length < need - 8U ? length : need - 8U}, h);
#elif CRD_OS_LINUX
    cont::Array<crd::u8> raw(h.modules.allocator());
    raw.resize(1U << 16U);
    const int fd = ::open("/sys/firmware/dmi/tables/DMI", O_RDONLY | O_CLOEXEC);
    crd::usize got = 0U;
    if (fd >= 0)
    {
        for (;;)
        {
            if (got == raw.size())
            {
                raw.resize(raw.size() * 2U);
            }
            const ssize_t n = ::read(fd, raw.data() + got, raw.size() - got);
            if (n <= 0)
            {
                break;
            }
            got += static_cast<crd::usize>(n);
        }
        (void)::close(fd);
    }
    if (got > 0U)
    {
        parse_smbios(cont::ConstSpan<crd::u8>{raw.data(), got}, h);
    }
    if ((h.flags & kHardwareFirmwareKnown) == 0U)
    {
        read_dmi_strings(h);
    }
#else
    (void)h; // macOS reads IOKit's platform entry under MAC.DIAG
#endif
}

void read_memory_total(HardwareIdentity& h)
{
#if CRD_OS_WINDOWS
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status) != 0)
    {
        h.memory_bytes = status.ullTotalPhys;
    }
#elif CRD_OS_LINUX
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0)
    {
        h.memory_bytes = static_cast<crd::u64>(pages) * static_cast<crd::u64>(page);
    }
#elif CRD_OS_MAC
    crd::u64 bytes = 0U;
    std::size_t size = sizeof(bytes);
    if (::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0U) == 0)
    {
        h.memory_bytes = bytes;
    }
#endif
    if (h.memory_bytes != 0U)
    {
        h.flags |= kHardwareMemoryKnown;
    }
}

void put_text(cont::Array<crd::u8>& out, const cont::String& s)
{
    detail::put_field(out, view(s), kHardwareFieldBytes);
}

HardwareIdentityRead refuse(HardwareIdentityRead& r, const char* why) noexcept
{
    r.ok = false;
    r.error = cont::StringView{why};
    return static_cast<HardwareIdentityRead&&>(r);
}

} // namespace

void parse_smbios(cont::ConstSpan<crd::u8> table, HardwareIdentity& out)
{
    out.flags |= kHardwareModulesKnown; // cleared below unless the walk completes
    bool complete = false;
    bool bios = false;
    bool system = false;
    bool board = false;
    crd::usize at = 0U;
    while (table.size() - at >= 4U)
    {
        SmbiosStructure s;
        s.formatted = table.data() + at;
        s.length = table[at + 1U];
        const crd::u8 type = table[at];
        if (s.length < 4U || table.size() - at < s.length + 2U)
        {
            break; // malformed: the formatted area or the string set's terminator is past the table
        }
        // The string set runs to the first NUL pair after the formatted area.
        crd::usize end = at + s.length;
        while (end + 1U < table.size() && (table[end] != 0U || table[end + 1U] != 0U))
        {
            ++end;
        }
        if (end + 1U >= table.size())
        {
            break;
        }
        s.strings = table.data() + at + s.length;
        s.strings_bytes = end - (at + s.length);
        if (type == 127U)
        {
            complete = true;
            break;
        }
        if (type == 0U && !bios)
        {
            bios = true;
            (void)s.string(0x04U, out.bios_vendor);
            (void)s.string(0x05U, out.bios_version);
            (void)s.string(0x08U, out.bios_date);
            if (!out.bios_vendor.empty() || !out.bios_version.empty())
            {
                out.flags |= kHardwareFirmwareKnown;
            }
        }
        else if (type == 1U && !system)
        {
            system = true;
            (void)s.string(0x04U, out.system_vendor);
            (void)s.string(0x05U, out.system_name);
        }
        else if (type == 2U && !board)
        {
            board = true;
            (void)s.string(0x04U, out.board_vendor);
            (void)s.string(0x05U, out.board_name);
        }
        else if (type == 17U)
        {
            add_module(s, out);
        }
        at = end + 2U;
    }
    if (!complete)
    {
        out.flags &= ~kHardwareModulesKnown; // no end-of-table structure: the module list may be partial
    }
}

HardwareIdentity read_hardware_identity(crd::memory::IAllocator* alloc)
{
    HardwareIdentity h(alloc);
    const crd::CpuIdentity cpu = crd::read_cpu_identity();
    assign_trimmed(h.cpu_vendor, cpu.vendor);
    assign_trimmed(h.cpu_model, cpu.model);
    h.cpu_signature = cpu.signature;
    if (cpu.signature != 0U)
    {
        h.flags |= kHardwareCpuKnown;
    }
    if (cpu.microcode_known)
    {
        h.microcode = cpu.microcode;
        h.flags |= kHardwareMicrocodeKnown;
    }
    h.logical_cpus = std::thread::hardware_concurrency();
    read_memory_total(h);
    read_firmware(h);
    return h;
}

const HardwareIdentity& host_hardware_identity()
{
    static const HardwareIdentity kHost = read_hardware_identity();
    return kHost;
}

bool same_hardware_identity(const HardwareIdentity& a, const HardwareIdentity& b, cont::String& differing)
{
    const crd::usize before = differing.size();
    const auto note = [&differing](bool same, const char* name)
    {
        if (same)
        {
            return;
        }
        if (!differing.empty())
        {
            differing.push_back(',');
        }
        differing.append(name);
    };
    const auto same_modules = [&a, &b]()
    {
        if (a.modules.size() != b.modules.size())
        {
            return false;
        }
        for (crd::usize i = 0U; i < a.modules.size(); ++i)
        {
            const MemoryModule& x = a.modules[i];
            const MemoryModule& y = b.modules[i];
            if (x.size_mib != y.size_mib || x.rated_mts != y.rated_mts || x.configured_mts != y.configured_mts ||
                x.type != y.type || x.form_factor != y.form_factor || view(x.locator) != view(y.locator) ||
                view(x.manufacturer) != view(y.manufacturer) || view(x.part_number) != view(y.part_number))
            {
                return false;
            }
        }
        return true;
    };
    const auto same_gpus = [&a, &b]()
    {
        if (a.gpus.size() != b.gpus.size())
        {
            return false;
        }
        for (crd::usize i = 0U; i < a.gpus.size(); ++i)
        {
            const GpuIdentity& x = a.gpus[i];
            const GpuIdentity& y = b.gpus[i];
            if (x.vendor_id != y.vendor_id || x.device_id != y.device_id || view(x.name) != view(y.name) ||
                view(x.driver) != view(y.driver))
            {
                return false;
            }
        }
        return true;
    };
    note(a.flags == b.flags, "flags");
    note(a.cpu_signature == b.cpu_signature, "cpu_signature");
    note(a.microcode == b.microcode, "microcode");
    note(a.logical_cpus == b.logical_cpus, "logical_cpus");
    note(a.memory_bytes == b.memory_bytes, "memory_bytes");
    note(view(a.cpu_vendor) == view(b.cpu_vendor), "cpu_vendor");
    note(view(a.cpu_model) == view(b.cpu_model), "cpu_model");
    note(view(a.bios_vendor) == view(b.bios_vendor), "bios_vendor");
    note(view(a.bios_version) == view(b.bios_version), "bios_version");
    note(view(a.bios_date) == view(b.bios_date), "bios_date");
    note(view(a.board_vendor) == view(b.board_vendor), "board_vendor");
    note(view(a.board_name) == view(b.board_name), "board_name");
    note(view(a.system_vendor) == view(b.system_vendor), "system_vendor");
    note(view(a.system_name) == view(b.system_name), "system_name");
    note(same_modules(), "modules");
    note(same_gpus(), "gpus");
    return differing.size() == before;
}

cont::Array<crd::u8> encode_hardware_identity(const HardwareIdentity& h, crd::memory::IAllocator* alloc)
{
    const crd::usize modules = h.modules.size() < kMaxMemoryModules ? h.modules.size() : kMaxMemoryModules;
    const crd::usize gpus = h.gpus.size() < kMaxGpus ? h.gpus.size() : kMaxGpus;
    crd::u32 flags = h.flags & kHardwareKnownFlags;
    if (modules != h.modules.size())
    {
        flags &= ~kHardwareModulesKnown;
    }
    cont::Array<crd::u8> out(alloc);
    put_u32(out, kHardwareIdentityMagic);
    put_u32(out, kHardwareIdentityVersion);
    put_u32(out, flags);
    put_u32(out, h.cpu_signature);
    put_u32(out, h.microcode);
    put_u32(out, h.logical_cpus);
    put_u64(out, h.memory_bytes);
    for (const cont::String* field : {&h.cpu_vendor, &h.cpu_model, &h.bios_vendor, &h.bios_version, &h.bios_date,
                                      &h.board_vendor, &h.board_name, &h.system_vendor, &h.system_name})
    {
        put_text(out, *field);
    }
    put_u16(out, static_cast<crd::u16>(modules));
    for (crd::usize i = 0U; i < modules; ++i)
    {
        const MemoryModule& m = h.modules[i];
        put_u32(out, m.size_mib);
        put_u32(out, m.rated_mts);
        put_u32(out, m.configured_mts);
        put_u8(out, m.type);
        put_u8(out, m.form_factor);
        put_text(out, m.locator);
        put_text(out, m.manufacturer);
        put_text(out, m.part_number);
    }
    put_u16(out, static_cast<crd::u16>(gpus));
    for (crd::usize i = 0U; i < gpus; ++i)
    {
        const GpuIdentity& g = h.gpus[i];
        put_u32(out, g.vendor_id);
        put_u32(out, g.device_id);
        put_text(out, g.name);
        put_text(out, g.driver);
    }
    return out;
}

HardwareIdentityRead decode_hardware_identity(cont::ConstSpan<crd::u8> payload, crd::memory::IAllocator* alloc)
{
    HardwareIdentityRead r;
    r.identity = HardwareIdentity(alloc);
    HardwareIdentity& h = r.identity;
    Cursor in{payload};
    if (in.u32() != kHardwareIdentityMagic || !in.ok)
    {
        return refuse(r, "not a hardware identity payload (magic)");
    }
    if (in.u32() != kHardwareIdentityVersion || !in.ok)
    {
        return refuse(r, "unsupported hardware identity version");
    }
    h.flags = in.u32();
    h.cpu_signature = in.u32();
    h.microcode = in.u32();
    h.logical_cpus = in.u32();
    h.memory_bytes = in.u64();
    if (!in.ok)
    {
        return refuse(r, "hardware identity header is short");
    }
    if ((h.flags & ~kHardwareKnownFlags) != 0U)
    {
        return refuse(r, "unknown hardware identity flags");
    }
    for (cont::String* field : {&h.cpu_vendor, &h.cpu_model, &h.bios_vendor, &h.bios_version, &h.bios_date,
                                &h.board_vendor, &h.board_name, &h.system_vendor, &h.system_name})
    {
        if (!in.field(*field, kHardwareFieldBytes))
        {
            return refuse(r, "a hardware identity field is over its bound or past the payload");
        }
    }
    const crd::u16 modules = in.u16();
    if (!in.ok || modules > kMaxMemoryModules)
    {
        return refuse(r, "the memory module count is over its bound or past the payload");
    }
    for (crd::u16 i = 0U; i < modules; ++i)
    {
        MemoryModule m(alloc);
        m.size_mib = in.u32();
        m.rated_mts = in.u32();
        m.configured_mts = in.u32();
        m.type = in.u8();
        m.form_factor = in.u8();
        if (!in.ok || !in.field(m.locator, kHardwareFieldBytes) || !in.field(m.manufacturer, kHardwareFieldBytes) ||
            !in.field(m.part_number, kHardwareFieldBytes))
        {
            return refuse(r, "a memory module is over its bounds or past the payload");
        }
        h.modules.push_back(static_cast<MemoryModule&&>(m));
    }
    const crd::u16 gpus = in.u16();
    if (!in.ok || gpus > kMaxGpus)
    {
        return refuse(r, "the GPU count is over its bound or past the payload");
    }
    for (crd::u16 i = 0U; i < gpus; ++i)
    {
        GpuIdentity g(alloc);
        g.vendor_id = in.u32();
        g.device_id = in.u32();
        if (!in.ok || !in.field(g.name, kHardwareFieldBytes) || !in.field(g.driver, kHardwareFieldBytes))
        {
            return refuse(r, "a GPU entry is over its bounds or past the payload");
        }
        h.gpus.push_back(static_cast<GpuIdentity&&>(g));
    }
    if (!in.done())
    {
        return refuse(r, "trailing bytes after the hardware identity");
    }
    r.ok = true;
    return r;
}

} // namespace crd::perf
