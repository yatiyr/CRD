#pragma once

// crd-ceir — the interned TYPE model (CEIR-3a, §16). A CEIR value carries a `TypeId`, an interned handle into the
// Context's type table: identical types share one id, so type equality is a u32 compare and repeats cost nothing
// (the AttrValue interning pattern, CEIR-1c). CEIR-1 treated `TypeId` opaquely; here it gains STRUCTURE — scalars +
// aggregates (§16). Generics / interfaces / callable types are CEIR-3b; resource/view types CEIR-3c; shapes CEIR-3d;
// unit tags CEIR-3e; ownership CEIR-3f.
//
// ⛔ `kind` is a FIELD, never a method: a jump table over the CLOSED `t.kind` value-enum is fine — the open-world I6
// rule forbids only dispatching on an OP's kind, i.e. branching on the op-kind method (see check_ceir_invariants).

#include <crd/ceir/id.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/units/dim.hpp> // crd-units is link-legal for crd-ceir (ADR-0109); CEIR-3e mirrors its 8-base model

namespace crd::ceir
{
// The type constructors §16 covers. Open to extension (append at END — never renumber; the binary form encodes this
// value). Arbitrary/fixed-precision (§16 "where later useful") and resources/views (3c) are deliberately absent here.
enum class TypeKind : u8
{
    // -- scalars --
    Bool = 0,
    Int,   // `width`-bit two's-complement, `is_signed`
    Float, // `fkind` selects the format
    Index, // a target-defined size/index integer (§16 index/size type)
    // -- numeric aggregates --
    Vector,     // `count` x `elem`
    Matrix,     // `rows` x `cols` x `elem`
    Complex,    // real+imag of `elem`
    Quaternion, // w,x,y,z of `elem`
    // -- general aggregates --
    Array,   // `count` x `elem`
    Tuple,   // positional `members`
    Struct,  // `name` + named fields (`members` types, `labels` names)
    Enum,    // `name` + `labels` case names
    Variant, // positional alternatives `members` (tagged union)
    Option,  // `members[0]` or none
    Result,  // `members[0]` ok or `members[1]` error
    // -- generics (CEIR-3b, §16/§98) --
    TypeParam, // a generic type variable: `name` + `members` = the traits it must satisfy (its constraints)
    Trait,     // a nominal capability/trait contract: `name` + `members` = its supertraits
    Callable,  // a function type: `count` = number of params, `members` = params ++ results
    // -- resources + views (CEIR-3c, §23) --
    Buffer,           // `count` = BufferMode; raw has no element, else `members[0]` = element type
    Image,            // `count` = ImageDim; `members[0]` = format (an element type, backend-agnostic)
    Sampler,          // `is_signed` = comparison sampler?
    ResourceTable,    // `members[0]` = element type (a bindless table of T)
    AccelStruct,      // ray-tracing acceleration structure (niladic)
    VideoFrame,       // (niladic)
    AudioBuffer,      // (niladic)
    ExternalResource, // an imported/opaque external resource (niladic)
    View,             // `members[0]` = the underlying resource; `count` = a ViewRange presence MASK (range VALUES are
                      // runtime — the view TYPE says WHICH range dimensions are constrained, not their offsets/sizes)
    // -- shapes + tensors (CEIR-3d, §21/§35) --
    Dim,          // one dimension: `cols` = DimKind; Static uses `count` (extent), Symbolic uses `name`
    Shape,        // `members` = dims (each a Dim); rank = members.size() (rank-0 = a scalar shape)
    Tensor,       // `members` = [element, shape]
    SparseTensor, // `members` = [element, shape] (the sparse ENCODING is CEIR-18; the kind distinction is CHIR headroom)
    // -- physical quantities (CEIR-3e, §17/§18) --
    Quantity, // `members[0]` = underlying numeric type; the 8 SI base-dimension exponents pack into `count`+`cols`
    // -- ownership / lifetime qualifier (CEIR-3f, §19) --
    Qualified, // `members[0]` = the qualified type; `count` = an OwnershipKind (the §19 ownership/view category)
    // -- OPEN-WORLD extension (CEIR-8a, ADR-0111) — the ONE-TIME door for dialect-defined type-classes --
    Extern, // a dialect-defined type-class: `type_class` names it (interned "dialect.class"); parameters ride the
            // existing slots (`members`/`count`/`cols`/`is_signed`/`fkind`/`name`/`labels`). Every custom type after
            // this one lands with ZERO further TypeKind edits. The registered class's verify hook owns its arity.
};

// The §19 ownership/lifetime categories, in §19 order (append at END). A flat MUTUALLY-EXCLUSIVE enum, not a bitmask —
// §19 lists categories, not composable axes. Stored in `Type::count` on a Qualified type. An UNqualified type (a bare
// `!f32`) carries no OwnershipKind — it is distinct from `!qual<imm,!f32>` (unannotated legacy vs explicitly immutable).
enum class OwnershipKind : u8
{
    ImmutableValue = 0,
    MutableValue,
    BorrowedView,  // ⛔ a borrow may not outlive its region (the allocator-outlives-borrowers scar, IR edition) — the
                   // escape CHECK is the CEIR-3z module walk over `value_escapes_region`, not a per-op verifier
    OwnedResource,
    SharedHandle,
    WeakHandle,
    StateSlot,
    ExternalHandle,
    TransientArena,
};

// A dimension's flavor (§21). Stored in `Type::cols`. Append at END.
enum class DimKind : u8
{
    Static = 0,  // a fixed extent in `count`
    Symbolic,    // a named symbolic dim in `name` (structural-by-name, like a TypeParam — alpha-scoping is CHIR's)
    Dynamic,     // a runtime-unknown extent
};

// The result of a shape-relation query (§21 "runtime shape checks" is exactly the `Unknown` deferral to a CEIR-18
// assertion op — Unknown is a principled third state, not a failure).
enum class ShapeCompat : u8
{
    Compatible = 0,
    Incompatible,
    Unknown,
};

// A physical DIMENSION (CEIR-3e, §17). The 8 signed base-dimension exponents in the ADR-0078 / mp-units-P1935 order —
// the SAME model as `crd::units::Dim<L,M,T,I,Th,N,J,A>` (that type is compile-time-only, so CEIR carries a runtime
// mirror). Canonical grammar letter per base is in the comment; the static_assert below pins base 0 == crd-units Length.
struct QuantityDim
{
    // [0]=L(ength) [1]=M(ass) [2]=T(ime) [3]=I(current) [4]=K(temperature; crd-units `Th`) [5]=N(amount) [6]=J(luminous)
    // [7]=A(ngle). Real exponents are tiny (|e| <= ~4); i8 is generous. Dimensionless = all zero.
    i8 exp[8] = {};
    friend constexpr bool operator==(const QuantityDim&, const QuantityDim&) noexcept = default;
};
inline constexpr usize kQuantityBases = 8U;

// ⛔ Pin our base order to crd-units so the mirror is mechanically verified, not a comment that can rot.
static_assert(crd::units::Dim<1, 0, 0, 0, 0, 0, 0, 0>::length == 1
                  && crd::units::Dim<0, 0, 1, 0, 0, 0, 0, 0>::time == 1
                  && crd::units::Dim<0, 0, 0, 0, 0, 0, 0, 1>::angle == 1,
              "QuantityDim base order must mirror crd::units::Dim");

// ONE pair of pack/unpack helpers (⛔ no inline bit math anywhere else — sign-extension is the trap). Each exponent is
// stored two's-complement in its own byte; count holds L,M,T,I and cols holds K,N,J,A.
[[nodiscard]] inline u32 pack_dim_count(const QuantityDim& d) noexcept
{
    return static_cast<u32>(static_cast<u8>(d.exp[0])) | (static_cast<u32>(static_cast<u8>(d.exp[1])) << 8U)
           | (static_cast<u32>(static_cast<u8>(d.exp[2])) << 16U) | (static_cast<u32>(static_cast<u8>(d.exp[3])) << 24U);
}
[[nodiscard]] inline u32 pack_dim_cols(const QuantityDim& d) noexcept
{
    return static_cast<u32>(static_cast<u8>(d.exp[4])) | (static_cast<u32>(static_cast<u8>(d.exp[5])) << 8U)
           | (static_cast<u32>(static_cast<u8>(d.exp[6])) << 16U) | (static_cast<u32>(static_cast<u8>(d.exp[7])) << 24U);
}
[[nodiscard]] inline QuantityDim unpack_dim(u32 count, u32 cols) noexcept
{
    QuantityDim d;
    d.exp[0] = static_cast<i8>(count & 0xFFU); // static_cast<i8> re-signs each byte (the sign-extension guard)
    d.exp[1] = static_cast<i8>((count >> 8U) & 0xFFU);
    d.exp[2] = static_cast<i8>((count >> 16U) & 0xFFU);
    d.exp[3] = static_cast<i8>((count >> 24U) & 0xFFU);
    d.exp[4] = static_cast<i8>(cols & 0xFFU);
    d.exp[5] = static_cast<i8>((cols >> 8U) & 0xFFU);
    d.exp[6] = static_cast<i8>((cols >> 16U) & 0xFFU);
    d.exp[7] = static_cast<i8>((cols >> 24U) & 0xFFU);
    return d;
}

// The result of a dimension mismatch query (§17). `first_differing_base` is the canonical base index (0=Length) of the
// first clashing exponent — so a 3z `Length+Time` diagnostic can name the base. Meaningful only when `!equal`.
struct DimMismatch
{
    bool equal               = false;
    u8   first_differing_base = 0;
};
[[nodiscard]] inline DimMismatch quantity_dims_equal(const QuantityDim& a, const QuantityDim& b) noexcept
{
    for (u8 i = 0; i < static_cast<u8>(kQuantityBases); ++i)
    {
        if (a.exp[i] != b.exp[i]) { return DimMismatch{false, i}; }
    }
    return DimMismatch{true, 0U};
}

// Dimension arithmetic (§17 — the seed CEIR-4's arith verifiers call). ⛔ Per-exponent overflow → FAILURE, never i8
// wraparound (the u32-wrap scar family): mul adds exponents, div subtracts.
struct DimArith
{
    bool        ok = false;
    QuantityDim dim;
};
[[nodiscard]] inline DimArith quantity_dim_combine(const QuantityDim& a, const QuantityDim& b, bool subtract) noexcept
{
    DimArith r;
    for (usize i = 0; i < kQuantityBases; ++i)
    {
        const i32 sum = subtract ? (static_cast<i32>(a.exp[i]) - static_cast<i32>(b.exp[i]))
                                 : (static_cast<i32>(a.exp[i]) + static_cast<i32>(b.exp[i]));
        if (sum > 127 || sum < -128) { return DimArith{false, {}}; } // i8 overflow
        r.dim.exp[i] = static_cast<i8>(sum);
    }
    r.ok = true;
    return r;
}
[[nodiscard]] inline DimArith quantity_dim_mul(const QuantityDim& a, const QuantityDim& b) noexcept { return quantity_dim_combine(a, b, false); }
[[nodiscard]] inline DimArith quantity_dim_div(const QuantityDim& a, const QuantityDim& b) noexcept { return quantity_dim_combine(a, b, true); }

// A buffer's flavor (§23 raw_buffer / buffer<T> / structured_buffer<T> / typed_buffer<T>). Stored in `Type::count`.
enum class BufferMode : u8
{
    Raw = 0, // no element type
    Plain,
    Structured,
    Typed,
};

// An image's dimensionality (§23 image<Dim,Format>). Stored in `Type::count`. Append at END.
enum class ImageDim : u8
{
    Dim1D = 0,
    Dim2D,
    Dim3D,
    Cube,
};

// The range dimensions a VIEW constrains (§23). A bit set stored in `Type::count`; the VALUES (offsets/sizes) are
// runtime. Append new bits at END (plane / tensor-slice / sparse-tile become bits later with zero record change).
enum class ViewRange : u8
{
    Byte    = 1U << 0U,
    Element = 1U << 1U,
    Mip     = 1U << 2U,
    Layer   = 1U << 3U,
    Aspect  = 1U << 4U,
};
inline constexpr u32 kViewRangeAll = 0x1FU; // the union of the five defined bits (the mask the decoder bounds against)
[[nodiscard]] constexpr u32 operator|(ViewRange a, ViewRange b) noexcept
{
    return static_cast<u32>(a) | static_cast<u32>(b);
}
[[nodiscard]] constexpr u32 operator|(u32 a, ViewRange b) noexcept { return a | static_cast<u32>(b); }

// Floating-point formats (§16). Append at END.
enum class FloatKind : u8
{
    F16 = 0,
    BF16,
    F32,
    F64,
    F8E4M3, // future FP8 (§16)
    F8E5M2,
};

// One interned type. A tag + fixed scalar/shape params + arena-owned spans of child TypeIds / member names. Stored BY
// VALUE in the Context intern table (only unique types are kept), keyed by STRUCTURAL equality (the name participates,
// so two same-shaped structs with different names are different types). The spans point into the Context arena once
// interned (the factory + `intern_type` copy them), so a `Type` handed back by `type_of` outlives the caller.
struct Type
{
    TypeKind    kind       = TypeKind::Bool;
    bool        is_signed  = false;             // Int
    FloatKind   fkind      = FloatKind::F32;    // Float
    u32         count      = 0;                 // Int: bit width; Vector/Array: element count; Matrix: rows
    u32         cols       = 0;                 // Matrix: columns
    TypeClassId type_class         = {};        // Extern ONLY: the dialect-defined type-class (0 otherwise) — CEIR-8a
    u32         type_class_version = 0;          // Extern ONLY: the class schema version (preserved for round-trip; 0 else)
    containers::ConstSpan<TypeId>              members; // child types (see the per-kind notes above)
    containers::StringView                     name;    // Struct / Enum type name (empty otherwise)
    containers::ConstSpan<containers::StringView> labels; // Struct field names / Enum case names (empty otherwise)

    [[nodiscard]] static Type scalar(TypeKind k) noexcept
    {
        Type t;
        t.kind = k;
        return t;
    }
};

[[nodiscard]] inline bool operator==(const Type& a, const Type& b) noexcept
{
    // ⛔ CEIR-8a landmine: `type_class` MUST participate — two DIFFERENT Extern classes with identical param slots are
    // DIFFERENT types; omitting it would intern them to one TypeId (silent type confusion). It is 0 for every built-in.
    if (a.kind != b.kind || a.is_signed != b.is_signed || a.fkind != b.fkind || a.count != b.count || a.cols != b.cols
        || a.type_class != b.type_class || a.type_class_version != b.type_class_version || a.name != b.name
        || a.members.size() != b.members.size() || a.labels.size() != b.labels.size())
    {
        return false;
    }
    for (usize i = 0; i < a.members.size(); ++i)
    {
        if (a.members[i] != b.members[i]) { return false; }
    }
    for (usize i = 0; i < a.labels.size(); ++i)
    {
        if (a.labels[i] != b.labels[i]) { return false; }
    }
    return true;
}

// A memory-safety WELL-FORMEDNESS invariant: the per-kind member/label arity + parity a consumer (printer, verifier,
// substitution) relies on when it indexes `members`/`labels` — e.g. a wrapper has exactly one member, a Callable's
// `count` (num params) must be within `members`. This is arity ONLY, never value semantics (`!i7` is well-formed here);
// the parser builds valid types by construction, but the binary DECODER must reject a hand-crafted record that would
// otherwise drive an out-of-bounds `members[i]` in a later consumer. A generic type param is STRUCTURALLY identified by
// (name, constraints): two generics both using `T:Ord` share one interned param and substitution binds every occurrence
// by TypeId — alpha-scoping (distinguishing unrelated `T`s across scopes) is a CHIR concern, not an IR-type one.
[[nodiscard]] inline bool type_is_well_formed(const Type& t) noexcept
{
    const usize m = t.members.size();
    const usize l = t.labels.size();
    switch (t.kind)
    {
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Index:
        return m == 0U && l == 0U;
    case TypeKind::Vector:
    case TypeKind::Matrix:
    case TypeKind::Complex:
    case TypeKind::Quaternion:
    case TypeKind::Array:
    case TypeKind::Option:
        return m == 1U && l == 0U;
    case TypeKind::Result:
        return m == 2U && l == 0U;
    case TypeKind::Tuple:
    case TypeKind::Variant:
        return l == 0U; // any number of positional members
    case TypeKind::Struct:
        return l == m; // one field name per field type
    case TypeKind::Enum:
        return m == 0U; // labels = case names, any count
    case TypeKind::TypeParam: // members = constraint traits (any count)
    case TypeKind::Trait:     // members = supertraits (any count)
        return l == 0U;
    case TypeKind::Callable:
        return l == 0U && static_cast<usize>(t.count) <= m; // count params, then results, within members
    case TypeKind::Buffer: // raw buffer (mode 0) has no element; every other mode has exactly one
        return l == 0U && m == (t.count == static_cast<u32>(BufferMode::Raw) ? 0U : 1U);
    case TypeKind::Sampler:
    case TypeKind::AccelStruct:
    case TypeKind::VideoFrame:
    case TypeKind::AudioBuffer:
    case TypeKind::ExternalResource:
        return m == 0U && l == 0U; // niladic resource kinds
    case TypeKind::Image:         // format element
    case TypeKind::ResourceTable: // table element
    case TypeKind::View:          // underlying resource
    case TypeKind::Quantity:      // underlying numeric type
    case TypeKind::Qualified:     // the qualified type
        return m == 1U && l == 0U;
    case TypeKind::Dim: // a dimension carries no member types (its data is cols/count/name)
        return m == 0U && l == 0U;
    case TypeKind::Shape: // members = dims, any rank (incl. 0)
        return l == 0U;
    case TypeKind::Tensor:
    case TypeKind::SparseTensor: // members = [element, shape]
        return m == 2U && l == 0U;
    case TypeKind::Extern: // CEIR-8a: a dialect-defined type-class owns its own arity (its registered verify hook) —
        return true;       // this Context-free check accepts structurally; the class hook is the real arity layer.
    }
    return false;
}

// CANONICAL form: every field a kind DOES NOT use is at its `Type::scalar` default, so two records that print
// identically ARE identical. The parser + factories build canonical types by construction; the DECODER must reject a
// hand-crafted record with junk in an ignored field (e.g. a `name` on an Int, an extent on a Dynamic Dim) — such a type
// prints lossily (the printer skips the ignored field), silently breaking the 1f "text and binary forms agree"
// contract. ⛔ Default `fkind` is F32 (=2), NOT 0. Subsumes well-formedness (arity/parity) first.
[[nodiscard]] inline bool type_is_canonical(const Type& t) noexcept
{
    if (!type_is_well_formed(t)) { return false; }
    // ⛔ CEIR-8a: the type-class fields are Extern-ONLY — junk on any other kind prints identically to the real type but
    // interns as a DISTINCT id (equality includes type_class), the exact "prints-but-differs" hazard this check guards.
    if (t.kind != TypeKind::Extern && (t.type_class.valid() || t.type_class_version != 0U)) { return false; }
    const bool scalars_default = !t.is_signed && t.fkind == FloatKind::F32 && t.count == 0U && t.cols == 0U;
    switch (t.kind)
    {
    case TypeKind::Bool:
    case TypeKind::Index:
    case TypeKind::Complex:
    case TypeKind::Quaternion:
    case TypeKind::Option:
    case TypeKind::Result:
    case TypeKind::Tuple:
    case TypeKind::Variant:
    case TypeKind::ResourceTable:
    case TypeKind::AccelStruct:
    case TypeKind::VideoFrame:
    case TypeKind::AudioBuffer:
    case TypeKind::ExternalResource:
    case TypeKind::Shape:
    case TypeKind::Tensor:
    case TypeKind::SparseTensor:
        return scalars_default && t.name.empty(); // no scalar field, no name
    case TypeKind::Int:                            // uses is_signed + count
        return t.fkind == FloatKind::F32 && t.cols == 0U && t.name.empty();
    case TypeKind::Float: // uses fkind
        return !t.is_signed && t.count == 0U && t.cols == 0U && t.name.empty();
    case TypeKind::Vector:
    case TypeKind::Array:
    case TypeKind::Callable:
    case TypeKind::Buffer:
    case TypeKind::Image:
    case TypeKind::View:      // uses count
    case TypeKind::Qualified: // uses count (the OwnershipKind)
        return !t.is_signed && t.fkind == FloatKind::F32 && t.cols == 0U && t.name.empty();
    case TypeKind::Matrix:   // uses count + cols
    case TypeKind::Quantity: // uses count + cols (the packed dimension — any value is a valid dimension, incl. all-zero
                             // = a dimensionless Quantity, a DISTINCT type from its raw underlying, intentionally)
        return !t.is_signed && t.fkind == FloatKind::F32 && t.name.empty();
    case TypeKind::Sampler: // uses is_signed
        return t.fkind == FloatKind::F32 && t.count == 0U && t.cols == 0U && t.name.empty();
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::TypeParam:
    case TypeKind::Trait: // uses name
        return scalars_default;
    case TypeKind::Dim: // cols = DimKind; Static uses count, Symbolic uses name (and must NOT be the reserved "dyn")
    {
        if (t.is_signed || t.fkind != FloatKind::F32) { return false; }
        switch (static_cast<DimKind>(t.cols))
        {
        case DimKind::Static: return t.name.empty();
        case DimKind::Symbolic: return t.count == 0U && !t.name.empty() && t.name != containers::StringView("dyn");
        case DimKind::Dynamic: return t.count == 0U && t.name.empty();
        }
        return false; // an out-of-range DimKind
    }
    case TypeKind::Extern: // CEIR-8a: an Extern MUST name a class (class 0 is nonsense — the factory could be handed {});
        return t.type_class.valid(); // the registered hook owns the SLOT canonicality (we cannot know which it uses).
    }
    return false;
}

// One binding of a generic type PARAMETER to a concrete type, consumed by `Context::substitute`.
struct TypeBinding
{
    TypeId param;
    TypeId concrete;
};

// The result of `Context::substitute`. On success, `type` is the substituted type (still generic if some params were
// left unbound). On a CONSTRAINT VIOLATION, `ok` is false and `failed_param` could not be bound to its concrete because
// that concrete does not satisfy the required `failed_trait` — a pointing diagnostic (the 3z gate's "constraint
// violation"), not a bare bool. On a COMPOSITION VIOLATION, `ok` is false and `failed_compose` is the rebuilt composite
// whose substituted member is no longer a legal underlying (e.g. binding T -> qty makes qty<qty<...>>) — substitution is
// a fourth construction path and must honor the same composition rules the parser/decoder/factory enforce.
struct SubstResult
{
    bool   ok = false;
    TypeId type;
    TypeId failed_param;
    TypeId failed_trait;
    TypeId failed_compose;
};

// The result of a broadcast query (§21). `position` is the RIGHT-ALIGNED dim index of the first incompatible pair
// (0 = the innermost dim) — meaningful only when `compat == Incompatible`, so a 3z diagnostic can point at the dim.
struct BroadcastResult
{
    ShapeCompat compat   = ShapeCompat::Unknown;
    u32         position = 0;
};
} // namespace crd::ceir
