#include <crd/ceir/input.hpp>

#include <crd/ceir/attr.hpp>

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
    return ctx.op_name(kind) == containers::StringView{"input.random"};
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
} // namespace crd::ceir::input
