#pragma once

// crd-perf -- seeded, authorized fault injection (DIAG.9b).
//
// A FaultInjector holds one armed FaultPlan: a seed and up to kFaultMaxRules rules, each naming a site, the site's
// hit ordinals it covers and the share of them that fail. A site is an existing seam that already has a failure
// path; injecting there drives that path, never a test-only one:
//
//   allocation-failure    FaultAllocator (an IAllocator decorator): try_allocate returns null. allocate() is never
//                         failed: its contract is non-null or fatal, so an injected failure there is not recoverable.
//   failed-read           the I/O fault seam (crd/core/fault_seam.hpp), every read: the read fails.
//   partial-read          the same, every read: the read delivers `value` bytes (clamped below the request) and
//                         stops. A synchronous read then reports failure; an AsyncFile completion reports Short.
//   delayed-completion    the same, AsyncFile completions only: the completion arrives `value` ns late.
//   cancelled-completion  the same, AsyncFile completions only: the completion reports Cancelled.
//
// Determinism. Each site counts its hits from 0 while a plan is armed. Whether hit n of a site fires is a pure
// function of (seed, site, n, the rules) -- fault_plan_fires() -- so the same plan over the same sequence of calls
// injects the same faults, and a fired fault is reproduced alone by the one-hit plan its recipe names. A read
// consults every read site it is eligible for (so each site's ordinals count reads), then applies the first site that
// fires in the order failed-read, partial-read, cancelled-completion, delayed-completion.
//
// Evidence. Every fired fault is recorded into the attached FlightRecorder (kFlightCodeFaultInjected, a = site,
// b = hit). With FaultPlan::trigger_flight it also registers the recipe as the recorder's replay recipe and fires an
// InjectedFault trigger, so the capture holds the window around the fault even when the faulty operation cannot
// complete. The recipe is "fault-plan/1 fired=<site>#<hit> minimal=<one-hit plan text> plan=<armed plan text>"; the
// one-hit plan comes first, so a recipe clipped to the recorder's bound (and marked "...") still reproduces the fault.
//
// While a plan is armed the injector owns the recorder's replay recipe: arm() registers "fault-plan/1 plan=<text>"
// (a capture taken before any fault fired is reproduced by the plan), each fired fault registers its own recipe, and
// disarm() marks replay unavailable, since neither recipe reproduces a later failure; the host registers its own
// again after disarming. A capture keeps the recipe that was registered when its trigger fired.
//
// Authority and build. arm() needs DiagAuthority::Inject in the caller's grant (no other grant implies it) and a build
// with the fault seams (crd::fault_seams_compiled(): every build but shipping). A shipping build refuses every arm as
// Unavailable, so its decorators pass every call through. The fault.* diag commands (register_fault_commands) are the
// only request route, and they carry the service's grant. Contract: docs/design/runtime-diagnostics.md (DIAG.9b);
// ADR-0133.

#include <crd/perf/diag_commands.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>
#include <mutex>

namespace crd::perf
{
namespace cont = crd::containers;

class FlightRecorder;

enum class FaultSite : crd::u8
{
    AllocationFailure   = 0U,
    FailedRead          = 1U,
    PartialRead         = 2U,
    DelayedCompletion   = 3U,
    CancelledCompletion = 4U,
};

inline constexpr crd::u32 kFaultSiteCount = 5U;

// "allocation-failure", "failed-read", "partial-read", "delayed-completion", "cancelled-completion".
[[nodiscard]] cont::StringView fault_site_name(FaultSite s) noexcept;
[[nodiscard]] bool             parse_fault_site(cont::StringView text, FaultSite& out) noexcept;

inline constexpr crd::u32 kFaultPlanVersion = 1U;
inline constexpr crd::u32 kFaultMaxRules    = 8U;
inline constexpr crd::u32 kFaultPpmAlways   = 1000000U;
inline constexpr crd::u64 kFaultMaxDelayNs  = 10ULL * 1000ULL * 1000ULL * 1000ULL; // a delay rule's bound (10 s)
inline constexpr crd::u32 kFaultMaxPlanText = 800U; // any plan's text fits unclipped; parse refuses longer text
inline constexpr crd::u32 kFaultRecipeBytes = 256U; // a recipe's bound, NUL included (the flight recorder's)

struct FaultRule
{
    FaultSite site            = FaultSite::AllocationFailure;
    crd::u64  first_hit       = 0U;              // the site's first covered hit ordinal (0-based)
    crd::u64  hit_limit       = 1U;              // covered hits from first_hit; 0 = every later hit
    crd::u32  probability_ppm = kFaultPpmAlways; // of the covered hits, the share that fires (seeded), 1..1000000
    crd::u64  value           = 0U;              // partial-read: bytes delivered; delayed-completion: ns; else 0
};

struct FaultPlan
{
    crd::u32  schema_version = kFaultPlanVersion;
    crd::u64  seed           = 0U;
    FaultRule rules[kFaultMaxRules];
    crd::u32  rule_count     = 0U;
    bool      trigger_flight = true; // a fired fault also triggers the attached flight recorder
};

enum class FaultPlanError : crd::u8
{
    None,
    UnknownVersion, // schema_version is not kFaultPlanVersion
    NoRules,        // rule_count is 0 or above kFaultMaxRules
    BadProbability, // a rule's probability_ppm is 0 or above kFaultPpmAlways
    BadValue,       // a value on a site that takes none, or a delay above kFaultMaxDelayNs, or a zero delay
    RangeOverflow,  // first_hit + hit_limit overflows
};

// "none", "unknown-version", "no-rules", "bad-probability", "bad-value", "range-overflow".
[[nodiscard]] cont::StringView fault_plan_error_name(FaultPlanError e) noexcept;
[[nodiscard]] FaultPlanError   validate_fault_plan(const FaultPlan& plan) noexcept;

// Whether hit `hit` of `site` fires under `plan`; `rule_index` names the first rule that fires it. Pure.
[[nodiscard]] bool fault_plan_fires(const FaultPlan& plan, FaultSite site, crd::u64 hit,
                                    crd::u32* rule_index = nullptr) noexcept;

// The plan's text: "seed=<n>[;notrigger];<site>@<first>+<limit>%<ppm>[=<value>]..." (rules in order; "=<value>" only
// when non-zero). parse_fault_plan reads exactly that grammar back and validates the plan; it returns false with
// `reason` set (and `out` untouched) on anything else.
void               fault_plan_to_text(const FaultPlan& plan, cont::String& out);
[[nodiscard]] bool parse_fault_plan(cont::StringView text, FaultPlan& out, cont::String& reason);

enum class FaultArmStatus : crd::u8
{
    Armed,
    Unauthorized, // the grant lacks DiagAuthority::Inject
    Unavailable,  // this build has no fault seams (shipping)
    Rejected,     // the plan failed validation (the previous plan, if any, stays armed)
};

// "armed", "unauthorized", "unavailable", "rejected".
[[nodiscard]] cont::StringView fault_arm_status_name(FaultArmStatus s) noexcept;

// One decision at a site.
struct FaultDecision
{
    bool      fire  = false;
    FaultSite site  = FaultSite::AllocationFailure;
    crd::u64  hit   = 0U; // the site's hit ordinal this decision answered
    crd::u64  value = 0U; // the firing rule's value
    crd::u32  rule  = 0U; // the firing rule's index
};

class FaultInjector
{
public:
    FaultInjector() = default;
    ~FaultInjector() = default;

    FaultInjector(const FaultInjector&)            = delete;
    FaultInjector& operator=(const FaultInjector&) = delete;
    FaultInjector(FaultInjector&&)                 = delete;
    FaultInjector& operator=(FaultInjector&&)      = delete;

    // Arm `plan` (replacing an armed one and restarting every site's hit count) when `grant` holds Inject and the
    // build has fault seams. `error`, when given, receives the validation error of a Rejected plan.
    FaultArmStatus arm(const FaultPlan& plan, DiagAuthoritySet grant, FaultPlanError* error = nullptr) noexcept;
    void           disarm() noexcept;

    [[nodiscard]] bool     armed() const noexcept { return m_armed.load(std::memory_order_acquire); }
    [[nodiscard]] crd::u64 generation() const noexcept; // arms so far
    [[nodiscard]] bool     plan(FaultPlan& out) const noexcept; // false when disarmed

    // Count one hit of `site` and decide it. Never fires while disarmed (the hit is not counted). Thread-safe.
    FaultDecision decide(FaultSite site) noexcept;

    // Hits and fired faults per site since the last arm.
    [[nodiscard]] crd::u64 hits(FaultSite site) const noexcept;
    [[nodiscard]] crd::u64 fired(FaultSite site) const noexcept;

    // Where fired faults are recorded and whose replay recipe an armed plan owns (null: nowhere). Set before arming;
    // the recorder must outlive the injector's use of it.
    void attach_flight_recorder(FlightRecorder* recorder) noexcept;

    // The recipe of the last fired fault ("" before one fired since the last arm).
    void last_recipe(cont::String& out) const;

    // The I/O fault seam's hook (crd::install_io_fault_hook(&FaultInjector::io_hook, injector)).
    static crd::IoFault io_hook(void* injector, crd::IoFaultOp op, const char* path, crd::usize path_bytes,
                                crd::u64 offset, crd::u64 size) noexcept;

private:
    // Count one hit of `site` and decide it, without recording a fire; commit() records a firing decision (its fired
    // count, recipe, flight event and trigger). io_hook evaluates every eligible site and commits only the one it
    // applies.
    FaultDecision evaluate(FaultSite site) noexcept;
    void          commit(const FaultDecision& d) noexcept;

    mutable std::mutex m_mutex;
    std::mutex         m_recorder_mutex; // orders the recipe registrations and triggers this injector makes
    FaultPlan          m_plan;
    std::atomic<bool>  m_armed{false};
    crd::u64           m_generation = 0U;
    crd::u64           m_hits[kFaultSiteCount]   = {};
    crd::u64           m_fired[kFaultSiteCount]  = {};
    FlightRecorder*    m_recorder                = nullptr;
    char               m_recipe[kFaultRecipeBytes] = {};
    crd::u32           m_recipe_length             = 0U;
};

// IAllocator decorator for the allocation-failure site: try_allocate asks the injector first and returns null when
// the fault fires; every other call passes through. Its own MemoryStats stay zero (the backing allocator keeps them).
class FaultAllocator final : public crd::memory::IAllocator
{
public:
    FaultAllocator(crd::memory::IAllocator* backing, FaultInjector* injector,
                   const char* name = "FaultAllocator") noexcept;

    void* allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override;
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override;
    void  deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    [[nodiscard]] crd::usize allocation_size(const void* p) const noexcept override;

    [[nodiscard]] crd::u64 injected() const noexcept { return m_injected.load(std::memory_order_relaxed); }

private:
    crd::memory::IAllocator* m_backing;
    FaultInjector*           m_injector;
    std::atomic<crd::u64>    m_injected{0U};
};

// The fault.* commands, owner "perf":
//   fault.arm     Inject  arm the plan in argument `plan` (parse_fault_plan text) with the service's grant
//   fault.disarm  Inject  disarm
//   fault.status  Read    whether the build has fault seams, the armed plan, and each site's hits and fired faults
// Returns false when any of them could not be registered. `injector` must outlive the service.
[[nodiscard]] bool register_fault_commands(DiagCommandService& service, FaultInjector& injector);

} // namespace crd::perf
