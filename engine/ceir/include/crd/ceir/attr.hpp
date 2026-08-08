#pragma once

// crd-ceir — the typed ATTRIBUTE value model (CEIR-1c, §7/§8). Attributes are the compile-time, side-band data on an
// op (a func's visibility, a call's callee symbol, a constant's value, ...). Values are INTERNED by the Context:
// identical values share one `AttrId`, so equality is a u32 compare and repeats cost nothing. This is the starter
// kind set; CEIR-2/3 extend it (arrays, dictionaries, typed constants) behind the same `AttrValue`.

#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <cstring> // std::memcpy (f64 <-> bit pattern for exact intern equality)

namespace crd::ceir
{
enum class AttrKind : u8
{
    Int = 0,   // i64
    Float,     // f64 (interned by bit pattern — exact, NaN-stable)
    Bool,      // bool
    String,    // an arena-interned StringView (free-form text)
    SymbolRef, // an arena-interned StringView naming a symbol (a func, ...) — resolved via a SymbolTable
    Type,      // a TypeId
};

// One attribute value. A tagged trivial union for the scalar kinds + a StringView for the text kinds. Constructed
// through the factories so the tag and the active field never disagree. Stored BY VALUE in the Context intern table
// (only unique values are kept), so its size is not on any hot path.
struct AttrValue
{
    AttrKind kind = AttrKind::Int;
    union
    {
        i64    i = 0; // Int
        u64    f;     // Float — the f64 bit pattern (exact equality/hash; -0.0 != +0.0 by bits, NaN == same NaN)
        bool   b;     // Bool
        TypeId t;     // Type
    };
    containers::StringView s; // String / SymbolRef only

    [[nodiscard]] static AttrValue of_int(i64 v) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::Int;
        a.i    = v;
        return a;
    }
    [[nodiscard]] static AttrValue of_float(f64 v) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::Float;
        std::memcpy(&a.f, &v, sizeof(v));
        return a;
    }
    [[nodiscard]] static AttrValue of_bool(bool v) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::Bool;
        a.b    = v;
        return a;
    }
    [[nodiscard]] static AttrValue of_string(containers::StringView interned) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::String;
        a.s    = interned;
        return a;
    }
    [[nodiscard]] static AttrValue of_symbol(containers::StringView interned) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::SymbolRef;
        a.s    = interned;
        return a;
    }
    [[nodiscard]] static AttrValue of_type(TypeId t) noexcept
    {
        AttrValue a;
        a.kind = AttrKind::Type;
        a.t    = t;
        return a;
    }

    [[nodiscard]] f64 as_float() const noexcept
    {
        f64 v = 0.0;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    }
};

[[nodiscard]] inline bool operator==(const AttrValue& a, const AttrValue& b) noexcept
{
    if (a.kind != b.kind) { return false; }
    switch (a.kind)
    {
    case AttrKind::Int:       return a.i == b.i;
    case AttrKind::Float:     return a.f == b.f; // by bit pattern
    case AttrKind::Bool:      return a.b == b.b;
    case AttrKind::String:
    case AttrKind::SymbolRef: return a.s == b.s; // StringView content equality
    case AttrKind::Type:      return a.t == b.t;
    }
    return false;
}

// One named attribute on an op: an interned name → an interned value.
struct NamedAttr
{
    containers::StringView name;
    AttrId                 value;
};
} // namespace crd::ceir
