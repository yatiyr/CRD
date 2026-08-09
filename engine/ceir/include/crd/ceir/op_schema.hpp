#pragma once

// crd-ceir - the REFLECTION record types the CEIR-2 generator populates (CEIR-2c, section 8 / 122 / 161). One
// `OpSchema` per op = the op's schema as queryable C++ data, the SAME truth the generated wrapper/builder/verifier are
// emitted from (no parallel truth). The CR-D007 node editor DERIVES node presentation from it (input pins == operands,
// output pins == results, widgets == attributes; section 161), and the committed `<dialect>.ops.json` mirrors it for
// CLI/MCP/agent discovery (section 122). Hand-written + shared: `<dialect>_op_schemas()` in each generated header
// returns a `ConstSpan<OpSchema>` over generated static data of these types.

#include <crd/ceir/attr.hpp>    // AttrKind
#include <crd/ceir/effect.hpp> // EffectRecord
#include <crd/ceir/id.hpp>
#include <crd/ceir/semantics.hpp> // DeterminismClass
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
// A positional operand (SSA input). `variadic` is set only on the last operand of a variadic op.
struct OperandInfo
{
    containers::StringView name;
    containers::StringView doc;
    bool                   variadic;
};

// A positional result (SSA output).
struct ResultInfo
{
    containers::StringView name;
    containers::StringView doc;
};

// A compile-time attribute (the tagged AttrKind + whether the verifier requires it).
struct AttrInfo
{
    containers::StringView name;
    AttrKind               kind;
    bool                   required;
    containers::StringView doc;
};

// One operation's reflected schema. `qualified` is "<dialect>.<name>". `effects` carries the TYPED §26 effect records
// (CEIR-4a — the same data `Context::op_effects` returns; the "strings until CEIR-4" scaffold is now retired). `domain`
// is the typed §15 EvalDomain (CEIR-4c; the scaffold retired). `native_*` is populated iff the op declares an [op.native]
// binding (ADR-0110 §2.1).
struct OpSchema
{
    containers::StringView                        name;      // "const"
    containers::StringView                        qualified; // "arith.const"
    containers::StringView                        dialect;   // "arith"
    u32                                           version;
    containers::StringView                        summary;
    containers::StringView                        docs;
    containers::ConstSpan<OperandInfo>            operands;
    containers::ConstSpan<ResultInfo>             results;
    containers::ConstSpan<AttrInfo>               attributes;
    u32                                           traits;      // OpTrait flags (dialect.hpp)
    u32                                           num_regions;
    containers::ConstSpan<EffectRecord>           effects;     // typed §26 effects (CEIR-4a)
    DeterminismClass                              determinism; // §27 op-level class (CEIR-4b); Unspecified ⇒ no claim
    EvalDomain                                    domain;      // §15 op-kind domain affinity (CEIR-4c); Unspecified ⇒ none
    bool                                          intrinsic;   // true iff [op.native] present
    containers::StringView                        native_provider;    // "" unless intrinsic
    DeterminismClass                              native_determinism; // §27 provider claim (CEIR-4b); Unspecified unless intrinsic
};
} // namespace crd::ceir
