#pragma once

// crd-perf -- the hardware identity of a diagnostic bundle (HW-RESILIENCE.2; the HardwareIdentity bundle section,
// tag 8).
//
// A failure only some machines show is often the machine: an unstable CPU or its microcode, old firmware, memory run
// past its rating. The identity records what fault classification and a support reply need: the CPU's vendor, model,
// signature and microcode (crd::read_cpu_identity), the firmware and board (SMBIOS types 0, 1 and 2), the memory (its
// total, and each populated module from SMBIOS type 17: size, rated and configured speed, type, maker and part) and the
// GPUs with their drivers (filled by the host that owns the devices). What the OS does not let the process read stays
// unknown (its kHardware*Known flag clear), never guessed: Linux's SMBIOS table is root-only, so an unprivileged Linux
// process reports the firmware strings from /sys/class/dmi/id and no modules.
//
// Reading it touches the registry, sysfs and the firmware table, so it never runs on a fatal path:
// host_hardware_identity() reads it once, FlightRecorder::init reads it before any capture can be delivered, and every
// bundle copies that; attach_hardware_identity_to_crashes reads it when the crash handler is installed, and native
// dumps and crash records carry that copy.
//
// On-disk payload (little-endian, versioned; the bundle section CRC covers it):
//   u32 magic 'CHW1', u32 version, u32 flags (kHardware*Known, a closed set), u32 cpu signature, u32 microcode,
//   u32 logical cpus, u64 memory bytes; the text fields cpu vendor, cpu model, bios vendor, bios version, bios date,
//   board vendor, board name, system vendor, system name (each a u16 length and at most kHardwareFieldBytes bytes);
//   u16 module count (at most kMaxMemoryModules), each u32 size MiB, u32 rated MT/s, u32 configured MT/s, u8 SMBIOS
//   memory type, u8 SMBIOS form factor, then locator, manufacturer and part number; u16 gpu count (at most kMaxGpus),
//   each u32 vendor id, u32 device id, then name and driver. The decoder refuses a wrong magic or version, unknown
//   flags, a field or count over its bound or past the payload, and trailing bytes.
//
// Contract: docs/design/runtime-diagnostics.md (HW-RESILIENCE.2); ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

inline constexpr crd::u32 kHardwareIdentityMagic = 0x31574843U; // bytes 'C','H','W','1'
inline constexpr crd::u32 kHardwareIdentityVersion = 1U;
inline constexpr crd::u32 kHardwareFieldBytes = 64U; // each text field's bound
inline constexpr crd::u32 kMaxMemoryModules = 64U;
inline constexpr crd::u32 kMaxGpus = 16U;

// HardwareIdentity::flags bits: which parts the OS let this process read.
inline constexpr crd::u32 kHardwareCpuKnown = 1U << 0;       // the CPU's signature (CPUID, MIDR)
inline constexpr crd::u32 kHardwareMicrocodeKnown = 1U << 1; // the loaded microcode revision
inline constexpr crd::u32 kHardwareFirmwareKnown = 1U << 2;  // the BIOS and board strings
inline constexpr crd::u32 kHardwareMemoryKnown = 1U << 3;    // the total physical memory
inline constexpr crd::u32 kHardwareModulesKnown = 1U << 4;   // SMBIOS type 17: the module list is complete
inline constexpr crd::u32 kHardwareKnownFlags =
    kHardwareCpuKnown | kHardwareMicrocodeKnown | kHardwareFirmwareKnown | kHardwareMemoryKnown | kHardwareModulesKnown;

struct MemoryModule
{
    MemoryModule() = default;
    explicit MemoryModule(crd::memory::IAllocator* alloc) : locator(alloc), manufacturer(alloc), part_number(alloc) {}

    crd::u32 size_mib = 0U;          // 0 when the table says the size is unknown
    crd::u32 rated_mts = 0U;         // the module's maximum speed, MT/s (0: unknown)
    crd::u32 configured_mts = 0U;    // the speed the firmware set, MT/s (0: unknown)
    crd::u8 type = 0U;               // SMBIOS memory type (0x1A DDR4, 0x22 DDR5, ...)
    crd::u8 form_factor = 0U;        // SMBIOS form factor (0x09 DIMM, 0x0D SODIMM, ...)
    crd::containers::String locator; // the slot ("DIMM_A1")
    crd::containers::String manufacturer;
    crd::containers::String part_number;
};

struct GpuIdentity
{
    GpuIdentity() = default;
    explicit GpuIdentity(crd::memory::IAllocator* alloc) : name(alloc), driver(alloc) {}

    crd::u32 vendor_id = 0U; // PCI vendor (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, ...)
    crd::u32 device_id = 0U;
    crd::containers::String name;
    crd::containers::String driver; // the driver version as the API reports it
};

struct HardwareIdentity
{
    HardwareIdentity() = default;
    explicit HardwareIdentity(crd::memory::IAllocator* alloc)
        : cpu_vendor(alloc), cpu_model(alloc), bios_vendor(alloc), bios_version(alloc), bios_date(alloc),
          board_vendor(alloc), board_name(alloc), system_vendor(alloc), system_name(alloc), modules(alloc), gpus(alloc)
    {
    }

    crd::u32 flags = 0U;         // kHardware*Known
    crd::u32 cpu_signature = 0U; // x64 CPUID 1 EAX; ARM64 MIDR_EL1
    crd::u32 microcode = 0U;
    crd::u32 logical_cpus = 0U;
    crd::u64 memory_bytes = 0U;
    crd::containers::String cpu_vendor;
    crd::containers::String cpu_model;
    crd::containers::String bios_vendor;
    crd::containers::String bios_version;
    crd::containers::String bios_date;
    crd::containers::String board_vendor;
    crd::containers::String board_name;
    crd::containers::String system_vendor;
    crd::containers::String system_name;
    crd::containers::Array<MemoryModule> modules;
    crd::containers::Array<GpuIdentity> gpus;
};

// Reads this machine. Not for a fatal path (see the header comment).
[[nodiscard]] HardwareIdentity
read_hardware_identity(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// This machine, read once at the first call (thread-safe); FlightRecorder::init calls it, so a bundle never reads
// the OS.
[[nodiscard]] const HardwareIdentity& host_hardware_identity();

// Fills the firmware strings and the module list of `out` from an SMBIOS structure table (Windows' RSMB table after
// its 8-byte header; Linux's /sys/firmware/dmi/tables/DMI), setting kHardwareFirmwareKnown when a BIOS structure
// names a vendor or version and kHardwareModulesKnown when the walk reached the end-of-table structure (type 127).
// Bounds-checked: a malformed structure ends the walk with what it read before it; nothing is read outside
// `table`. Empty slots are skipped; more than kMaxMemoryModules modules clear kHardwareModulesKnown.
void parse_smbios(crd::containers::ConstSpan<crd::u8> table, HardwareIdentity& out);

// True when every field of `a` and `b` matches. Otherwise appends the differing field names to `differing`, comma
// separated, in declaration order ("flags", "cpu_signature", "microcode", "logical_cpus", "memory_bytes",
// "cpu_vendor", "cpu_model", "bios_vendor", "bios_version", "bios_date", "board_vendor", "board_name",
// "system_vendor", "system_name", "modules", "gpus").
[[nodiscard]] bool same_hardware_identity(const HardwareIdentity& a, const HardwareIdentity& b,
                                          crd::containers::String& differing);

// Encode `h` as a HardwareIdentity section payload. A text field longer than kHardwareFieldBytes is clipped to it;
// modules and GPUs past their bounds are left out and their Known flag cleared.
[[nodiscard]] crd::containers::Array<crd::u8>
encode_hardware_identity(const HardwareIdentity& h, crd::memory::IAllocator* alloc = crd::memory::default_allocator());

struct HardwareIdentityRead
{
    bool ok = false;
    crd::containers::StringView error; // why the payload was refused (static text)
    HardwareIdentity identity;
};

// Decode an untrusted HardwareIdentity payload; nothing is read outside `payload`.
[[nodiscard]] HardwareIdentityRead
decode_hardware_identity(crd::containers::ConstSpan<crd::u8> payload,
                         crd::memory::IAllocator* alloc = crd::memory::default_allocator());

// The most bytes encode_hardware_identity writes: the fixed header, every text field at its bound, kMaxMemoryModules
// modules and kMaxGpus GPUs. A native crash carries it whole (crd::crash::kMaxMachineStreamBytes holds it).
inline constexpr crd::u32 kHardwareIdentityMaxBytes =
    6U * 4U + 8U + 9U * (2U + kHardwareFieldBytes) + 2U +
    kMaxMemoryModules * (3U * 4U + 2U + 3U * (2U + kHardwareFieldBytes)) + 2U +
    kMaxGpus * (2U * 4U + 2U * (2U + kHardwareFieldBytes));

// Hands host_hardware_identity(), encoded, to crd::crash, so every later Windows dump (its kMachineStreamType user
// stream) and Linux crash record (its `machine` line) names the machine a native crash happened on. Whatever installs
// the crash handler calls it right after the install (DiagnosticHost::start, Application), so the OS is read there,
// never on the fatal path. False on a platform without a crash backend.
bool attach_hardware_identity_to_crashes();

// The hardware identity a native crash capture carries (a minidump's kMachineStreamType stream or a Linux crash
// record's `machine` line), decoded as decode_hardware_identity does. Untrusted input, read on any platform; refused
// (`error` says why) when the capture carries none or it does not decode.
[[nodiscard]] HardwareIdentityRead
read_crash_hardware_identity(crd::containers::ConstSpan<crd::u8> capture,
                             crd::memory::IAllocator* alloc = crd::memory::default_allocator());

} // namespace crd::perf
