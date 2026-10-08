#include <crd/ceir/input.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/time.hpp> // builtin_domain_index, kBuiltinDomainCount

namespace crd::ceir::input
{
namespace
{
// The splitmix64 output function (Steele, Lea and Flood): a bijective 64-bit mix.
[[nodiscard]] constexpr crd::u64 mix64(crd::u64 z) noexcept
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// An integer attribute in [lo, 2^32).
[[nodiscard]] bool u32_attr(const Context& ctx, const Operation& op, containers::StringView name, crd::i64 lo,
                            crd::u64& out) noexcept
{
    const AttrId id = op.attr(name);
    if (!id.valid())
    {
        return false;
    }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::Int || v.i < lo || v.i > static_cast<crd::i64>(0xFFFFFFFFLL))
    {
        return false;
    }
    out = static_cast<crd::u64>(v.i);
    return true;
}
} // namespace

containers::StringView input_kind_name(InputKind k) noexcept
{
    switch (k) // no default (-Werror=switch): a new kind is named here
    {
    case InputKind::Random: return containers::StringView{"random"};
    case InputKind::Clock: return containers::StringView{"clock"};
    case InputKind::TimeStep: return containers::StringView{"time_step"};
    }
    return containers::StringView{"?"};
}

bool read_input(const InputSource* source, InputKind kind, crd::u32 channel, crd::i64& out)
{
    if (source == nullptr || source->next == nullptr)
    {
        return false;
    }
    return source->next(kind, channel, out, source->user);
}

bool reads_input(const Context& ctx, OpId kind) noexcept
{
    const containers::StringView name = ctx.op_name(kind);
    return name == containers::StringView{"input.random"} || name == containers::StringView{"input.clock"} ||
           name == containers::StringView{"input.time_step"};
}

bool time_attrs(const Context& ctx, const Operation& op, crd::u32& domain) noexcept
{
    const AttrId id = op.attr("domain");
    if (!id.valid())
    {
        return false;
    }
    const AttrValue v = ctx.attr_value(id);
    if (v.kind != AttrKind::String)
    {
        return false;
    }
    return time::builtin_domain_index(v.s, domain);
}

bool random_attrs(const Context& ctx, const Operation& op, crd::u32& stream, crd::u64& bound) noexcept
{
    crd::u64 s = 0U;
    if (!u32_attr(ctx, op, "stream", 0, s) || !u32_attr(ctx, op, "bound", 1, bound))
    {
        return false;
    }
    stream = static_cast<crd::u32>(s);
    return true;
}

SeededInputs::SeededInputs(crd::u64 seed, memory::IAllocator* alloc)
    : m_seed(seed), m_drawn(alloc), m_source{&SeededInputs::next, this}
{
}

void SeededInputs::reset(crd::u64 seed)
{
    m_seed = seed;
    m_drawn.clear();
}

crd::i64 SeededInputs::draw(crd::u64 seed, crd::u32 stream, crd::u64 n) noexcept
{
    return static_cast<crd::i64>(mix64(mix64(seed ^ mix64(static_cast<crd::u64>(stream))) + n));
}

bool SeededInputs::next(InputKind kind, crd::u32 channel, crd::i64& out, void* user)
{
    if (kind != InputKind::Random)
    {
        return false;
    }
    auto&           self  = *static_cast<SeededInputs*>(user);
    crd::u64* const drawn = self.m_drawn.find(channel);
    const crd::u64  n     = drawn != nullptr ? *drawn : 0U;
    out                   = draw(self.m_seed, channel, n);
    if (drawn != nullptr)
    {
        *drawn = n + 1U;
    }
    else
    {
        (void)self.m_drawn.insert(channel, crd::u64{1});
    }
    return true;
}

static_assert(time::kBuiltinDomainCount == 6U, "HostClock keeps one slot per built-in time domain");

HostClock::HostClock() noexcept : m_source{&HostClock::next, this} {}

void HostClock::set_reading(crd::u32 domain, crd::i64 now) noexcept
{
    if (domain < time::kBuiltinDomainCount)
    {
        m_domains[domain].has_now = true;
        m_domains[domain].now     = now;
    }
}

void HostClock::set_step(crd::u32 domain, crd::i64 step) noexcept
{
    if (domain < time::kBuiltinDomainCount)
    {
        m_domains[domain].has_step = true;
        m_domains[domain].step     = step;
    }
}

void HostClock::advance(crd::u32 domain, crd::i64 step) noexcept
{
    if (domain >= time::kBuiltinDomainCount)
    {
        return;
    }
    Domain&        d    = m_domains[domain];
    const crd::u64 from = d.has_now ? static_cast<crd::u64>(d.now) : 0U;
    d.now               = static_cast<crd::i64>(from + static_cast<crd::u64>(step));
    d.has_now           = true;
    d.step              = step;
    d.has_step          = true;
}

void HostClock::use_live_wall(MonotonicReader read, void* user) noexcept
{
    m_wall_read  = read;
    m_wall_user  = user;
    m_wall_epoch = read != nullptr ? read(user) : 0;
}

void HostClock::clear() noexcept
{
    for (Domain& d : m_domains)
    {
        d = Domain{};
    }
    m_wall_read  = nullptr;
    m_wall_user  = nullptr;
    m_wall_epoch = 0;
}

bool HostClock::next(InputKind kind, crd::u32 channel, crd::i64& out, void* user)
{
    const auto& self = *static_cast<const HostClock*>(user);
    if (channel >= time::kBuiltinDomainCount || (kind != InputKind::Clock && kind != InputKind::TimeStep))
    {
        return false;
    }
    const Domain& d = self.m_domains[channel];
    if (kind == InputKind::TimeStep)
    {
        out = d.step;
        return d.has_step;
    }
    if (channel == 0U && self.m_wall_read != nullptr) // the wall domain is ordinal 0
    {
        const auto now = static_cast<crd::u64>(self.m_wall_read(self.m_wall_user));
        out            = static_cast<crd::i64>(now - static_cast<crd::u64>(self.m_wall_epoch));
        return true;
    }
    out = d.now;
    return d.has_now;
}

InputRouter::InputRouter() noexcept : m_source{&InputRouter::next, this} {}

void InputRouter::route(InputKind kind, const InputSource* to) noexcept
{
    if (static_cast<crd::u32>(kind) <= static_cast<crd::u32>(kLastInputKind))
    {
        m_routes[static_cast<crd::u32>(kind)] = to;
    }
}

bool InputRouter::next(InputKind kind, crd::u32 channel, crd::i64& out, void* user)
{
    const auto& self = *static_cast<const InputRouter*>(user);
    if (static_cast<crd::u32>(kind) > static_cast<crd::u32>(kLastInputKind))
    {
        return false;
    }
    return read_input(self.m_routes[static_cast<crd::u32>(kind)], kind, channel, out);
}
} // namespace crd::ceir::input
