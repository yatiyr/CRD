#pragma once

// crd-ceir — the typed ATTRIBUTE value model (CEIR-1c, §7/§8). Attributes are the compile-time, side-band data on an
// op (a func's visibility, a call's callee symbol, a constant's value, ...). Values are INTERNED by the Context:
// identical values share one `AttrId`, so equality is a u32 compare and repeats cost nothing. This is the starter
// kind set; CEIR-2/3 extend it (arrays, dictionaries, typed constants) behind the same `AttrValue`.

#include <crd/ceir/id.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <cstring> // std::memcpy (f64 <-> bit pattern for exact intern equality)

namespace crd::ceir
{
// NOLINTNEXTLINE(performance-enum-size)
enum class AttrKind : u8
{
    Int = 0,   // i64
    Float,     // f64 (interned by bit pattern — exact, NaN-stable)
    Bool,      // bool
    String,    // an arena-interned StringView (free-form text)
    SymbolRef, // an arena-interned StringView naming a symbol (a func, ...) — resolved via a SymbolTable
    Type,      // a TypeId
    // ── CEIR-8b aggregate + wrapper kinds (ADR-0112) — append at END (the binary out-of-range reject; no version bump) ──
    Array,      // an ordered `elems` span of AttrIds (heterogeneous allowed)
    Dict,       // a string→value map: parallel `keys` (byte-order SORTED, canonical) + `elems` values
    TypedConst, // `wrapped_type` (a TypeId) + a single `payload` value — a value OF a type (the units story)
    Extern,     // `attr_class` (a dialect-defined attribute-class) + `version` + a single `payload` value
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
    // ── CEIR-8b (ADR-0112) — every field is DEFAULT unless its kind uses it (attr_is_canonical enforces this) ──
    containers::ConstSpan<AttrId>                 elems;              // Array/Dict VALUES only (span; deep-copied by intern)
    containers::ConstSpan<containers::StringView> keys;               // Dict keys only (parallel to elems; SORTED canonical)
    AttrId                                        payload;            // TypedConst/Extern single wrapped value (0 otherwise)
    AttrClassId                                   attr_class;         // Extern only (the dialect-defined attribute-class)
    u32                                           attr_class_version = 0; // Extern only (the class schema version)
    TypeId                                        wrapped_type;       // TypedConst only (the declared type of the payload)

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
    // CEIR-8b aggregates + wrappers. The spans are BORROWED (intern_attr deep-copies into the arena, like intern_type).
    // ⛔ Dict keys must be SORTED byte-order (the Context factory `attr_dict` canonicalizes; a hand-built unsorted dict
    // fails attr_is_canonical). A wrapper's `payload` must NOT itself be a wrapper (the qty<qty> composition rule).
    [[nodiscard]] static AttrValue of_array(containers::ConstSpan<AttrId> elems) noexcept
    {
        AttrValue a;
        a.kind  = AttrKind::Array;
        a.elems = elems;
        return a;
    }
    [[nodiscard]] static AttrValue of_dict(containers::ConstSpan<containers::StringView> keys,
                                           containers::ConstSpan<AttrId>                 values) noexcept
    {
        AttrValue a;
        a.kind  = AttrKind::Dict;
        a.keys  = keys;
        a.elems = values;
        return a;
    }
    [[nodiscard]] static AttrValue of_typed_const(TypeId ty, AttrId value) noexcept
    {
        AttrValue a;
        a.kind         = AttrKind::TypedConst;
        a.wrapped_type = ty;
        a.payload      = value;
        return a;
    }
    [[nodiscard]] static AttrValue of_extern(AttrClassId cls, u32 version, AttrId value) noexcept
    {
        AttrValue a;
        a.kind               = AttrKind::Extern;
        a.attr_class         = cls;
        a.attr_class_version = version;
        a.payload            = value;
        return a;
    }

    [[nodiscard]] f64 as_float() const noexcept
    {
        f64 v = 0.0;
        std::memcpy(&v, &f, sizeof(v));
        return v;
    }
};

[[nodiscard]] inline bool attr_spans_eq(containers::ConstSpan<AttrId> a, containers::ConstSpan<AttrId> b) noexcept
{
    if (a.size() != b.size()) { return false; }
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}
[[nodiscard]] inline bool attr_keys_eq(containers::ConstSpan<containers::StringView> a,
                                       containers::ConstSpan<containers::StringView> b) noexcept
{
    if (a.size() != b.size()) { return false; }
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}
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
    case AttrKind::Array:     return attr_spans_eq(a.elems, b.elems);
    case AttrKind::Dict:      return attr_keys_eq(a.keys, b.keys) && attr_spans_eq(a.elems, b.elems);
    case AttrKind::TypedConst: return a.wrapped_type == b.wrapped_type && a.payload == b.payload;
    case AttrKind::Extern:
        return a.attr_class == b.attr_class && a.attr_class_version == b.attr_class_version && a.payload == b.payload;
    }
    return false;
}

// CANONICAL form (CEIR-8b, the ADR-0111 canonicality analogue): every field a kind does NOT use is at its default, so
// two values that intern to one id cannot serialize divergently. Asserted in `intern_attr`; rejected by decoder/parser.
// ⛔ `i == 0` reads the whole 8-byte scalar union (i/f/b/t overlap) — the "union unused" check for the non-scalar kinds.
[[nodiscard]] inline bool attr_is_canonical(const AttrValue& v) noexcept
{
    const bool union0 = v.i == 0;
    const bool s0     = v.s.empty();
    const bool elems0 = v.elems.size() == 0U;
    const bool keys0  = v.keys.size() == 0U;
    const bool pay0   = !v.payload.valid();
    const bool cls0   = !v.attr_class.valid() && v.attr_class_version == 0U;
    const bool wt0    = !v.wrapped_type.valid();
    switch (v.kind)
    {
    case AttrKind::Int:
    case AttrKind::Float:
    case AttrKind::Bool:
    case AttrKind::Type: // the scalar union is used; every OTHER field default
        return s0 && elems0 && keys0 && pay0 && cls0 && wt0;
    case AttrKind::String:
    case AttrKind::SymbolRef: // `s` used
        return union0 && elems0 && keys0 && pay0 && cls0 && wt0;
    case AttrKind::Array: // `elems` used
        return union0 && s0 && keys0 && pay0 && cls0 && wt0;
    case AttrKind::Dict: // `keys` + `elems` used, parallel + keys strictly byte-order sorted (dedup + hash stability)
    {
        if (!(union0 && s0 && pay0 && cls0 && wt0)) { return false; }
        if (v.keys.size() != v.elems.size()) { return false; }
        for (usize i = 1; i < v.keys.size(); ++i)
        {
            const containers::StringView p = v.keys[i - 1U];
            const containers::StringView q = v.keys[i];
            const usize                  n = p.size() < q.size() ? p.size() : q.size();
            usize                        k = 0;
            while (k < n && p[k] == q[k]) { ++k; }
            const bool p_less = (k < n) ? (static_cast<unsigned char>(p[k]) < static_cast<unsigned char>(q[k]))
                                        : (p.size() < q.size());
            if (!p_less) { return false; } // not strictly increasing ⇒ unsorted or a duplicate key
        }
        return true;
    }
    case AttrKind::TypedConst: // `wrapped_type` + `payload` used
        return union0 && s0 && elems0 && keys0 && cls0 && v.wrapped_type.valid() && v.payload.valid();
    case AttrKind::Extern: // `attr_class` + `version` + `payload` used
        return union0 && s0 && elems0 && keys0 && wt0 && v.attr_class.valid() && v.payload.valid();
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
