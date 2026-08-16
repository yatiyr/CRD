#pragma once

// ckir_asset.hpp — CEIR-18q (D-007 §BAND-18): the `.ckir` NODE-GRAPH asset — the AUTHORABLE, NODE-EDITOR-READABLE form
// of a CKIR program. `ckir_write` renders a (KGraph, KEntry) as a TOML node graph; `ckir_read` loads it back into a
// byte-identical graph. This is the human/editor form that sits BESIDE `ckir_serialize`'s KGPH cook blob (ADR-0101 "the
// IR is the source of truth for all shaders"; user mandate: EVERYTHING — CEIR and CKIR — authorable + asset-driven, a
// node editor reads this and turns it into CKIR). A committed `.ckir` replaces a hand-built `ensure_*`/`build_*` builder:
// write once, commit, load disk-first, DELETE the builder. Identity by hash: `serialize_graph(ckir_read(ckir_write(g)))
// == serialize_graph(g)`.
//
// FORM — valid TOML, so ANY toml parser (a node editor) reads it. Value NODES are `[[node]]` tables with an `id`, an `op`
// NAME, `in=[…]` operand edges (node ids), and every OTHER field emitted ONLY when it differs from the KNode default
// (default-elision is what makes it readable, not an assembly dump). STATEMENTS (control flow / stores) are a `[[stmt]]`
// list mirroring KStmt 1:1. The entry (stage/outputs/kernel-body) is a labeled `[entry]`-style header. `cval` is an exact
// 0x-hex bit pattern (NaN/-0 survive). ⛔ kir stays toml++-free: `ckir_write` emits TOML by hand, `ckir_read` parses this
// canonical subset with the `Tok` cursor (whitespace/`#`-comment/key-order tolerant). Node ids are canonical `"n<index>"`
// on write but ANY string on read (an editor rename maps id→index); SSA order is preserved (forward refs are an error).

#include <crd/kir/ckir.hpp>

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdio>  // snprintf
#include <cstring> // memcpy

namespace crd::kir
{
namespace asset_detail
{
// ── ENUM NAME TABLES (indexed by the enum's u8 value; a bijection test in the gate proves no typo/dup/gap). ──────────
// ⛔ Order MUST match the enum declaration in ckir.hpp. A new enumerator appended at END there trips the static_assert.
inline constexpr const char* kKOpNames[] = {
    "Input", "Const", "Iota",
    "Neg", "Recip", "Abs", "Exp", "Log", "Sin", "Cos", "Sqrt", "Tanh", "Floor", "Ceil", "Sign", "Trunc", "Round",
    "Add", "Sub", "Mul", "Div", "Max", "Min", "CmpLt", "CmpEq", "CmpLe",
    "Shl", "Shr", "BitAnd", "BitOr", "BitXor",
    "CmpGt", "CmpGe", "CmpNe",
    "BitNot", "BitCount", "FindLSB", "FindMSB", "BitfieldExtract",
    "Pow", "Step", "Fract", "Clamp", "Mix",
    "Rsqrt", "Exp2", "Log2", "Tan", "Radians", "Degrees", "Atan2", "Smoothstep",
    "Asin", "Acos", "Atan", "Sinh", "Cosh", "Cbrt", "Mod", "Fma",
    "Vec2", "Vec3", "VecComp", "Dot", "Cross", "Normalize", "VecLen",
    "VecConcat", "Swizzle",
    "MatVecMul", "MatMatMul", "MatTranspose",
    "MatFromCols",
    "Splat", "Reflect", "Refract", "Faceforward", "VecAny", "VecAll",
    "OuterProduct", "Determinant", "MatInverse",
    "Slerp", "QuatMul", "QuatConj", "QuatRotate", "QuatAxisAngle", "QuatToMat3",
    "For", "LoopIndex", "LoopAcc",
    "BitReverse", "Ldexp", "FloatBitsToInt", "IntBitsToFloat", "Modf",
    "StructMake", "ArrayMake", "FieldGet", "ArrayGet",
    "StageIn", "Builtin", "UniformBlock",
    "Select",
    "ReduceSum", "ReduceMax", "ReduceMin", "ReduceProd", "ArgMax", "ArgMin",
    "ScanSum",
    "Reshape", "Permute", "Broadcast",
    "Contract",
    "Gather",
    "Scatter",
    "ScatterAdd",
    "Cast",
    "DFdx", "DFdy", "Fwidth",
    "StorageLoad",
    "Texture", "Sampler", "TexSample",
    "SampleLod", "SampleGrad", "SampleCmp", "TexelFetch", "TexGather", "TexSize",
    "SampleIndexed",
    "BufferDecl",
    "SharedDecl",
    "BufferLoad",
    "SharedLoad",
    "KernelLoopVar",
    "SubgroupBallot",
    "SubgroupBallotExclCount",
    "SubgroupMatch",
    "SampleIndexedLod",
    "AtomicResult",
    "AccelStructDecl",
    "RayHitResult",
    "RayPayloadDecl",
    "PayloadLoad",
    "SubgroupAdd", "SubgroupMin", "SubgroupMax", "SubgroupAnd", "SubgroupOr", "SubgroupXor",
    "SubgroupInclusiveAdd", "SubgroupExclusiveAdd", "SubgroupBroadcastFirst", "SubgroupShuffle",
    "QuadBroadcast", "QuadSwapX", "QuadSwapY", "QuadSwapDiagonal",
    "Call",
    "Attention",
    "CallableDataDecl",
};
inline constexpr int kKOpCount = static_cast<int>(sizeof(kKOpNames) / sizeof(kKOpNames[0]));
static_assert(kKOpCount == static_cast<int>(KOp::CallableDataDecl) + 1, "kKOpNames out of sync with enum class KOp");

inline constexpr const char* kKStmtNames[] = {
    "BufferStore", "SharedStore", "Barrier", "For", "If", "Materialize", "SpinUntilNonzero", "SharedAtomicAdd",
    "BufferAtomicAdd", "ForBreakIf", "BufferTicket", "SyncWarp", "BufferAtomicMin", "BufferAtomicAddFetch",
    "BufferAtomicExchange", "TraceRayClosest", "TraceRayHit", "TraceRayCurves", "TraceRayPipeline", "PayloadStore",
    "ReorderThread", "IgnoreHitIf", "ReportHit", "ExecuteCallable",
};
inline constexpr int kKStmtCount = static_cast<int>(sizeof(kKStmtNames) / sizeof(kKStmtNames[0]));
static_assert(kKStmtCount == static_cast<int>(KStmtKind::ExecuteCallable) + 1, "kKStmtNames out of sync with KStmtKind");

inline constexpr const char* kStageNames[] = {
    "Compute", "Vertex", "TessControl", "TessEval", "Geometry", "Fragment", "Task", "Mesh",
    "RayGen", "Intersection", "AnyHit", "ClosestHit", "Miss", "Callable",
};
static_assert(static_cast<int>(sizeof(kStageNames) / sizeof(kStageNames[0])) == kStageCount, "kStageNames vs KStage");

inline constexpr const char* kDTypeNames[] = {"F32", "F64", "F16", "BF16", "I32", "I64", "U8", "Bool", "U32"};
static_assert(static_cast<int>(sizeof(kDTypeNames) / sizeof(kDTypeNames[0])) == static_cast<int>(DType::U32) + 1, "kDTypeNames vs DType");

inline constexpr const char* kTKindNames[] = {"Scalar", "Vec", "Mat", "Struct", "Texture", "Sampler"};
static_assert(static_cast<int>(sizeof(kTKindNames) / sizeof(kTKindNames[0])) == static_cast<int>(TKind::Sampler) + 1, "kTKindNames vs TKind");

inline constexpr const char* kInterpNames[] = {"Smooth", "Flat", "NoPerspective", "Centroid", "Sample"};
static_assert(static_cast<int>(sizeof(kInterpNames) / sizeof(kInterpNames[0])) == static_cast<int>(Interp::Sample) + 1, "kInterpNames vs Interp");

inline constexpr const char* kDepthNames[] = {"Any", "Greater", "Less"};
static_assert(static_cast<int>(sizeof(kDepthNames) / sizeof(kDepthNames[0])) == static_cast<int>(DepthMode::Less) + 1, "kDepthNames vs DepthMode");

inline constexpr const char* kScopeNames[] = {"Workgroup", "Buffer"};
static_assert(static_cast<int>(sizeof(kScopeNames) / sizeof(kScopeNames[0])) == static_cast<int>(BarrierScope::Buffer) + 1, "kScopeNames vs BarrierScope");

inline constexpr const char* kTierNames[] = {"Exact", "Fast"};
static_assert(static_cast<int>(sizeof(kTierNames) / sizeof(kTierNames[0])) == static_cast<int>(DetTier::Fast) + 1, "kTierNames vs DetTier");

// ── WRITE helpers ──────────────────────────────────────────────────────────────────────────────────────────────────
inline void w_str(crd::containers::String& s, const char* v) { s.append(v); }
inline void w_i64(crd::containers::String& s, crd::i64 v)
{
    char buf[24];
    (void)std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    s.append(static_cast<const char*>(buf));
}
inline void w_u64(crd::containers::String& s, crd::u64 v)
{
    char buf[24];
    (void)std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    s.append(static_cast<const char*>(buf));
}
// `key = <i64>\n`
inline void w_kv_i(crd::containers::String& s, const char* key, crd::i64 v) { w_str(s, key); w_str(s, " = "); w_i64(s, v); w_str(s, "\n"); }
inline void w_kv_u(crd::containers::String& s, const char* key, crd::u64 v) { w_str(s, key); w_str(s, " = "); w_u64(s, v); w_str(s, "\n"); }
inline void w_kv_b(crd::containers::String& s, const char* key, bool v) { w_str(s, key); w_str(s, v ? " = true\n" : " = false\n"); }
inline void w_kv_s(crd::containers::String& s, const char* key, const char* v) { w_str(s, key); w_str(s, " = \""); w_str(s, v); w_str(s, "\"\n"); }
inline void w_noderef(crd::containers::String& s, crd::i32 idx) { w_str(s, "\"n"); w_i64(s, idx); w_str(s, "\""); } // "n<idx>"
inline void w_kv_hex(crd::containers::String& s, const char* key, crd::f64 v)
{
    crd::u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    char buf[24];
    (void)std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(bits));
    w_str(s, key);
    w_str(s, " = \"");
    s.append(static_cast<const char*>(buf));
    w_str(s, "\"\n");
}

// ── READ cursor: a whitespace/comment-tolerant token stream with a latched error offset. ────────────────────────────
struct Tok
{
    crd::containers::StringView in;
    crd::usize                  pos = 0;
    bool                        ok  = true;
    crd::usize                  err = 0;
    const char*                 msg = "";

    static bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    void fail(const char* m) noexcept { if (ok) { ok = false; err = pos; msg = m; } }

    // skip whitespace AND `#` line comments.
    void skip() noexcept
    {
        for (;;)
        {
            while (pos < in.size() && is_ws(in[pos])) { ++pos; }
            if (pos < in.size() && in[pos] == '#') { while (pos < in.size() && in[pos] != '\n') { ++pos; } continue; }
            break;
        }
    }
    [[nodiscard]] bool eof() noexcept { skip(); return pos >= in.size(); }
    [[nodiscard]] char peek() noexcept { skip(); return pos < in.size() ? in[pos] : '\0'; }
    // a bareword token (identifier / keyword / op name): [A-Za-z0-9_].
    bool word(crd::usize& b, crd::usize& e) noexcept
    {
        skip();
        b = pos;
        while (pos < in.size())
        {
            const char c = in[pos];
            const bool w = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!w) { break; }
            ++pos;
        }
        e = pos;
        if (b == e) { fail("expected a word"); return false; }
        return true;
    }
    [[nodiscard]] bool word_is(crd::usize b, crd::usize e, const char* k) const noexcept
    {
        crd::usize n = 0;
        for (crd::usize i = b; i < e; ++i, ++n) { if (k[n] == '\0' || k[n] != in[i]) { return false; } }
        return k[n] == '\0';
    }
    // consume one literal char (skipping ws/comments first); fail if it is not `c`.
    void lit(char c) noexcept { if (peek() != c) { fail("expected a delimiter"); return; } ++pos; }
    [[nodiscard]] bool at(char c) noexcept { return peek() == c; }

    // a "double-bracket" header opener: returns the header word for `[[word]]`, else fails. Assumes the caller peeked `[`.
    bool header(crd::usize& b, crd::usize& e) noexcept
    {
        lit('['); lit('[');
        if (!word(b, e)) { return false; }
        lit(']'); lit(']');
        return ok;
    }

    [[nodiscard]] crd::i64 int_val() noexcept
    {
        skip();
        const crd::usize b   = pos;
        bool             neg = false;
        if (pos < in.size() && (in[pos] == '-' || in[pos] == '+')) { neg = in[pos] == '-'; ++pos; }
        crd::u64   v = 0;
        crd::usize d = pos;
        for (; pos < in.size() && in[pos] >= '0' && in[pos] <= '9'; ++pos) { v = v * 10U + static_cast<crd::u64>(in[pos] - '0'); }
        if (pos == d) { pos = b; fail("expected integer"); return 0; }
        return neg ? -static_cast<crd::i64>(v) : static_cast<crd::i64>(v);
    }
    [[nodiscard]] bool bool_val() noexcept
    {
        crd::usize b = 0; crd::usize e = 0;
        if (!word(b, e)) { return false; }
        if (word_is(b, e, "true")) { return true; }
        if (word_is(b, e, "false")) { return false; }
        pos = b; fail("expected true/false"); return false;
    }
    // a "quoted string" → returns [b,e) of the CONTENTS (no quotes).
    bool str_val(crd::usize& b, crd::usize& e) noexcept
    {
        if (peek() != '"') { fail("expected string"); return false; }
        ++pos;
        b = pos;
        while (pos < in.size() && in[pos] != '"') { ++pos; }
        e = pos;
        if (pos >= in.size()) { fail("unterminated string"); return false; }
        ++pos; // closing quote
        return true;
    }
    // a node ref "n<idx>" → the index (a quoted string starting with 'n').
    [[nodiscard]] crd::i32 noderef() noexcept
    {
        crd::usize b = 0; crd::usize e = 0;
        if (!str_val(b, e)) { return -1; }
        if (e - b < 2 || in[b] != 'n') { pos = b; fail("expected a node ref \"n<idx>\""); return -1; }
        crd::i64 v = 0;
        for (crd::usize i = b + 1; i < e; ++i)
        {
            if (in[i] < '0' || in[i] > '9') { pos = b; fail("bad node ref"); return -1; }
            v = v * 10 + (in[i] - '0');
        }
        return static_cast<crd::i32>(v);
    }
    [[nodiscard]] crd::f64 hex_f64() noexcept
    {
        crd::usize b = 0; crd::usize e = 0;
        if (!str_val(b, e)) { return 0.0; }
        crd::usize i = b;
        if (e - i >= 2 && in[i] == '0' && (in[i + 1] == 'x' || in[i + 1] == 'X')) { i += 2; }
        else { pos = b; fail("expected 0x hex"); return 0.0; }
        crd::u64 bits = 0;
        for (; i < e; ++i)
        {
            const char h = in[i];
            crd::u64   d = 0;
            if (h >= '0' && h <= '9') { d = static_cast<crd::u64>(h - '0'); }
            else if (h >= 'a' && h <= 'f') { d = static_cast<crd::u64>(h - 'a') + 10U; }
            else if (h >= 'A' && h <= 'F') { d = static_cast<crd::u64>(h - 'A') + 10U; }
            else { pos = b; fail("bad hex digit"); return 0.0; }
            bits = (bits << 4U) | d;
        }
        crd::f64 v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    // a string value looked up in a name table → the enum index.
    [[nodiscard]] int enum_val(const char* const* names, int count, const char* m) noexcept
    {
        crd::usize b = 0; crd::usize e = 0;
        if (!str_val(b, e)) { return 0; }
        for (int k = 0; k < count; ++k) { if (word_is(b, e, names[k])) { return k; } }
        pos = b; fail(m); return 0;
    }
};

} // namespace asset_detail

// ── WRITE — serialize (KGraph, KEntry) to the `.ckir` TOML node-graph form. ──────────────────────────────────────────
[[nodiscard]] inline crd::containers::String ckir_write(const KGraph& g, const KEntry& e, crd::memory::IAllocator* a)
{
    namespace td = asset_detail;
    crd::containers::String s(a);
    s.append("# .ckir - an authored CKIR program (node graph). A node editor reads this and turns it into CKIR.\n");
    s.append("schema = 1\n\n");

    // ── [[entry]] — the stage interface (labeled; the graph is the [[node]] section below). ──
    s.append("[[entry]]\n");
    td::w_kv_s(s, "stage", td::kStageNames[static_cast<int>(e.stage)]);
    td::w_kv_i(s, "inputs", g.n_inputs());
    td::w_kv_i(s, "position", e.position);
    td::w_kv_i(s, "frag_depth", e.frag_depth);
    td::w_kv_i(s, "discard_cond", e.discard_cond);
    td::w_kv_b(s, "early_fragment_tests", e.early_fragment_tests);
    td::w_kv_s(s, "depth_mode", td::kDepthNames[static_cast<int>(e.depth_mode)]);
    td::w_kv_i(s, "shading_rate", e.shading_rate);
    td::w_kv_i(s, "storage_write_index", e.storage_write_index);
    td::w_kv_i(s, "storage_write_value", e.storage_write_value);
    td::w_kv_b(s, "interlock", e.interlock);
    td::w_kv_i(s, "n_out", e.n_out);
    s.append("local_size = [");
    td::w_u64(s, e.local_size[0]); s.append(", "); td::w_u64(s, e.local_size[1]); s.append(", "); td::w_u64(s, e.local_size[2]);
    s.append("]\n");
    td::w_kv_i(s, "kernel_body_begin", e.kernel_body_begin);
    td::w_kv_i(s, "kernel_body_count", e.kernel_body_count);
    td::w_kv_u(s, "mesh_vertices", e.mesh_vertices);
    td::w_kv_u(s, "mesh_primitives", e.mesh_primitives);
    td::w_kv_i(s, "mesh_prim", e.mesh_prim);
    td::w_kv_i(s, "task_emit", e.task_emit);
    s.append("task_payload = [");
    for (int k = 0; k < KEntry::kMaxTaskPayload; ++k) { if (k) { s.append(", "); } td::w_i64(s, e.task_payload[k]); }
    s.append("]\n");
    td::w_kv_u(s, "n_task_payload", e.n_task_payload);
    td::w_kv_u(s, "tess_patch_size", e.tess_patch_size);
    td::w_kv_i(s, "tess_inner", e.tess_inner);
    td::w_kv_i(s, "tess_outer", e.tess_outer);
    td::w_kv_b(s, "mesh_payload_in", e.mesh_payload_in);
    td::w_kv_b(s, "storage_read_only", e.storage_read_only);
    // per stage-output (raster): [0, n_out) — node ref + location + interp.
    for (int k = 0; k < e.n_out && k < kMaxStageOutputs; ++k)
    {
        s.append("\n[[out]]\nnode = ");
        td::w_noderef(s, e.out[k].node);
        s.append("\n");
        td::w_kv_i(s, "location", e.out[k].location);
        td::w_kv_s(s, "interp", td::kInterpNames[static_cast<int>(e.out[k].interp)]);
    }

    // ── [[node]] — the value DAG. Only NON-DEFAULT fields print (that is what makes it readable, not an IR dump). ──
    const KType def_type; // the KType default {F32, Scalar, 1, 1, 1, -1, 0}
    const auto& nodes = g.serial_nodes();
    for (crd::usize i = 0; i < nodes.size(); ++i)
    {
        const KNode& n = nodes[i];
        s.append("\n[[node]]\nid = \"n");
        td::w_u64(s, i);
        s.append("\"\n");
        td::w_kv_s(s, "op", td::kKOpNames[static_cast<int>(n.op)]);
        // type — elide when default.
        if (n.type.scalar != def_type.scalar) { td::w_kv_s(s, "dtype", td::kDTypeNames[static_cast<int>(n.type.scalar)]); }
        if (n.type.kind != def_type.kind) { td::w_kv_s(s, "tkind", td::kTKindNames[static_cast<int>(n.type.kind)]); }
        if (n.type.rows != def_type.rows) { td::w_kv_u(s, "trows", n.type.rows); }
        if (n.type.cols != def_type.cols) { td::w_kv_u(s, "tcols", n.type.cols); }
        if (n.type.count != def_type.count) { td::w_kv_u(s, "tcount", n.type.count); }
        if (n.type.struct_id != def_type.struct_id) { td::w_kv_i(s, "tstruct", n.type.struct_id); }
        if (n.type.elem_comps != def_type.elem_comps) { td::w_kv_u(s, "telem", n.type.elem_comps); }
        // shape — elide when rank 0 (scalar).
        if (n.shape.rank != 0)
        {
            s.append("shape = [");
            for (int k = 0; k < n.shape.rank && k < kMaxRank; ++k) { if (k) { s.append(", "); } td::w_i64(s, n.shape.dims[k]); }
            s.append("]\n");
        }
        // operand edges — `in = [ …node refs… ]` up to the highest set operand; "" marks a -1 gap. Elide if none.
        const crd::i32 ops[4] = {n.a, n.b, n.c, n.d};
        int            hi     = -1;
        for (int k = 0; k < 4; ++k) { if (ops[k] >= 0) { hi = k; } }
        if (hi >= 0)
        {
            s.append("in = [");
            for (int k = 0; k <= hi; ++k)
            {
                if (k) { s.append(", "); }
                if (ops[k] >= 0) { td::w_noderef(s, ops[k]); } else { s.append("\"\""); }
            }
            s.append("]\n");
        }
        if (n.cval != 0.0) { td::w_kv_hex(s, "cval", n.cval); }
        if (n.iidx != 0) { td::w_kv_i(s, "iidx", n.iidx); }
        if (n.axes != 0U) { td::w_kv_u(s, "axes", n.axes); }
        bool any_perm = false;
        for (int k = 0; k < kMaxRank; ++k) { if (n.perm[k] != 0U) { any_perm = true; } }
        if (any_perm)
        {
            s.append("perm = [");
            for (int k = 0; k < kMaxRank; ++k) { if (k) { s.append(", "); } td::w_u64(s, n.perm[k]); }
            s.append("]\n");
        }
        if (n.tier != DetTier::Exact) { td::w_kv_s(s, "tier", td::kTierNames[static_cast<int>(n.tier)]); }
        if (n.ext != -1) { td::w_kv_i(s, "ext", n.ext); }
        if (n.n_ext != 0U) { td::w_kv_u(s, "n_ext", n.n_ext); }
        if (n.dset != 0U) { td::w_kv_u(s, "dset", n.dset); }
    }

    // ── ext / struct-field / struct-begin pools (aggregate/variadic operand support). ⛔ These are SECTIONS ([[ext]]/[[sbegin]]),
    // NOT bare top-level `key = [...]` lines: a preceding [[node]]/[[stmt]]'s key-loop terminates only at a `[` (a section
    // header), and `ext` is ALSO a node/stmt scalar key — so a bare `ext = [...]` here is greedily eaten as the LAST node's
    // `ext` key ⇒ "expected integer" on the `[`. First surfaced by CEIR-19c's inline-ray-query kernel (the FIRST non-empty
    // serial_ext pool authored through the text form — TraceRayHit's 9 global-ext operands). No committed asset had a pool. ──
    const auto& ext = g.serial_ext();
    if (ext.size() > 0)
    {
        s.append("\n[[ext]]\nvalues = [");
        for (crd::usize i = 0; i < ext.size(); ++i) { if (i) { s.append(", "); } td::w_i64(s, ext[i]); }
        s.append("]\n");
    }
    const auto& sbegin = g.serial_sbegin();
    if (sbegin.size() > 0)
    {
        s.append("\n[[sbegin]]\nvalues = [");
        for (crd::usize i = 0; i < sbegin.size(); ++i) { if (i) { s.append(", "); } td::w_u64(s, sbegin[i]); }
        s.append("]\n");
    }
    const auto& sfields = g.serial_sfields();
    for (crd::usize i = 0; i < sfields.size(); ++i)
    {
        const KType& t = sfields[i];
        s.append("\n[[sfield]]\n");
        td::w_kv_s(s, "dtype", td::kDTypeNames[static_cast<int>(t.scalar)]);
        td::w_kv_s(s, "tkind", td::kTKindNames[static_cast<int>(t.kind)]);
        td::w_kv_u(s, "trows", t.rows);
        td::w_kv_u(s, "tcols", t.cols);
        td::w_kv_u(s, "tcount", t.count);
        td::w_kv_i(s, "tstruct", t.struct_id);
        td::w_kv_u(s, "telem", t.elem_comps);
    }

    // ── [[stmt]] — the statement list (control flow / stores), 1:1 with the KStmt pool. ──
    const auto& stmts = g.serial_stmts();
    for (crd::usize i = 0; i < stmts.size(); ++i)
    {
        const KStmt& st = stmts[i];
        s.append("\n[[stmt]]\n");
        td::w_kv_s(s, "kind", td::kKStmtNames[static_cast<int>(st.kind)]);
        if (st.target != -1) { s.append("target = "); td::w_noderef(s, st.target); s.append("\n"); }
        if (st.index != -1) { s.append("index = "); td::w_noderef(s, st.index); s.append("\n"); }
        if (st.value != -1) { s.append("value = "); td::w_noderef(s, st.value); s.append("\n"); }
        if (st.scope != BarrierScope::Workgroup) { td::w_kv_s(s, "scope", td::kScopeNames[static_cast<int>(st.scope)]); }
        if (st.body_begin != -1 || st.body_count != 0)
        {
            s.append("body = ["); td::w_i64(s, st.body_begin); s.append(", "); td::w_i64(s, st.body_count); s.append("]\n");
        }
        if (st.result != -1) { s.append("result = "); td::w_noderef(s, st.result); s.append("\n"); }
        if (st.ext != -1) { td::w_kv_i(s, "ext", st.ext); }
        if (st.n_ext != 0U) { td::w_kv_u(s, "n_ext", st.n_ext); }
    }
    return s;
}

// ── READ — outcome mirrors crd::ceir::ParseResult (reported, never thrown). ──────────────────────────────────────────
struct CkirReadResult
{
    bool        ok           = false;
    crd::usize  error_offset = 0;
    const char* error        = "";
    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] inline CkirReadResult ckir_read(crd::containers::StringView text, KGraph& g, KEntry& e)
{
    namespace td = asset_detail;
    td::Tok t{text, 0, true, 0, ""};

    auto*                            al = g.serial_nodes().allocator();
    crd::containers::Array<KNode>    nodes(al);
    crd::containers::Array<crd::i32> ext(al);
    crd::containers::Array<KType>    sfields(al);
    crd::containers::Array<crd::u32> sbegin(al);
    crd::containers::Array<KStmt>    stmts(al);
    KEntry                           en;
    int                              nin = 0;

    // read a `[[out]]` into en.out[n_out_seen].
    int n_out_seen = 0;

    // read a KType's 7 fields from `key = value` lines until the next section/EOF.
    const auto read_type_block = [&](KType& ty) {
        while (t.ok && !t.eof() && !t.at('['))
        {
            crd::usize b = 0; crd::usize en2 = 0;
            if (!t.word(b, en2)) { break; }
            t.lit('=');
            if (t.word_is(b, en2, "dtype")) { ty.scalar = static_cast<DType>(t.enum_val(td::kDTypeNames, static_cast<int>(DType::U32) + 1, "bad dtype")); }
            else if (t.word_is(b, en2, "tkind")) { ty.kind = static_cast<TKind>(t.enum_val(td::kTKindNames, static_cast<int>(TKind::Sampler) + 1, "bad tkind")); }
            else if (t.word_is(b, en2, "trows")) { ty.rows = static_cast<crd::u8>(t.int_val()); }
            else if (t.word_is(b, en2, "tcols")) { ty.cols = static_cast<crd::u8>(t.int_val()); }
            else if (t.word_is(b, en2, "tcount")) { ty.count = static_cast<crd::u16>(t.int_val()); }
            else if (t.word_is(b, en2, "tstruct")) { ty.struct_id = static_cast<crd::i16>(t.int_val()); }
            else if (t.word_is(b, en2, "telem")) { ty.elem_comps = static_cast<crd::u16>(t.int_val()); }
            else { t.pos = b; t.fail("unknown sfield key"); }
        }
    };
    // read a `[ … ]` array of ints via a callback.
    const auto read_int_array = [&](auto&& push) {
        t.lit('[');
        if (!t.at(']')) { for (;;) { push(t.int_val()); if (t.at(',')) { t.lit(','); continue; } break; } }
        t.lit(']');
    };

    while (t.ok && !t.eof())
    {
        if (t.at('['))
        {
            crd::usize hb = 0; crd::usize he = 0;
            if (!t.header(hb, he)) { break; }
            if (t.word_is(hb, he, "node"))
            {
                KNode n;
                n.a = n.b = n.c = n.d = -1;
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "id")) { crd::usize sb = 0; crd::usize se = 0; (void)t.str_val(sb, se); } // canonical "n<index>" == pool order; ignored (order authoritative)
                    else if (t.word_is(b, en2, "op")) { n.op = static_cast<KOp>(t.enum_val(td::kKOpNames, td::kKOpCount, "bad op")); }
                    else if (t.word_is(b, en2, "dtype")) { n.type.scalar = static_cast<DType>(t.enum_val(td::kDTypeNames, static_cast<int>(DType::U32) + 1, "bad dtype")); }
                    else if (t.word_is(b, en2, "tkind")) { n.type.kind = static_cast<TKind>(t.enum_val(td::kTKindNames, static_cast<int>(TKind::Sampler) + 1, "bad tkind")); }
                    else if (t.word_is(b, en2, "trows")) { n.type.rows = static_cast<crd::u8>(t.int_val()); }
                    else if (t.word_is(b, en2, "tcols")) { n.type.cols = static_cast<crd::u8>(t.int_val()); }
                    else if (t.word_is(b, en2, "tcount")) { n.type.count = static_cast<crd::u16>(t.int_val()); }
                    else if (t.word_is(b, en2, "tstruct")) { n.type.struct_id = static_cast<crd::i16>(t.int_val()); }
                    else if (t.word_is(b, en2, "telem")) { n.type.elem_comps = static_cast<crd::u16>(t.int_val()); }
                    else if (t.word_is(b, en2, "shape")) { int r = 0; read_int_array([&](crd::i64 v) { if (r < kMaxRank) { n.shape.dims[r] = v; } ++r; }); n.shape.rank = r; }
                    else if (t.word_is(b, en2, "in"))
                    {
                        int k = 0;
                        t.lit('[');
                        if (!t.at(']'))
                        {
                            for (;;)
                            {
                                crd::i32 ref = -1;
                                // an operand is either a "n<idx>" ref or "" (a gap).
                                crd::usize pk = t.pos;
                                crd::usize sb = 0; crd::usize se = 0;
                                if (t.peek() == '"') { crd::usize save = t.pos; (void)t.str_val(sb, se); if (se == sb) { ref = -1; } else { t.pos = save; ref = t.noderef(); } }
                                else { t.fail("expected operand ref"); }
                                (void)pk;
                                if (k == 0) { n.a = ref; } else if (k == 1) { n.b = ref; } else if (k == 2) { n.c = ref; } else if (k == 3) { n.d = ref; }
                                ++k;
                                if (t.at(',')) { t.lit(','); continue; }
                                break;
                            }
                        }
                        t.lit(']');
                    }
                    else if (t.word_is(b, en2, "cval")) { n.cval = t.hex_f64(); }
                    else if (t.word_is(b, en2, "iidx")) { n.iidx = static_cast<crd::i32>(t.int_val()); }
                    else if (t.word_is(b, en2, "axes")) { n.axes = static_cast<crd::u32>(t.int_val()); }
                    else if (t.word_is(b, en2, "perm")) { int k = 0; read_int_array([&](crd::i64 v) { if (k < kMaxRank) { n.perm[k] = static_cast<crd::u8>(v); } ++k; }); }
                    else if (t.word_is(b, en2, "tier")) { n.tier = static_cast<DetTier>(t.enum_val(td::kTierNames, static_cast<int>(DetTier::Fast) + 1, "bad tier")); }
                    else if (t.word_is(b, en2, "ext")) { n.ext = static_cast<crd::i32>(t.int_val()); }
                    else if (t.word_is(b, en2, "n_ext")) { n.n_ext = static_cast<crd::u16>(t.int_val()); }
                    else if (t.word_is(b, en2, "dset")) { n.dset = static_cast<crd::u8>(t.int_val()); }
                    else { t.pos = b; t.fail("unknown node key"); }
                }
                if (t.ok) { nodes.push_back(n); }
            }
            else if (t.word_is(hb, he, "sfield")) { KType ty; read_type_block(ty); if (t.ok) { sfields.push_back(ty); } }
            else if (t.word_is(hb, he, "stmt"))
            {
                KStmt st;
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "kind")) { st.kind = static_cast<KStmtKind>(t.enum_val(td::kKStmtNames, td::kKStmtCount, "bad kind")); }
                    else if (t.word_is(b, en2, "target")) { st.target = t.noderef(); }
                    else if (t.word_is(b, en2, "index")) { st.index = t.noderef(); }
                    else if (t.word_is(b, en2, "value")) { st.value = t.noderef(); }
                    else if (t.word_is(b, en2, "scope")) { st.scope = static_cast<BarrierScope>(t.enum_val(td::kScopeNames, static_cast<int>(BarrierScope::Buffer) + 1, "bad scope")); }
                    else if (t.word_is(b, en2, "body")) { int k = 0; read_int_array([&](crd::i64 v) { if (k == 0) { st.body_begin = static_cast<crd::i32>(v); } else if (k == 1) { st.body_count = static_cast<crd::i32>(v); } ++k; }); }
                    else if (t.word_is(b, en2, "result")) { st.result = t.noderef(); }
                    else if (t.word_is(b, en2, "ext")) { st.ext = static_cast<crd::i32>(t.int_val()); }
                    else if (t.word_is(b, en2, "n_ext")) { st.n_ext = static_cast<crd::u16>(t.int_val()); }
                    else { t.pos = b; t.fail("unknown stmt key"); }
                }
                if (t.ok) { stmts.push_back(st); }
            }
            else if (t.word_is(hb, he, "entry"))
            {
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "stage")) { en.stage = static_cast<KStage>(t.enum_val(td::kStageNames, kStageCount, "bad stage")); }
                    else if (t.word_is(b, en2, "inputs")) { nin = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "position")) { en.position = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "frag_depth")) { en.frag_depth = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "discard_cond")) { en.discard_cond = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "early_fragment_tests")) { en.early_fragment_tests = t.bool_val(); }
                    else if (t.word_is(b, en2, "depth_mode")) { en.depth_mode = static_cast<DepthMode>(t.enum_val(td::kDepthNames, static_cast<int>(DepthMode::Less) + 1, "bad depth_mode")); }
                    else if (t.word_is(b, en2, "shading_rate")) { en.shading_rate = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "storage_write_index")) { en.storage_write_index = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "storage_write_value")) { en.storage_write_value = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "interlock")) { en.interlock = t.bool_val(); }
                    else if (t.word_is(b, en2, "n_out")) { en.n_out = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "local_size")) { int k = 0; read_int_array([&](crd::i64 v) { if (k < 3) { en.local_size[k] = static_cast<crd::u32>(v); } ++k; }); }
                    else if (t.word_is(b, en2, "kernel_body_begin")) { en.kernel_body_begin = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "kernel_body_count")) { en.kernel_body_count = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "mesh_vertices")) { en.mesh_vertices = static_cast<crd::u32>(t.int_val()); }
                    else if (t.word_is(b, en2, "mesh_primitives")) { en.mesh_primitives = static_cast<crd::u32>(t.int_val()); }
                    else if (t.word_is(b, en2, "mesh_prim")) { en.mesh_prim = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "task_emit")) { en.task_emit = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "task_payload")) { int k = 0; read_int_array([&](crd::i64 v) { if (k < KEntry::kMaxTaskPayload) { en.task_payload[k] = static_cast<int>(v); } ++k; }); }
                    else if (t.word_is(b, en2, "n_task_payload")) { en.n_task_payload = static_cast<crd::u32>(t.int_val()); }
                    else if (t.word_is(b, en2, "tess_patch_size")) { en.tess_patch_size = static_cast<crd::u32>(t.int_val()); }
                    else if (t.word_is(b, en2, "tess_inner")) { en.tess_inner = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "tess_outer")) { en.tess_outer = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "mesh_payload_in")) { en.mesh_payload_in = t.bool_val(); }
                    else if (t.word_is(b, en2, "storage_read_only")) { en.storage_read_only = t.bool_val(); }
                    else { t.pos = b; t.fail("unknown entry key"); }
                }
            }
            else if (t.word_is(hb, he, "out"))
            {
                KStageOutput o;
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "node")) { o.node = t.noderef(); }
                    else if (t.word_is(b, en2, "location")) { o.location = static_cast<int>(t.int_val()); }
                    else if (t.word_is(b, en2, "interp")) { o.interp = static_cast<Interp>(t.enum_val(td::kInterpNames, static_cast<int>(Interp::Sample) + 1, "bad interp")); }
                    else { t.pos = b; t.fail("unknown out key"); }
                }
                if (t.ok && n_out_seen < kMaxStageOutputs) { en.out[n_out_seen++] = o; }
            }
            else if (t.word_is(hb, he, "ext")) // CEIR-19c: the global ext-operand pool (a SECTION, not a bare key — see the writer note)
            {
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "values")) { read_int_array([&](crd::i64 v) { ext.push_back(static_cast<crd::i32>(v)); }); }
                    else { t.pos = b; t.fail("unknown ext key"); }
                }
            }
            else if (t.word_is(hb, he, "sbegin")) // CEIR-19c: the struct-begin pool (a SECTION, same collision fix as [[ext]])
            {
                while (t.ok && !t.eof() && !t.at('['))
                {
                    crd::usize b = 0; crd::usize en2 = 0;
                    if (!t.word(b, en2)) { break; }
                    t.lit('=');
                    if (t.word_is(b, en2, "values")) { read_int_array([&](crd::i64 v) { sbegin.push_back(static_cast<crd::u32>(v)); }); }
                    else { t.pos = b; t.fail("unknown sbegin key"); }
                }
            }
            else { t.fail("unknown [[section]]"); }
        }
        else
        {
            // a top-level `key = value` (only `schema` today — ext/sbegin are now [[ext]]/[[sbegin]] SECTIONS, see the writer note).
            crd::usize b = 0; crd::usize en2 = 0;
            if (!t.word(b, en2)) { break; }
            t.lit('=');
            if (t.word_is(b, en2, "schema")) { (void)t.int_val(); }
            else { t.pos = b; t.fail("unknown top-level key"); }
        }
    }

    if (!t.ok) { return CkirReadResult{false, t.err, t.msg}; }
    // ⛔ a real CKIR program has value nodes; empty / non-`.ckir` input (0 nodes) is REPORTED, never a silent empty graph.
    if (nodes.size() == 0) { return CkirReadResult{false, 0, "not a CKIR program (no nodes)"}; }

    e = en;
    g.serial_restore(nodes.data(), nodes.size(), ext.data(), ext.size(), sfields.data(), sfields.size(),
                     sbegin.data(), sbegin.size(), stmts.data(), stmts.size(), nin);
    return CkirReadResult{true, 0, ""};
}

} // namespace crd::kir
