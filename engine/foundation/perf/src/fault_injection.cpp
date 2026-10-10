// crd-perf -- seeded, authorized fault injection (DIAG.9b). See crd/perf/fault_injection.hpp.

#include <crd/perf/fault_injection.hpp>

#include <crd/perf/diag_assets.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <cstdio>
#include <cstring>

namespace crd::perf
{
static_assert(kFaultRecipeBytes == kFlightRecipeBytes, "a fault recipe is the flight recorder's replay recipe");

namespace
{
constexpr cont::StringView kNoSeams{"this build has no fault seams (shipping): injection is compiled out"};
constexpr cont::StringView kDisarmed{"the fault plan that owned the replay recipe was disarmed; no recipe since"};

// The longest text write_plan can produce for any plan, valid or not: "seed=" and 20 digits, ";notrigger", then 8
// rules of ";" <longest site name> "@" 20 digits "+" 20 digits "%" 10 digits "=" 20 digits.
constexpr crd::u32 kLongestSiteName = 20U; // "cancelled-completion"
constexpr crd::u32 kLongestRuleText = 1U + kLongestSiteName + 1U + 20U + 1U + 20U + 1U + 10U + 1U + 20U;
static_assert(5U + 20U + 10U + (kFaultMaxRules * kLongestRuleText) <= kFaultMaxPlanText,
              "fault_plan_to_text must never clip a plan");

// A bounded text writer over a caller's char buffer: never writes past `capacity - 1` bytes, keeps the text
// NUL-terminated and remembers whether anything was cut.
class FixedText
{
public:
    FixedText(char* buffer, crd::u32 capacity) noexcept : m_buffer(buffer), m_capacity(capacity)
    {
        m_buffer[0] = '\0';
    }

    void put(cont::StringView text) noexcept
    {
        for (crd::usize i = 0U; i < text.size(); ++i)
        {
            if (m_length + 1U >= m_capacity)
            {
                m_overflow         = true;
                m_buffer[m_length] = '\0';
                return;
            }
            m_buffer[m_length] = text[i];
            ++m_length;
        }
        m_buffer[m_length] = '\0';
    }

    void put(const char* text) noexcept { put(cont::StringView{text}); }

    void put_u64(crd::u64 v) noexcept
    {
        char digits[24];
        (void)std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(v));
        put(digits);
    }

    // When something was cut, overwrite the last three bytes with "..." so the clip is never silent.
    void mark_if_clipped() noexcept
    {
        if (!m_overflow || m_length < 3U)
        {
            return;
        }
        for (crd::u32 i = m_length - 3U; i < m_length; ++i)
        {
            m_buffer[i] = '.';
        }
    }

    [[nodiscard]] crd::u32         length() const noexcept { return m_length; }
    [[nodiscard]] bool             overflow() const noexcept { return m_overflow; }
    [[nodiscard]] cont::StringView view() const noexcept { return cont::StringView{m_buffer, m_length}; }

private:
    char*    m_buffer;
    crd::u32 m_capacity;
    crd::u32 m_length   = 0U;
    bool     m_overflow = false;
};

void write_rule(FixedText& out, const FaultRule& r)
{
    out.put(fault_site_name(r.site));
    out.put("@");
    out.put_u64(r.first_hit);
    out.put("+");
    out.put_u64(r.hit_limit);
    out.put("%");
    out.put_u64(r.probability_ppm);
    if (r.value != 0U)
    {
        out.put("=");
        out.put_u64(r.value);
    }
}

void write_plan(FixedText& out, const FaultPlan& plan)
{
    out.put("seed=");
    out.put_u64(plan.seed);
    if (!plan.trigger_flight)
    {
        out.put(";notrigger");
    }
    for (crd::u32 i = 0U; i < plan.rule_count && i < kFaultMaxRules; ++i)
    {
        out.put(";");
        write_rule(out, plan.rules[i]);
    }
}

[[nodiscard]] crd::u64 mix64(crd::u64 x) noexcept
{
    // splitmix64's finalizer: a fixed, well-distributed hash of (seed, site, rule, hit).
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

// Parse a decimal u64 that must make up all of `text`.
[[nodiscard]] bool parse_u64(cont::StringView text, crd::u64& out) noexcept
{
    if (text.empty() || text.size() > 20U)
    {
        return false;
    }
    crd::u64 v = 0U;
    for (crd::usize i = 0U; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        const auto digit = static_cast<crd::u64>(c - '0');
        if (v > (~0ULL - digit) / 10U)
        {
            return false;
        }
        v = v * 10U + digit;
    }
    out = v;
    return true;
}

[[nodiscard]] crd::usize find_char(cont::StringView text, char c, crd::usize from) noexcept
{
    for (crd::usize i = from; i < text.size(); ++i)
    {
        if (text[i] == c)
        {
            return i;
        }
    }
    return text.size();
}

[[nodiscard]] cont::StringView slice(cont::StringView text, crd::usize begin, crd::usize end) noexcept
{
    return cont::StringView{text.data() + begin, end - begin};
}

// "<site>@<first>+<limit>%<ppm>[=<value>]"
[[nodiscard]] bool parse_rule(cont::StringView text, FaultRule& out) noexcept
{
    const crd::usize at      = find_char(text, '@', 0U);
    const crd::usize plus    = find_char(text, '+', at);
    const crd::usize percent = find_char(text, '%', plus);
    const crd::usize equals  = find_char(text, '=', percent);
    if (at >= text.size() || plus >= text.size() || percent >= text.size())
    {
        return false;
    }
    FaultRule r;
    crd::u64  ppm = 0U;
    if (!parse_fault_site(slice(text, 0U, at), r.site) || !parse_u64(slice(text, at + 1U, plus), r.first_hit) ||
        !parse_u64(slice(text, plus + 1U, percent), r.hit_limit) ||
        !parse_u64(slice(text, percent + 1U, equals), ppm) || ppm > kFaultPpmAlways)
    {
        return false;
    }
    r.probability_ppm = static_cast<crd::u32>(ppm);
    if (equals < text.size() && !parse_u64(slice(text, equals + 1U, text.size()), r.value))
    {
        return false;
    }
    out = r;
    return true;
}
} // namespace

cont::StringView fault_site_name(FaultSite s) noexcept
{
    switch (s) // no default: every site is named
    {
    case FaultSite::AllocationFailure:   return cont::StringView{"allocation-failure"};
    case FaultSite::FailedRead:          return cont::StringView{"failed-read"};
    case FaultSite::PartialRead:         return cont::StringView{"partial-read"};
    case FaultSite::DelayedCompletion:   return cont::StringView{"delayed-completion"};
    case FaultSite::CancelledCompletion: return cont::StringView{"cancelled-completion"};
    case FaultSite::DeviceLoss:          return cont::StringView{"device-loss"};
    }
    return cont::StringView{"unknown"};
}

bool parse_fault_site(cont::StringView text, FaultSite& out) noexcept
{
    for (crd::u32 i = 0U; i < kFaultSiteCount; ++i)
    {
        const auto s = static_cast<FaultSite>(i);
        if (fault_site_name(s) == text)
        {
            out = s;
            return true;
        }
    }
    return false;
}

cont::StringView fault_plan_error_name(FaultPlanError e) noexcept
{
    switch (e) // no default: every error is named
    {
    case FaultPlanError::None:           return cont::StringView{"none"};
    case FaultPlanError::UnknownVersion: return cont::StringView{"unknown-version"};
    case FaultPlanError::NoRules:        return cont::StringView{"no-rules"};
    case FaultPlanError::BadProbability: return cont::StringView{"bad-probability"};
    case FaultPlanError::BadValue:       return cont::StringView{"bad-value"};
    case FaultPlanError::RangeOverflow:  return cont::StringView{"range-overflow"};
    }
    return cont::StringView{"unknown"};
}

FaultPlanError validate_fault_plan(const FaultPlan& plan) noexcept
{
    if (plan.schema_version != kFaultPlanVersion)
    {
        return FaultPlanError::UnknownVersion;
    }
    if (plan.rule_count == 0U || plan.rule_count > kFaultMaxRules)
    {
        return FaultPlanError::NoRules;
    }
    for (crd::u32 i = 0U; i < plan.rule_count; ++i)
    {
        const FaultRule& r = plan.rules[i];
        if (static_cast<crd::u32>(r.site) >= kFaultSiteCount)
        {
            return FaultPlanError::BadValue;
        }
        if (r.probability_ppm == 0U || r.probability_ppm > kFaultPpmAlways)
        {
            return FaultPlanError::BadProbability;
        }
        if (r.hit_limit != 0U && r.first_hit > ~0ULL - r.hit_limit)
        {
            return FaultPlanError::RangeOverflow;
        }
        const bool delay_ok = r.site == FaultSite::DelayedCompletion && r.value != 0U && r.value <= kFaultMaxDelayNs;
        const bool value_ok = r.site == FaultSite::PartialRead || delay_ok ||
                              (r.site != FaultSite::DelayedCompletion && r.value == 0U);
        if (!value_ok)
        {
            return FaultPlanError::BadValue;
        }
    }
    return FaultPlanError::None;
}

bool fault_plan_fires(const FaultPlan& plan, FaultSite site, crd::u64 hit, crd::u32* rule_index) noexcept
{
    for (crd::u32 i = 0U; i < plan.rule_count && i < kFaultMaxRules; ++i)
    {
        const FaultRule& r = plan.rules[i];
        if (r.site != site || hit < r.first_hit || (r.hit_limit != 0U && hit - r.first_hit >= r.hit_limit))
        {
            continue;
        }
        bool fires = r.probability_ppm >= kFaultPpmAlways;
        if (!fires)
        {
            const crd::u64 key = plan.seed ^ (static_cast<crd::u64>(site) << 56U) ^ (static_cast<crd::u64>(i) << 48U);
            fires              = mix64(mix64(key) ^ hit) % kFaultPpmAlways < r.probability_ppm;
        }
        if (fires)
        {
            if (rule_index != nullptr)
            {
                *rule_index = i;
            }
            return true;
        }
    }
    return false;
}

void fault_plan_to_text(const FaultPlan& plan, cont::String& out)
{
    char      buffer[kFaultMaxPlanText + 1U];
    FixedText text(buffer, sizeof(buffer));
    write_plan(text, plan);
    out.clear();
    out.append(text.view());
}

bool parse_fault_plan(cont::StringView text, FaultPlan& out, cont::String& reason)
{
    if (text.size() > kFaultMaxPlanText)
    {
        reason.append("the plan text is longer than 800 bytes");
        return false;
    }
    FaultPlan  plan;
    crd::usize begin = 0U;
    crd::u32   item  = 0U;
    while (begin <= text.size())
    {
        const crd::usize       end   = find_char(text, ';', begin);
        const cont::StringView field = slice(text, begin, end);
        if (item == 0U)
        {
            const cont::StringView prefix{"seed="};
            if (field.size() <= prefix.size() || slice(field, 0U, prefix.size()) != prefix ||
                !parse_u64(slice(field, prefix.size(), field.size()), plan.seed))
            {
                reason.append("the plan must start with seed=<decimal>");
                return false;
            }
        }
        else if (item == 1U && field == cont::StringView{"notrigger"})
        {
            plan.trigger_flight = false;
        }
        else
        {
            if (plan.rule_count == kFaultMaxRules)
            {
                reason.append("more than 8 rules");
                return false;
            }
            if (!parse_rule(field, plan.rules[plan.rule_count]))
            {
                reason.append("a rule is not <site>@<first>+<limit>%<ppm>[=<value>]: ");
                reason.append(field);
                return false;
            }
            ++plan.rule_count;
        }
        ++item;
        begin = end + 1U;
    }
    const FaultPlanError e = validate_fault_plan(plan);
    if (e != FaultPlanError::None)
    {
        reason.append("the plan is invalid: ");
        reason.append(fault_plan_error_name(e));
        return false;
    }
    out = plan;
    return true;
}

cont::StringView fault_arm_status_name(FaultArmStatus s) noexcept
{
    switch (s) // no default: every status is named
    {
    case FaultArmStatus::Armed:        return cont::StringView{"armed"};
    case FaultArmStatus::Unauthorized: return cont::StringView{"unauthorized"};
    case FaultArmStatus::Unavailable:  return cont::StringView{"unavailable"};
    case FaultArmStatus::Rejected:     return cont::StringView{"rejected"};
    }
    return cont::StringView{"unknown"};
}

// ---- FaultInjector ----------------------------------------------------------------------------------------------

FaultArmStatus FaultInjector::arm(const FaultPlan& plan, DiagAuthoritySet grant, FaultPlanError* error) noexcept
{
    if (!grants(grant, DiagAuthority::Inject))
    {
        return FaultArmStatus::Unauthorized;
    }
    if (!crd::fault_seams_compiled())
    {
        return FaultArmStatus::Unavailable;
    }
    const FaultPlanError e = validate_fault_plan(plan);
    if (error != nullptr)
    {
        *error = e;
    }
    if (e != FaultPlanError::None)
    {
        return FaultArmStatus::Rejected;
    }
    char            recipe[kFaultRecipeBytes];
    FlightRecorder* recorder = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_plan = plan;
        for (crd::u32 i = 0U; i < kFaultSiteCount; ++i)
        {
            m_hits[i]  = 0U;
            m_fired[i] = 0U;
        }
        m_recipe[0]     = '\0';
        m_recipe_length = 0U;
        ++m_generation;
        m_armed.store(true, std::memory_order_release);
        recorder = m_recorder;

        FixedText text(recipe, sizeof(recipe));
        text.put("fault-plan/1 plan=");
        write_plan(text, m_plan);
        text.mark_if_clipped();
    }
    // The armed plan owns the recorder's recipe: a capture taken before any fault fired is reproduced by the plan.
    if (recorder != nullptr)
    {
        const std::lock_guard<std::mutex> order(m_recorder_mutex);
        recorder->set_replay_recipe(cont::StringView{recipe});
    }
    return FaultArmStatus::Armed;
}

void FaultInjector::disarm() noexcept
{
    FlightRecorder* recorder  = nullptr;
    bool            was_armed = false;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        was_armed = m_armed.exchange(false, std::memory_order_acq_rel);
        recorder  = m_recorder;
    }
    // Neither the plan's nor a fired fault's recipe reproduces a later failure: say so until the host registers one.
    if (was_armed && recorder != nullptr)
    {
        const std::lock_guard<std::mutex> order(m_recorder_mutex);
        recorder->set_replay_unavailable(kDisarmed);
    }
}

crd::u64 FaultInjector::generation() const noexcept
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_generation;
}

bool FaultInjector::plan(FaultPlan& out) const noexcept
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_armed.load(std::memory_order_acquire))
    {
        return false;
    }
    out = m_plan;
    return true;
}

FaultDecision FaultInjector::evaluate(FaultSite site) noexcept
{
    FaultDecision d;
    d.site = site;
    const auto index = static_cast<crd::u32>(site);
    if (index >= kFaultSiteCount || !m_armed.load(std::memory_order_acquire))
    {
        return d;
    }
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (!m_armed.load(std::memory_order_acquire))
    {
        return d;
    }
    d.hit = m_hits[index];
    ++m_hits[index];
    crd::u32 rule = 0U;
    if (fault_plan_fires(m_plan, site, d.hit, &rule))
    {
        d.fire  = true;
        d.rule  = rule;
        d.value = m_plan.rules[rule].value;
    }
    return d;
}

void FaultInjector::commit(const FaultDecision& d) noexcept
{
    if (!d.fire)
    {
        return;
    }
    // Build the recipe and take what the report needs under the lock; record and trigger outside it.
    char            recipe[kFaultRecipeBytes];
    FlightRecorder* recorder       = nullptr;
    bool            trigger_flight = false;
    crd::u64        seed           = 0U;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        ++m_fired[static_cast<crd::u32>(d.site)];

        FaultPlan minimal;
        minimal.seed           = m_plan.seed;
        minimal.trigger_flight = m_plan.trigger_flight;
        minimal.rule_count     = 1U;
        minimal.rules[0]       = FaultRule{d.site, d.hit, 1U, kFaultPpmAlways, d.value};

        // The fired fault and its one-hit plan come first, so a clipped recipe still reproduces the fault.
        FixedText text(recipe, sizeof(recipe));
        text.put("fault-plan/1 fired=");
        text.put(fault_site_name(d.site));
        text.put("#");
        text.put_u64(d.hit);
        text.put(" minimal=");
        write_plan(text, minimal);
        text.put(" plan=");
        write_plan(text, m_plan);
        text.mark_if_clipped(); // the minimal plan is complete before any clip
        std::memcpy(m_recipe, recipe, sizeof(m_recipe));
        m_recipe_length = static_cast<crd::u32>(std::strlen(m_recipe));

        recorder       = m_recorder;
        trigger_flight = m_plan.trigger_flight;
        seed           = m_plan.seed;
    }
    if (recorder == nullptr)
    {
        return;
    }
    const std::lock_guard<std::mutex> order(m_recorder_mutex);
    char                              message[kFlightMessageBytes];
    (void)std::snprintf(message, sizeof(message), "injected %s at hit %llu (seed %llu)",
                        fault_site_name(d.site).data(), static_cast<unsigned long long>(d.hit),
                        static_cast<unsigned long long>(seed));
    recorder->record(kFlightCodeFaultInjected, Severity::Warning, cont::StringView{message},
                     static_cast<crd::u64>(d.site), d.hit);
    if (trigger_flight)
    {
        const cont::StringView recipe_view{recipe};
        recorder->set_replay_recipe(recipe_view);
        FlightTriggerInput input;
        input.kind    = FlightTrigger::InjectedFault;
        input.code    = kFlightCodeFaultInjected;
        input.message = recipe_view;
        input.value_a = static_cast<crd::u64>(d.site);
        input.value_b = d.hit;
        (void)recorder->trigger(input);
    }
}

FaultDecision FaultInjector::decide(FaultSite site) noexcept
{
    const FaultDecision d = evaluate(site);
    commit(d);
    return d;
}

crd::u64 FaultInjector::hits(FaultSite site) const noexcept
{
    const auto index = static_cast<crd::u32>(site);
    if (index >= kFaultSiteCount)
    {
        return 0U;
    }
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_hits[index];
}

crd::u64 FaultInjector::fired(FaultSite site) const noexcept
{
    const auto index = static_cast<crd::u32>(site);
    if (index >= kFaultSiteCount)
    {
        return 0U;
    }
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_fired[index];
}

void FaultInjector::attach_flight_recorder(FlightRecorder* recorder) noexcept
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    m_recorder = recorder;
}

void FaultInjector::last_recipe(cont::String& out) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    out.clear();
    out.append(cont::StringView{m_recipe, m_recipe_length});
}

crd::IoFault FaultInjector::io_hook(void* injector, crd::IoFaultOp op, const char* /*path*/,
                                    crd::usize /*path_bytes*/, crd::u64 /*offset*/, crd::u64 /*size*/) noexcept
{
    auto* const self = static_cast<FaultInjector*>(injector);
    if (self == nullptr || !self->armed())
    {
        return crd::IoFault{};
    }
    // Every eligible site counts this read, so each site's ordinals count reads; the first that fires applies.
    const bool    completion = op == crd::IoFaultOp::AsyncRead;
    FaultDecision decisions[4];
    crd::u32      count = 0U;
    decisions[count++]  = self->evaluate(FaultSite::FailedRead);
    decisions[count++]  = self->evaluate(FaultSite::PartialRead);
    if (completion)
    {
        decisions[count++] = self->evaluate(FaultSite::CancelledCompletion);
        decisions[count++] = self->evaluate(FaultSite::DelayedCompletion);
    }
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const FaultDecision& d = decisions[i];
        if (!d.fire)
        {
            continue;
        }
        self->commit(d);
        switch (d.site) // no default: read sites map to their actions; allocation and device loss never reach here
        {
        case FaultSite::FailedRead:          return crd::IoFault{crd::IoFaultAction::Fail, 0U};
        case FaultSite::PartialRead:         return crd::IoFault{crd::IoFaultAction::Short, d.value};
        case FaultSite::CancelledCompletion: return crd::IoFault{crd::IoFaultAction::Cancel, 0U};
        case FaultSite::DelayedCompletion:   return crd::IoFault{crd::IoFaultAction::Delay, d.value};
        case FaultSite::AllocationFailure:
        case FaultSite::DeviceLoss:          break;
        }
    }
    return crd::IoFault{};
}

crd::GpuFaultAction FaultInjector::gpu_hook(void* injector, crd::GpuFaultOp /*op*/,
                                            const char* /*operation*/) noexcept
{
    auto* const self = static_cast<FaultInjector*>(injector);
    if (self == nullptr || !self->armed())
    {
        return crd::GpuFaultAction::Proceed;
    }
    // Submit is the only GPU op, so each device-loss ordinal counts one queue submission.
    if (self->decide(FaultSite::DeviceLoss).fire)
    {
        return crd::GpuFaultAction::LoseDevice;
    }
    return crd::GpuFaultAction::Proceed;
}

// ---- FaultAllocator ---------------------------------------------------------------------------------------------

FaultAllocator::FaultAllocator(crd::memory::IAllocator* backing, FaultInjector* injector, const char* name) noexcept
    : m_backing(backing), m_injector(injector)
{
    m_name = name;
}

void* FaultAllocator::allocate(crd::usize size, crd::usize alignment)
{
    return m_backing->allocate(size, alignment);
}

void* FaultAllocator::try_allocate(crd::usize size, crd::usize alignment)
{
    if (m_injector != nullptr && size > 0U && m_injector->decide(FaultSite::AllocationFailure).fire)
    {
        m_injected.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    return m_backing->try_allocate(size, alignment);
}

void FaultAllocator::deallocate(void* p) noexcept
{
    m_backing->deallocate(p);
}

bool FaultAllocator::owns(const void* p) const noexcept
{
    return m_backing->owns(p);
}

crd::usize FaultAllocator::allocation_size(const void* p) const noexcept
{
    return m_backing->allocation_size(p);
}

// ---- fault.* commands -------------------------------------------------------------------------------------------

namespace
{
DiagStatus check_fault_arm(void* /*context*/, cont::ConstSpan<DiagArg> args, cont::String& reason)
{
    if (args.size() == 1U && args[0].name == cont::StringView{"plan"})
    {
        FaultPlan plan;
        return parse_fault_plan(args[0].value, plan, reason) ? DiagStatus::Ok : DiagStatus::BadArgument;
    }
    if (args.size() == 1U && args[0].name == cont::StringView{"asset"})
    {
        if (!diag_path_is_safe(args[0].value))
        {
            reason.append("asset must be a relative path of plain [A-Za-z0-9._-] names separated by '/'");
            return DiagStatus::BadArgument;
        }
        return DiagStatus::Ok;
    }
    reason.append("fault.arm takes exactly one argument: plan=<fault plan text> or asset=<fault-plan asset path>");
    return DiagStatus::BadArgument;
}

// Read the plan an armed request names: its text, or the fault-plan asset under the host's root.
DiagStatus request_plan(const DiagCall& call, FaultPlan& plan, cont::String& reason)
{
    const DiagArg& arg = call.request->args[0];
    if (arg.name == cont::StringView{"plan"})
    {
        return parse_fault_plan(arg.value, plan, reason) ? DiagStatus::Ok : DiagStatus::BadArgument;
    }
    if (call.root.empty())
    {
        reason.append("the host granted no file root");
        return DiagStatus::Unavailable;
    }
    cont::String path(call.root);
    const char   last = call.root[call.root.size() - 1U];
    if (last != '/' && last != '\\')
    {
        path.push_back('/');
    }
    path.append(arg.value);
    cont::String text;
    switch (read_diag_asset_file(path.c_str(), text)) // no default: every result is answered
    {
    case DiagAssetRead::Ok:
        break;
    case DiagAssetRead::Missing:
        reason.append("the fault-plan asset could not be opened");
        return DiagStatus::Failed;
    case DiagAssetRead::TooLarge:
        reason.append("the fault-plan asset is larger than 16384 bytes");
        return DiagStatus::Oversized;
    case DiagAssetRead::Failed:
        reason.append("the fault-plan asset could not be read");
        return DiagStatus::Failed;
    }
    return parse_fault_plan_asset(cont::StringView{text.data(), text.size()}, plan, reason) ? DiagStatus::Ok
                                                                                              : DiagStatus::BadArgument;
}

void write_status(const FaultInjector& injector, DiagSnapshot& out)
{
    FaultPlan    plan;
    const bool   armed = injector.plan(plan);
    cont::String text(out.allocator());
    if (armed)
    {
        fault_plan_to_text(plan, text);
    }
    out.summary.boolean("compiled", crd::fault_seams_compiled())
        .boolean("armed", armed)
        .u64("generation", injector.generation())
        .str("plan", cont::StringView{text.data(), text.size()});
    for (crd::u32 i = 0U; i < kFaultSiteCount; ++i)
    {
        const auto site = static_cast<FaultSite>(i);
        DiagFields item(out.allocator());
        item.str("site", fault_site_name(site)).u64("hits", injector.hits(site)).u64("fired", injector.fired(site));
        (void)out.add_item(item);
    }
}

DiagStatus run_fault_arm(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const injector = static_cast<FaultInjector*>(context);
    FaultPlan   plan;
    if (const DiagStatus read = request_plan(call, plan, out.reason); read != DiagStatus::Ok)
    {
        return read;
    }
    switch (injector->arm(plan, call.granted)) // no default: every arm status is answered
    {
    case FaultArmStatus::Armed:
        break;
    case FaultArmStatus::Unauthorized:
        out.reason.append("the grant lacks inject");
        return DiagStatus::Unauthorized;
    case FaultArmStatus::Unavailable:
        out.reason.append(kNoSeams);
        return DiagStatus::Unavailable;
    case FaultArmStatus::Rejected:
        out.reason.append("the plan was rejected");
        return DiagStatus::BadArgument;
    }
    write_status(*injector, out);
    return DiagStatus::Ok;
}

DiagStatus run_fault_disarm(void* context, const DiagCall& /*call*/, DiagSnapshot& out)
{
    auto* const injector = static_cast<FaultInjector*>(context);
    injector->disarm();
    write_status(*injector, out);
    return DiagStatus::Ok;
}

DiagStatus run_fault_status(void* context, const DiagCall& /*call*/, DiagSnapshot& out)
{
    write_status(*static_cast<FaultInjector*>(context), out);
    return DiagStatus::Ok;
}
} // namespace

bool register_fault_commands(DiagCommandService& service, FaultInjector& injector)
{
    DiagCommandSpec arm;
    arm.name      = cont::StringView{"fault.arm"};
    arm.owner     = cont::StringView{"perf"};
    arm.summary   = cont::StringView{"arm a seeded fault plan (plan=<seed=...;site@first+limit%ppm> or asset=<path>)"};
    arm.authority = DiagAuthority::Inject;

    DiagCommandSpec disarm;
    disarm.name      = cont::StringView{"fault.disarm"};
    disarm.owner     = cont::StringView{"perf"};
    disarm.summary   = cont::StringView{"disarm the fault plan"};
    disarm.authority = DiagAuthority::Inject;

    DiagCommandSpec status;
    status.name      = cont::StringView{"fault.status"};
    status.owner     = cont::StringView{"perf"};
    status.summary   = cont::StringView{"fault seams in this build, the armed plan, and each site's hits and faults"};
    status.authority = DiagAuthority::Read;

    const bool a = service.register_command(arm, &run_fault_arm, &injector, &check_fault_arm);
    const bool d = service.register_command(disarm, &run_fault_disarm, &injector);
    const bool s = service.register_command(status, &run_fault_status, &injector);
    return a && d && s;
}

} // namespace crd::perf
