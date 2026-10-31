// HW-RESILIENCE.2: the hardware identity of a bundle (crd/perf/hardware_identity.hpp) -- the SMBIOS walk over
// synthetic and hostile tables, the section codec and its refusals, the field comparison, and this machine's reading.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/core/platform.hpp>
#include <crd/perf/hardware_identity.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace cont = crd::containers;
namespace cp = crd::perf;
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;

namespace
{

cont::StringView view(const cont::String& s)
{
    return cont::StringView{s.data(), s.size()};
}

// Builds an SMBIOS structure table: each structure's formatted area (type, length and handle first), then its
// strings and the NUL pair.
struct TableBuilder
{
    cont::Array<u8> bytes;

    // `formatted` holds the bytes after the 4-byte header; `strings` is the string set (nullptr-terminated list).
    void add(u8 type, const cont::Array<u8>& formatted, std::initializer_list<const char*> strings)
    {
        bytes.push_back(type);
        bytes.push_back(static_cast<u8>(4U + formatted.size()));
        bytes.push_back(0x34U);
        bytes.push_back(0x12U);
        for (const u8 b : formatted)
        {
            bytes.push_back(b);
        }
        for (const char* s : strings)
        {
            for (usize i = 0U; s[i] != '\0'; ++i)
            {
                bytes.push_back(static_cast<u8>(s[i]));
            }
            bytes.push_back(0U);
        }
        if (strings.size() == 0U)
        {
            bytes.push_back(0U);
        }
        bytes.push_back(0U);
    }
};

// A formatted area of `n` bytes after the header, with `values` at their SMBIOS offsets (offset 4 is index 0).
cont::Array<u8> formatted(usize n, std::initializer_list<std::pair<usize, u32>> bytes8,
                          std::initializer_list<std::pair<usize, u32>> words = {},
                          std::initializer_list<std::pair<usize, u32>> dwords = {})
{
    cont::Array<u8> out;
    out.resize(n);
    std::memset(out.data(), 0, n);
    for (const auto& [at, v] : bytes8)
    {
        out[at - 4U] = static_cast<u8>(v);
    }
    for (const auto& [at, v] : words)
    {
        out[at - 4U] = static_cast<u8>(v & 0xFFU);
        out[at - 4U + 1U] = static_cast<u8>((v >> 8U) & 0xFFU);
    }
    for (const auto& [at, v] : dwords)
    {
        for (u32 i = 0U; i < 4U; ++i)
        {
            out[at - 4U + i] = static_cast<u8>((v >> (8U * i)) & 0xFFU);
        }
    }
    return out;
}

// A desktop's table: BIOS, system, board, two populated DDR5 modules (one with the extended speeds, one with the
// extended size), an empty slot, and the end-of-table structure.
cont::Array<u8> desktop_table()
{
    TableBuilder t;
    t.add(0U, formatted(0x18U - 4U, {{0x04U, 1U}, {0x05U, 2U}, {0x08U, 3U}}),
          {"  American Megatrends Inc.  ", "2202", "05/07/2026"});
    t.add(1U, formatted(0x1BU - 4U, {{0x04U, 1U}, {0x05U, 2U}}), {"ASUS", "System Product Name"});
    t.add(2U, formatted(0x0FU - 4U, {{0x04U, 1U}, {0x05U, 2U}}),
          {"ASUSTeK COMPUTER INC.", "ROG STRIX Z790-E GAMING WIFI II"});
    // Module A: 16 GiB (the 16-bit field holds up to 32 GiB - 1 MiB; bit 15 would mean KiB), 0xFFFF speeds
    // deferring to the extended 8000/6000 MT/s fields (SMBIOS 3.3+, length 0x5C).
    t.add(17U,
          formatted(0x5CU - 4U, {{0x0EU, 0x09U}, {0x10U, 1U}, {0x12U, 0x22U}, {0x17U, 2U}, {0x1AU, 3U}},
                    {{0x0CU, 16384U}, {0x15U, 0xFFFFU}, {0x20U, 0xFFFFU}}, {{0x54U, 8000U}, {0x58U, 6000U}}),
          {"DIMM_A1", "G Skill Intl", "F5-6000J3038F16G    "});
    // An empty slot: size 0.
    t.add(17U, formatted(0x28U - 4U, {{0x10U, 1U}}), {"DIMM_A2"});
    // Module B: 0x7FFF defers to the extended size (64 GiB), plain speeds, no maker string.
    t.add(17U,
          formatted(0x28U - 4U, {{0x0EU, 0x0DU}, {0x10U, 1U}, {0x12U, 0x1AU}, {0x1AU, 2U}},
                    {{0x0CU, 0x7FFFU}, {0x15U, 3200U}, {0x20U, 2933U}}, {{0x1CU, 65536U}}),
          {"ChannelB-DIMM0", "M471A4G43AB1-CWE"});
    t.add(127U, cont::Array<u8>{}, {});
    return static_cast<cont::Array<u8>&&>(t.bytes);
}

cp::HardwareIdentity sample_identity()
{
    cp::HardwareIdentity h;
    h.flags = cp::kHardwareKnownFlags;
    h.cpu_signature = 0xB0671U;
    h.microcode = 0x133U;
    h.logical_cpus = 32U;
    h.memory_bytes = 64ULL << 30U;
    h.cpu_vendor.append("GenuineIntel");
    h.cpu_model.append("Intel(R) Core(TM) i9-14900K");
    h.bios_vendor.append("American Megatrends Inc.");
    h.bios_version.append("2202");
    h.bios_date.append("05/07/2026");
    h.board_vendor.append("ASUSTeK COMPUTER INC.");
    h.board_name.append("ROG STRIX Z790-E GAMING WIFI II");
    h.system_vendor.append("ASUS");
    h.system_name.append("System Product Name");
    cp::MemoryModule m;
    m.size_mib = 32768U;
    m.rated_mts = 8000U;
    m.configured_mts = 6000U;
    m.type = 0x22U;
    m.form_factor = 0x09U;
    m.locator.append("DIMM_A1");
    m.manufacturer.append("G Skill Intl");
    m.part_number.append("F5-6000J3038F16G");
    h.modules.push_back(static_cast<cp::MemoryModule&&>(m));
    cp::GpuIdentity g;
    g.vendor_id = 0x10DEU;
    g.device_id = 0x2705U;
    g.name.append("NVIDIA GeForce RTX 4070 Ti SUPER");
    g.driver.append("32.0.15.8180");
    h.gpus.push_back(static_cast<cp::GpuIdentity&&>(g));
    return h;
}

} // namespace

TEST_CASE("hardware identity: the SMBIOS walk reads the firmware, the board and every populated module",
          "[perf][diag][hw-resilience]")
{
    const cont::Array<u8> table = desktop_table();
    cp::HardwareIdentity h;
    cp::parse_smbios({table.data(), table.size()}, h);
    CHECK(h.flags == (cp::kHardwareFirmwareKnown | cp::kHardwareModulesKnown));
    CHECK(view(h.bios_vendor) == "American Megatrends Inc."); // padding trimmed
    CHECK(view(h.bios_version) == "2202");
    CHECK(view(h.bios_date) == "05/07/2026");
    CHECK(view(h.system_vendor) == "ASUS");
    CHECK(view(h.system_name) == "System Product Name");
    CHECK(view(h.board_vendor) == "ASUSTeK COMPUTER INC.");
    CHECK(view(h.board_name) == "ROG STRIX Z790-E GAMING WIFI II");
    REQUIRE(h.modules.size() == 2U); // the empty slot is left out
    const cp::MemoryModule& a = h.modules[0];
    CHECK(a.size_mib == 16384U);
    CHECK(a.rated_mts == 8000U);
    CHECK(a.configured_mts == 6000U);
    CHECK(a.type == 0x22U);
    CHECK(a.form_factor == 0x09U);
    CHECK(view(a.locator) == "DIMM_A1");
    CHECK(view(a.manufacturer) == "G Skill Intl");
    CHECK(view(a.part_number) == "F5-6000J3038F16G");
    const cp::MemoryModule& b = h.modules[1];
    CHECK(b.size_mib == 65536U);
    CHECK(b.rated_mts == 3200U);
    CHECK(b.configured_mts == 2933U);
    CHECK(b.type == 0x1AU);
    CHECK(view(b.locator) == "ChannelB-DIMM0");
    CHECK(b.manufacturer.empty());
    CHECK(view(b.part_number) == "M471A4G43AB1-CWE");
}

TEST_CASE("hardware identity: every cut of the table reads a prefix and never calls the module list complete",
          "[perf][diag][hw-resilience]")
{
    const cont::Array<u8> table = desktop_table();
    for (usize n = 0U; n < table.size(); ++n)
    {
        INFO("cut at " << n << " of " << table.size());
        // A copy of exactly n bytes, so a read past the cut is a read past the allocation (ASan lanes).
        cont::Array<u8> cut;
        cut.resize(n);
        if (n > 0U)
        {
            std::memcpy(cut.data(), table.data(), n);
        }
        cp::HardwareIdentity h;
        cp::parse_smbios({cut.data(), cut.size()}, h);
        CHECK((h.flags & cp::kHardwareModulesKnown) == 0U);
        CHECK(h.modules.size() <= 2U);
    }
}

TEST_CASE("hardware identity: hostile structures end the walk or read nothing they do not hold",
          "[perf][diag][hw-resilience]")
{
    // A structure length under the header's 4 bytes ends the walk before it.
    {
        TableBuilder t;
        t.add(0U, formatted(0x18U - 4U, {{0x04U, 1U}}), {"vendor"});
        t.bytes.push_back(17U);
        t.bytes.push_back(2U); // length 2: malformed
        t.bytes.push_back(0U);
        t.bytes.push_back(0U);
        t.add(127U, cont::Array<u8>{}, {});
        cp::HardwareIdentity h;
        cp::parse_smbios({t.bytes.data(), t.bytes.size()}, h);
        CHECK(view(h.bios_vendor) == "vendor");
        CHECK((h.flags & cp::kHardwareModulesKnown) == 0U);
    }
    // String numbers past the set, and a module too short to hold its speed fields, read nothing.
    {
        TableBuilder t;
        t.add(0U, formatted(0x18U - 4U, {{0x04U, 9U}, {0x05U, 1U}}), {"only one"});
        t.add(17U, formatted(0x10U - 4U, {}, {{0x0CU, 4096U}}), {});
        // Bit 15 of the size: KiB granularity (1536 KiB rounds up to 2 MiB, never to an unknown 0).
        t.add(17U, formatted(0x28U - 4U, {}, {{0x0CU, 0x8000U | 1536U}}), {});
        t.add(127U, cont::Array<u8>{}, {});
        cp::HardwareIdentity h;
        cp::parse_smbios({t.bytes.data(), t.bytes.size()}, h);
        CHECK(h.bios_vendor.empty());
        CHECK(view(h.bios_version) == "only one");
        REQUIRE(h.modules.size() == 1U);
        CHECK(h.modules[0].size_mib == 2U);
        CHECK((h.flags & cp::kHardwareModulesKnown) != 0U);
    }
    // More modules than the bound: the list stops at it and is not complete.
    {
        TableBuilder t;
        for (u32 i = 0U; i < cp::kMaxMemoryModules + 1U; ++i)
        {
            t.add(17U, formatted(0x28U - 4U, {{0x10U, 1U}}, {{0x0CU, 8192U}, {0x15U, 4800U}}), {"slot"});
        }
        t.add(127U, cont::Array<u8>{}, {});
        cp::HardwareIdentity h;
        cp::parse_smbios({t.bytes.data(), t.bytes.size()}, h);
        CHECK(h.modules.size() == cp::kMaxMemoryModules);
        CHECK((h.flags & cp::kHardwareModulesKnown) == 0U);
    }
}

TEST_CASE("hardware identity: the section round-trips and its decoder refuses every damaged payload",
          "[perf][diag][hw-resilience]")
{
    const cp::HardwareIdentity h = sample_identity();
    const cont::Array<u8> payload = cp::encode_hardware_identity(h);
    const cp::HardwareIdentityRead read = cp::decode_hardware_identity({payload.data(), payload.size()});
    REQUIRE(read.ok);
    cont::String differing;
    CHECK(cp::same_hardware_identity(read.identity, h, differing));
    CHECK(differing.empty());

    // Every shorter payload is refused, and so is one trailing byte.
    for (usize n = 0U; n < payload.size(); ++n)
    {
        INFO("cut at " << n);
        cont::Array<u8> cut;
        cut.resize(n);
        if (n > 0U)
        {
            std::memcpy(cut.data(), payload.data(), n);
        }
        CHECK_FALSE(cp::decode_hardware_identity({cut.data(), cut.size()}).ok);
    }
    cont::Array<u8> longer = payload;
    longer.push_back(0U);
    CHECK_FALSE(cp::decode_hardware_identity({longer.data(), longer.size()}).ok);

    const auto decodes_with = [&payload](usize at, u8 value)
    {
        cont::Array<u8> bad = payload;
        bad[at] = value;
        return cp::decode_hardware_identity({bad.data(), bad.size()}).ok;
    };
    CHECK_FALSE(decodes_with(0U, 0x00U)); // magic
    CHECK_FALSE(decodes_with(4U, 2U));    // version
    CHECK_FALSE(decodes_with(8U, 0x20U)); // an unknown flag
    // The first text field's length (after the 32-byte header) over its bound.
    CHECK_FALSE(decodes_with(32U, static_cast<u8>(cp::kHardwareFieldBytes + 1U)));
    CHECK(decodes_with(32U, payload[32])); // the same byte decodes: the offset is the field's
}

TEST_CASE("hardware identity: the encoder clips fields and counts to their bounds and says the list is partial",
          "[perf][diag][hw-resilience]")
{
    cp::HardwareIdentity h = sample_identity();
    for (u32 i = 0U; i < 100U; ++i)
    {
        h.cpu_model.push_back('x');
    }
    while (h.modules.size() <= cp::kMaxMemoryModules)
    {
        h.modules.push_back(cp::MemoryModule{});
    }
    const cont::Array<u8> payload = cp::encode_hardware_identity(h);
    const cp::HardwareIdentityRead read = cp::decode_hardware_identity({payload.data(), payload.size()});
    REQUIRE(read.ok);
    CHECK(read.identity.cpu_model.size() == cp::kHardwareFieldBytes);
    CHECK(read.identity.modules.size() == cp::kMaxMemoryModules);
    CHECK((read.identity.flags & cp::kHardwareModulesKnown) == 0U);
    CHECK((read.identity.flags & cp::kHardwareMicrocodeKnown) != 0U);
}

TEST_CASE("hardware identity: the comparison names every field that differs", "[perf][diag][hw-resilience]")
{
    const cp::HardwareIdentity a = sample_identity();
    cp::HardwareIdentity b = sample_identity();
    b.microcode = 0x129U;
    b.bios_version.clear();
    b.bios_version.append("1801");
    b.modules[0].configured_mts = 5600U;
    b.gpus[0].driver.append(".1");
    cont::String differing;
    CHECK_FALSE(cp::same_hardware_identity(a, b, differing));
    CHECK(view(differing) == "microcode,bios_version,modules,gpus");
}

TEST_CASE("hardware identity: this machine reads its CPU, memory and, where the OS allows, firmware and modules",
          "[perf][diag][hw-resilience]")
{
    const cp::HardwareIdentity& h = cp::host_hardware_identity();
    CHECK(&h == &cp::host_hardware_identity()); // read once
    CHECK((h.flags & cp::kHardwareMemoryKnown) != 0U);
    CHECK(h.memory_bytes >= (256ULL << 20U));
    CHECK(h.logical_cpus >= 1U);
#if CRD_ARCH_X64
    CHECK((h.flags & cp::kHardwareCpuKnown) != 0U);
    CHECK_FALSE(h.cpu_vendor.empty());
    CHECK_FALSE(h.cpu_model.empty());
    CHECK(crd::x64_signature_parts(h.cpu_signature).family != 0U);
#endif
#if CRD_OS_WINDOWS
    // Windows gives every process the firmware table and the microcode revision.
    CHECK((h.flags & cp::kHardwareFirmwareKnown) != 0U);
    CHECK((h.flags & cp::kHardwareModulesKnown) != 0U);
#if CRD_ARCH_X64
    CHECK((h.flags & cp::kHardwareMicrocodeKnown) != 0U);
#endif
#endif
    // Evidence on every run (CTest's log and JUnit keep it): what each lane's machine reported.
    const crd::CpuSignatureParts parts = crd::x64_signature_parts(h.cpu_signature);
    char line[512];
    (void)std::snprintf(line, sizeof(line),
                        "hardware: %s | %s | signature 0x%x (family %u model %u stepping %u) | microcode %s0x%x | "
                        "%u cpus, %llu MiB, %u modules%s | bios %s %s %s | board %s %s",
                        h.cpu_vendor.c_str(), h.cpu_model.c_str(), h.cpu_signature, parts.family, parts.model,
                        parts.stepping, (h.flags & cp::kHardwareMicrocodeKnown) != 0U ? "" : "unknown ", h.microcode,
                        h.logical_cpus, static_cast<unsigned long long>(h.memory_bytes >> 20U),
                        static_cast<unsigned>(h.modules.size()),
                        (h.flags & cp::kHardwareModulesKnown) != 0U ? "" : " (list unknown)", h.bios_vendor.c_str(),
                        h.bios_version.c_str(), h.bios_date.c_str(), h.board_vendor.c_str(), h.board_name.c_str());
    WARN(line);
}
