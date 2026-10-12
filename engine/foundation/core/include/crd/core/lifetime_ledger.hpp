#pragma once

// Lifetime accounting beyond heap bytes: the resource-lifetime ledger of DIAG.10c.
//
// Heap bytes are only one thing a long run can leak. A file the engine keeps open, a library it loaded, an address
// range it reserved, a GPU object it created, a callback someone registered and a thread it started all outlive the
// call that made them, and each must end when its owner ends. The ledger counts them per class, so a window around a
// workload (a warm/cold cycle, a reload, a shutdown) can say what it left behind (docs/design/runtime-diagnostics.md,
// DIAG.10c).
//
// The engine records at the site that acquires and the site that releases: `lifetime_ledger().acquired(c)` once the
// OS (or the owner) has granted the thing, `released(c)` once it has taken it back. Sites that open and close a
// handle inside one call are not recorded: nothing they hold can outlive the call. The design doc lists the wired
// sites and the scoped ones.
//
// The ledger checks itself. A release it cannot match (nothing of that class is live, or more bytes than are live)
// changes nothing and is counted as unmatched: the accounting is wrong somewhere, and a report says so instead of
// wrapping a count or clamping it to zero.
//
// Recording is wait-free and allocation-free: relaxed atomics in a fixed table, no lock, no allocation, safe from
// any thread and during static destruction. A snapshot is a set of independent reads, so a window is meaningful
// when the workload it brackets has finished, not while other threads are acquiring.

#include <crd/core/types.hpp>

#include <atomic>

namespace crd
{

// What the engine holds besides heap bytes.
// NOLINTNEXTLINE(performance-enum-size)
enum class LifetimeClass : crd::u8
{
    FileHandle,           // an OS file handle or descriptor kept open past the call that opened it
    LibraryHandle,        // a reference to a loaded dynamic library (LoadLibrary / dlopen)
    MappedRegion,         // a reserved or mapped address range; its bytes are counted too
    DeviceObject,         // a GPU resource or program carrying a Cerid identity
    CallbackRegistration, // a callback, subscriber, sink or observer registered with an owner
    Thread,               // an OS thread the engine started and must join (a worker, a watchdog, a host's run)
    NativeGpuObject,      // a GPU API object without a Cerid identity: an instance, a device reference, a created
                          // queue, a fence or a swapchain
    OsObject,             // another OS object the engine created and must close or release: a window, an event, a
                          // COM interface reference
};
// The masks in LifetimeReport are u8: eight classes fill them, so a ninth widens them first.
inline constexpr crd::usize kLifetimeClassCount = 8U;
[[nodiscard]] const char* lifetime_class_name(LifetimeClass c) noexcept;
[[nodiscard]] constexpr crd::u8 lifetime_class_bit(LifetimeClass c) noexcept
{
    return static_cast<crd::u8>(1U << static_cast<crd::u32>(c));
}

// One class's counts at one moment.
struct LifetimeCounts
{
    crd::u64 acquired           = 0U; // every acquisition recorded
    crd::u64 released           = 0U; // every release that matched a live acquisition
    crd::u64 live               = 0U; // acquired and not yet released
    crd::u64 high_water         = 0U; // the most that were live at once
    crd::u64 live_bytes         = 0U; // bytes of the live ones, for classes that have a size
    crd::u64 unmatched_releases = 0U; // releases that matched nothing and changed nothing
};

struct LifetimeSnapshot
{
    LifetimeCounts classes[kLifetimeClassCount]{};

    [[nodiscard]] const LifetimeCounts& of(LifetimeClass c) const noexcept
    {
        return classes[static_cast<crd::usize>(c)];
    }
};

class LifetimeLedger
{
public:
    constexpr LifetimeLedger() noexcept = default;

    LifetimeLedger(const LifetimeLedger&)            = delete;
    LifetimeLedger& operator=(const LifetimeLedger&) = delete;
    LifetimeLedger(LifetimeLedger&&)                 = delete;
    LifetimeLedger& operator=(LifetimeLedger&&)      = delete;

    // One `c` was granted; `bytes` is its size where the class has one (0 otherwise).
    void acquired(LifetimeClass c, crd::u64 bytes = 0U) noexcept;

    // One `c` was taken back. Returns false, and changes no count but `unmatched_releases`, when nothing of `c` is
    // live. When the count matches but more bytes are released than are live, the count is released, the bytes are
    // left as they were, the release is counted as unmatched and false is returned.
    bool released(LifetimeClass c, crd::u64 bytes = 0U) noexcept;

    [[nodiscard]] LifetimeSnapshot snapshot() const noexcept;

private:
    struct Slot
    {
        std::atomic<crd::u64> acquired{0U};
        std::atomic<crd::u64> released{0U};
        std::atomic<crd::u64> live{0U};
        std::atomic<crd::u64> high_water{0U};
        std::atomic<crd::u64> live_bytes{0U};
        std::atomic<crd::u64> unmatched{0U};
    };

    Slot m_slots[kLifetimeClassCount]{};
};

// The process-wide ledger every wired engine site records into. Constant-initialized, never destroyed early.
[[nodiscard]] LifetimeLedger& lifetime_ledger() noexcept;

// One acquisition an owner holds in the process-wide ledger, for an owner whose release is implicit (a COM smart
// pointer, a member destroyed with its owner). `hold()` records the acquisition once the thing was granted, and
// `reset()` or destruction records the release. Declare it before the member it accounts for, so the member is
// released first and the ledger never shows a release that has not happened yet. A move transfers the hold.
class LifetimeHold
{
public:
    constexpr explicit LifetimeHold(LifetimeClass c) noexcept : m_class(c) {}
    ~LifetimeHold() noexcept
    {
        reset();
    }

    LifetimeHold(const LifetimeHold&)            = delete;
    LifetimeHold& operator=(const LifetimeHold&) = delete;
    LifetimeHold(LifetimeHold&& other) noexcept : m_class(other.m_class), m_held(other.m_held)
    {
        other.m_held = false;
    }
    LifetimeHold& operator=(LifetimeHold&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_class      = other.m_class;
            m_held       = other.m_held;
            other.m_held = false;
        }
        return *this;
    }

    // Record the acquisition; a second call while held records nothing.
    void hold() noexcept
    {
        if (!m_held)
        {
            lifetime_ledger().acquired(m_class);
            m_held = true;
        }
    }
    // Record the release, if held.
    void reset() noexcept
    {
        if (m_held)
        {
            m_held = false;
            (void)lifetime_ledger().released(m_class);
        }
    }
    [[nodiscard]] bool held() const noexcept
    {
        return m_held;
    }

private:
    LifetimeClass m_class;
    bool          m_held = false;
};

// What a window between two snapshots left behind, the worst finding first.
// NOLINTNEXTLINE(performance-enum-size)
enum class LifetimeVerdict : crd::u8
{
    Balanced,    // every class ends with the live count and bytes it started with
    Outstanding, // a class holds more (count or bytes) than when the window opened
    Drained,     // nothing outstanding, and a class holds less: it released things acquired before the window
    Underflowed, // a release in the window matched nothing: the accounting itself is wrong
};
[[nodiscard]] const char* lifetime_verdict_name(LifetimeVerdict v) noexcept;

struct LifetimeReport
{
    LifetimeVerdict verdict     = LifetimeVerdict::Balanced;
    crd::u8         outstanding = 0U; // lifetime_class_bit() of each class that holds more
    crd::u8         drained     = 0U; // ... that holds less (and not more)
    crd::u8         underflowed = 0U; // ... that saw an unmatched release
    crd::i64        live_delta[kLifetimeClassCount]{};
    crd::i64        byte_delta[kLifetimeClassCount]{};

    [[nodiscard]] crd::i64 live_change(LifetimeClass c) const noexcept
    {
        return live_delta[static_cast<crd::usize>(c)];
    }
    [[nodiscard]] crd::i64 byte_change(LifetimeClass c) const noexcept
    {
        return byte_delta[static_cast<crd::usize>(c)];
    }
};

// Judge the window from `before` to `after` (two snapshots of one ledger, `before` taken first).
[[nodiscard]] LifetimeReport lifetime_report(const LifetimeSnapshot& before, const LifetimeSnapshot& after) noexcept;

// One line for a human or an agent, e.g. "outstanding: file-handle +1, mapped-region +1 (+65536 bytes)". Writes at
// most `capacity` bytes including the terminating NUL (a longer line is cut) and returns the length written.
crd::usize format_lifetime_report(const LifetimeReport& report, char* out, crd::usize capacity) noexcept;

} // namespace crd
