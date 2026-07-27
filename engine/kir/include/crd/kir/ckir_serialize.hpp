#pragma once

// ckir_serialize.hpp — D1 (D-007): the IR-as-crdr foundation. Serialize a CKIR `KGraph` + `KEntry` to a versioned byte blob (the
// on-disk shader-graph resource that REPLACES stored GLSL/HLSL — ADR-0101 "the IR is the source of truth for all shaders"), and
// deserialize it back into a byte-identical graph, so re-emitting from the loaded graph produces BIT-IDENTICAL backend source. Plus
// `reflect()` — the descriptor-set layout, vertex-input layout, push-constant size + workgroup size derived straight from OUR IR
// (no SPIRV-Cross; own-format mandate). The cook (D2) turns this resource into per-backend bytecode; D3 adds variants; D4 loads it
// with zero runtime compile; D5 hot-reloads it.
//
// FORMAT (little-endian, ADR-0037): [FourCC 'KGPH'][version][record-width manifest: KNode·KStmt·KType·KEntry][n_inputs] then the
// five pools (nodes · ext · struct-fields · struct-begins · stmts), each [count][canonical records], then the KEntry record.
//
// ⛔ CANONICAL, PACKED, PADDING-FREE RECORDS — NEVER blast the POD pools raw. `serialize_graph` is the CONTENT-HASH SOURCE for
// the whole cook (the shader cache key, D3's variant-matrix dedup, D8's unique-bundle table), so the blob MUST be a pure
// function of the graph's content. SCAR (2026-07-25): it used to `memcpy` the pools, which put every INDETERMINATE PADDING byte
// of KNode/KStmt/KType/KEntry into the hash (the builders default-initialize — `KNode n;` — so padding holds whatever the stack
// held). Identical variant keys then hashed DIFFERENTLY whenever the allocation layout differed: win-asan's randomized layout
// broke D3/D5/D6/D8/D10/D12 dedup while win-debug's `/RTC1` 0xCC-fill masked it. The first differing byte was KNode+1 — the
// hole between `KOp op` and the 2-aligned `KType type`. Writing each field explicitly removes every hole by construction, and
// makes the artifact ABI-INDEPENDENT as a bonus (a blob cooked by MSVC now loads under gcc/clang instead of being rejected by a
// sizeof manifest). The gate is `tests/kir/test_ckir_serialize_determinism.cpp`.

#include <crd/kir/ckir.hpp>

#include <crd/containers/span.hpp> // ConstSpan (ckir.hpp brings Array but not span)

#include <cstring> // memcpy — the f64 bit-pattern punt
#include <type_traits>

namespace crd::kir
{

inline constexpr crd::u32 kShaderGraphFourCC =
    (static_cast<crd::u32>('K')) | (static_cast<crd::u32>('G') << 8U) | (static_cast<crd::u32>('P') << 16U) | (static_cast<crd::u32>('H') << 24U);
// v2 = the canonical padding-free records (v1 was the raw-POD blast; a v1 blob is cleanly rejected ⇒ recook).
inline constexpr crd::u32 kShaderGraphVersion = 3U; // v3: KEntry += mesh_payload_in (REN-38-F6+)

static_assert(std::is_trivially_copyable_v<KNode>, "KNode must be trivially copyable to pool-serialize");
static_assert(std::is_trivially_copyable_v<KStmt>, "KStmt must be trivially copyable");
static_assert(std::is_trivially_copyable_v<KType>, "KType must be trivially copyable");
static_assert(std::is_trivially_copyable_v<KEntry>, "KEntry must be trivially copyable");

namespace serial_detail
{
// The ON-DISK width of one record — the exact sum of its field widths, no padding. These are part of the format (they are
// written into the manifest), so a field added to a struct MUST bump both its record width and the encoder/decoder below.
inline constexpr crd::u32 kTypeRec  = 10U;  // scalar·kind·rows·cols(4) + count·struct_id·elem_comps(6)
inline constexpr crd::u32 kShapeRec = 68U;  // dims[8] as i64 (64) + rank as i32 (4)
inline constexpr crd::u32 kNodeRec  = 127U; // op(1) + type(10) + shape(68) + a·b·c·d(16) + cval(8) + iidx(4) + axes(4)
                                            //   + perm[8](8) + tier(1) + ext(4) + n_ext(2) + dset(1)
inline constexpr crd::u32 kStmtRec  = 32U;  // kind(1) + target·index·value(12) + scope(1) + begin·count·result·ext(16) + n_ext(2)
inline constexpr crd::u32 kOutRec   = 9U;   // node(4) + location(4) + interp(1)
inline constexpr crd::u32 kEntryRec = 173U; // see write_entry — the field-by-field sum

// The three array extents the record widths are derived from. If one changes, the widths above are stale.
static_assert(kMaxRank == 8, "kShapeRec/kNodeRec assume Shape::dims[8] + perm[8]");
static_assert(kMaxStageOutputs == 8, "kEntryRec assumes KEntry::out[8]");
static_assert(KEntry::kMaxTaskPayload == 4, "kEntryRec assumes KEntry::task_payload[4]");

using ByteArray = crd::containers::Array<crd::u8>;

inline void put_u8(ByteArray& b, crd::u8 v) { b.push_back(v); }
inline void put_u16(ByteArray& b, crd::u16 v)
{
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
    b.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
}
inline void put_u32(ByteArray& b, crd::u32 v)
{
    for (crd::u32 s = 0; s < 32U; s += 8U) { b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU)); }
}
inline void put_u64(ByteArray& b, crd::u64 v)
{
    for (crd::u32 s = 0; s < 64U; s += 8U) { b.push_back(static_cast<crd::u8>((v >> s) & 0xFFU)); }
}
inline void put_i16(ByteArray& b, crd::i16 v) { put_u16(b, static_cast<crd::u16>(v)); }
inline void put_i32(ByteArray& b, crd::i32 v) { put_u32(b, static_cast<crd::u32>(v)); }
inline void put_i64(ByteArray& b, crd::i64 v) { put_u64(b, static_cast<crd::u64>(v)); }
inline void put_bool(ByteArray& b, bool v) { put_u8(b, v ? 1U : 0U); }
// The f64 goes out as its BIT PATTERN (not a decimal round-trip) so NaN payloads / signed zero survive exactly.
inline void put_f64(ByteArray& b, crd::f64 v)
{
    crd::u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(b, bits);
}

// A bounds-checked read cursor. Any short read latches `ok = false` and yields zeroes, so the caller checks once at the end.
struct Cursor
{
    crd::containers::ConstSpan<crd::u8> in;
    crd::u64                            pos = 0;
    bool                                ok  = true;

    [[nodiscard]] bool have(crd::u64 n) noexcept
    {
        if (!ok || pos + n > in.size()) { ok = false; }
        return ok;
    }
    [[nodiscard]] crd::u8 u8v() noexcept { return have(1U) ? in[pos++] : static_cast<crd::u8>(0); }
    [[nodiscard]] crd::u16 u16v() noexcept
    {
        if (!have(2U)) { return 0U; }
        const auto v = static_cast<crd::u16>(static_cast<crd::u16>(in[pos]) | static_cast<crd::u16>(static_cast<crd::u16>(in[pos + 1U]) << 8U));
        pos += 2U;
        return v;
    }
    [[nodiscard]] crd::u32 u32v() noexcept
    {
        if (!have(4U)) { return 0U; }
        crd::u32 v = 0;
        for (crd::u32 i = 0; i < 4U; ++i) { v |= static_cast<crd::u32>(in[pos + i]) << (i * 8U); }
        pos += 4U;
        return v;
    }
    [[nodiscard]] crd::u64 u64v() noexcept
    {
        if (!have(8U)) { return 0U; }
        crd::u64 v = 0;
        for (crd::u32 i = 0; i < 8U; ++i) { v |= static_cast<crd::u64>(in[pos + i]) << (i * 8U); }
        pos += 8U;
        return v;
    }
    [[nodiscard]] crd::i16 i16v() noexcept { return static_cast<crd::i16>(u16v()); }
    [[nodiscard]] crd::i32 i32v() noexcept { return static_cast<crd::i32>(u32v()); }
    [[nodiscard]] crd::i64 i64v() noexcept { return static_cast<crd::i64>(u64v()); }
    [[nodiscard]] bool     boolv() noexcept { return u8v() != 0U; }
    [[nodiscard]] crd::f64 f64v() noexcept
    {
        const crd::u64 bits = u64v();
        crd::f64       v    = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
};

inline void write_type(ByteArray& b, const KType& t)
{
    put_u8(b, static_cast<crd::u8>(t.scalar));
    put_u8(b, static_cast<crd::u8>(t.kind));
    put_u8(b, t.rows);
    put_u8(b, t.cols);
    put_u16(b, t.count);
    put_i16(b, t.struct_id);
    put_u16(b, t.elem_comps);
}
inline KType read_type(Cursor& c)
{
    KType t;
    t.scalar     = static_cast<DType>(c.u8v());
    t.kind       = static_cast<TKind>(c.u8v());
    t.rows       = c.u8v();
    t.cols       = c.u8v();
    t.count      = c.u16v();
    t.struct_id  = c.i16v();
    t.elem_comps = c.u16v();
    return t;
}

inline void write_shape(ByteArray& b, const Shape& s)
{
    for (int i = 0; i < kMaxRank; ++i) { put_i64(b, s.dims[i]); }
    put_i32(b, s.rank);
}
inline Shape read_shape(Cursor& c)
{
    Shape s;
    for (int i = 0; i < kMaxRank; ++i) { s.dims[i] = c.i64v(); }
    s.rank = c.i32v();
    return s;
}

inline void write_node(ByteArray& b, const KNode& n)
{
    put_u8(b, static_cast<crd::u8>(n.op));
    write_type(b, n.type);
    write_shape(b, n.shape);
    put_i32(b, n.a);
    put_i32(b, n.b);
    put_i32(b, n.c);
    put_i32(b, n.d);
    put_f64(b, n.cval);
    put_i32(b, n.iidx);
    put_u32(b, n.axes);
    for (int i = 0; i < kMaxRank; ++i) { put_u8(b, n.perm[i]); }
    put_u8(b, static_cast<crd::u8>(n.tier));
    put_i32(b, n.ext);
    put_u16(b, n.n_ext);
    put_u8(b, n.dset);
}
inline KNode read_node(Cursor& c)
{
    KNode n;
    n.op    = static_cast<KOp>(c.u8v());
    n.type  = read_type(c);
    n.shape = read_shape(c);
    n.a     = c.i32v();
    n.b     = c.i32v();
    n.c     = c.i32v();
    n.d     = c.i32v();
    n.cval  = c.f64v();
    n.iidx  = c.i32v();
    n.axes  = c.u32v();
    for (int i = 0; i < kMaxRank; ++i) { n.perm[i] = c.u8v(); }
    n.tier  = static_cast<DetTier>(c.u8v());
    n.ext   = c.i32v();
    n.n_ext = c.u16v();
    n.dset  = c.u8v();
    return n;
}

inline void write_stmt(ByteArray& b, const KStmt& s)
{
    put_u8(b, static_cast<crd::u8>(s.kind));
    put_i32(b, s.target);
    put_i32(b, s.index);
    put_i32(b, s.value);
    put_u8(b, static_cast<crd::u8>(s.scope));
    put_i32(b, s.body_begin);
    put_i32(b, s.body_count);
    put_i32(b, s.result);
    put_i32(b, s.ext);
    put_u16(b, s.n_ext);
}
inline KStmt read_stmt(Cursor& c)
{
    KStmt s;
    s.kind       = static_cast<KStmtKind>(c.u8v());
    s.target     = c.i32v();
    s.index      = c.i32v();
    s.value      = c.i32v();
    s.scope      = static_cast<BarrierScope>(c.u8v());
    s.body_begin = c.i32v();
    s.body_count = c.i32v();
    s.result     = c.i32v();
    s.ext        = c.i32v();
    s.n_ext      = c.u16v();
    return s;
}

inline void write_entry(ByteArray& b, const KEntry& e)
{
    put_u8(b, static_cast<crd::u8>(e.stage));                     // 1
    put_i32(b, e.position);                                       // 4
    put_i32(b, e.frag_depth);                                     // 4
    put_i32(b, e.discard_cond);                                   // 4
    put_bool(b, e.early_fragment_tests);                          // 1
    put_u8(b, static_cast<crd::u8>(e.depth_mode));                // 1
    put_i32(b, e.shading_rate);                                   // 4
    put_i32(b, e.storage_write_index);                            // 4
    put_i32(b, e.storage_write_value);                            // 4
    put_bool(b, e.interlock);                                     // 1
    put_i32(b, e.n_out);                                          // 4
    static_assert(kOutRec == 9U, "one KStageOutput record is node(4) + location(4) + interp(1)");
    for (int i = 0; i < kMaxStageOutputs; ++i)                    // 8 * kOutRec = 72
    {
        put_i32(b, e.out[i].node);
        put_i32(b, e.out[i].location);
        put_u8(b, static_cast<crd::u8>(e.out[i].interp));
    }
    for (int i = 0; i < 3; ++i) { put_u32(b, e.local_size[i]); }  // 12
    put_i32(b, e.kernel_body_begin);                              // 4
    put_i32(b, e.kernel_body_count);                              // 4
    put_u32(b, e.mesh_vertices);                                  // 4
    put_u32(b, e.mesh_primitives);                                // 4
    put_i32(b, e.mesh_prim);                                      // 4
    put_i32(b, e.task_emit);                                      // 4
    for (int i = 0; i < KEntry::kMaxTaskPayload; ++i) { put_i32(b, e.task_payload[i]); } // 16
    put_u32(b, e.n_task_payload);                                 // 4
    put_u32(b, e.tess_patch_size);                                // 4
    put_i32(b, e.tess_inner);                                     // 4
    put_i32(b, e.tess_outer);                                     // 4
    put_bool(b, e.mesh_payload_in);                               // 1  => 173 (v3, REN-38-F6+)
}
inline KEntry read_entry(Cursor& c)
{
    KEntry e;
    e.stage                = static_cast<KStage>(c.u8v());
    e.position             = c.i32v();
    e.frag_depth           = c.i32v();
    e.discard_cond         = c.i32v();
    e.early_fragment_tests = c.boolv();
    e.depth_mode           = static_cast<DepthMode>(c.u8v());
    e.shading_rate         = c.i32v();
    e.storage_write_index  = c.i32v();
    e.storage_write_value  = c.i32v();
    e.interlock            = c.boolv();
    e.n_out                = c.i32v();
    for (int i = 0; i < kMaxStageOutputs; ++i)
    {
        e.out[i].node     = c.i32v();
        e.out[i].location = c.i32v();
        e.out[i].interp   = static_cast<Interp>(c.u8v());
    }
    for (int i = 0; i < 3; ++i) { e.local_size[i] = c.u32v(); }
    e.kernel_body_begin = c.i32v();
    e.kernel_body_count = c.i32v();
    e.mesh_vertices     = c.u32v();
    e.mesh_primitives   = c.u32v();
    e.mesh_prim         = c.i32v();
    e.task_emit         = c.i32v();
    for (int i = 0; i < KEntry::kMaxTaskPayload; ++i) { e.task_payload[i] = c.i32v(); }
    e.n_task_payload = c.u32v();
    e.tess_patch_size = c.u32v();
    e.tess_inner      = c.i32v();
    e.tess_outer      = c.i32v();
    e.mesh_payload_in = c.boolv(); // v3 (REN-38-F6+)
    return e;
}
} // namespace serial_detail

// Serialize (KGraph, KEntry) → a versioned byte blob (allocated from `a`). Byte-identical for structurally identical graphs.
[[nodiscard]] inline crd::containers::Array<crd::u8> serialize_graph(const KGraph& g, const KEntry& e, crd::memory::IAllocator* a)
{
    namespace sd = serial_detail;
    crd::containers::Array<crd::u8> out(a);
    out.reserve(64U + (g.serial_nodes().size() * sd::kNodeRec) + (g.serial_stmts().size() * sd::kStmtRec)
                + (g.serial_ext().size() * 4U) + (g.serial_sfields().size() * sd::kTypeRec) + (g.serial_sbegin().size() * 4U)
                + sd::kEntryRec);
    sd::put_u32(out, kShaderGraphFourCC);
    sd::put_u32(out, kShaderGraphVersion);
    sd::put_u32(out, sd::kNodeRec);
    sd::put_u32(out, sd::kStmtRec);
    sd::put_u32(out, sd::kTypeRec);
    sd::put_u32(out, sd::kEntryRec);
    sd::put_u32(out, static_cast<crd::u32>(g.n_inputs()));

    const auto& nodes = g.serial_nodes();
    sd::put_u32(out, static_cast<crd::u32>(nodes.size()));
    for (crd::usize i = 0; i < nodes.size(); ++i) { sd::write_node(out, nodes[i]); }
    const auto& ext = g.serial_ext();
    sd::put_u32(out, static_cast<crd::u32>(ext.size()));
    for (crd::usize i = 0; i < ext.size(); ++i) { sd::put_i32(out, ext[i]); }
    const auto& sfields = g.serial_sfields();
    sd::put_u32(out, static_cast<crd::u32>(sfields.size()));
    for (crd::usize i = 0; i < sfields.size(); ++i) { sd::write_type(out, sfields[i]); }
    const auto& sbegin = g.serial_sbegin();
    sd::put_u32(out, static_cast<crd::u32>(sbegin.size()));
    for (crd::usize i = 0; i < sbegin.size(); ++i) { sd::put_u32(out, sbegin[i]); }
    const auto& stmts = g.serial_stmts();
    sd::put_u32(out, static_cast<crd::u32>(stmts.size()));
    for (crd::usize i = 0; i < stmts.size(); ++i) { sd::write_stmt(out, stmts[i]); }

    sd::write_entry(out, e);
    return out;
}

// Deserialize a blob into a FRESH `g` (uses g's allocator for the pools) + `e`. Returns false on a bad magic / version /
// record-width manifest (an encoder drift ⇒ clean reject, recook) / truncation.
[[nodiscard]] inline bool deserialize_graph(crd::containers::ConstSpan<crd::u8> in, KGraph& g, KEntry& e)
{
    namespace sd = serial_detail;
    sd::Cursor c{in, 0, true};
    if (c.u32v() != kShaderGraphFourCC || !c.ok) { return false; }
    if (c.u32v() != kShaderGraphVersion) { return false; }
    if (c.u32v() != sd::kNodeRec) { return false; }
    if (c.u32v() != sd::kStmtRec) { return false; }
    if (c.u32v() != sd::kTypeRec) { return false; }
    if (c.u32v() != sd::kEntryRec) { return false; }
    const crd::u32 nin = c.u32v();
    if (!c.ok) { return false; }

    auto*                            al = g.serial_nodes().allocator();
    crd::containers::Array<KNode>    nodes(al);
    crd::containers::Array<crd::i32> ext(al);
    crd::containers::Array<KType>    sfields(al);
    crd::containers::Array<crd::u32> sbegin(al);
    crd::containers::Array<KStmt>    stmts(al);

    // Each pool is [count][records]. The per-record width is fixed, so bounds-check the whole run BEFORE reserving —
    // a truncated/corrupt blob must not drive a huge allocation.
    const auto pool_count = [&](crd::u32 rec) -> crd::u32 {
        const crd::u32 n = c.u32v();
        if (!c.ok || (static_cast<crd::u64>(n) * rec) > (in.size() - c.pos)) { c.ok = false; return 0U; }
        return n;
    };
    const crd::u32 n_nodes = pool_count(sd::kNodeRec);
    if (!c.ok) { return false; }
    nodes.reserve(n_nodes);
    for (crd::u32 i = 0; i < n_nodes; ++i) { nodes.push_back(sd::read_node(c)); }
    const crd::u32 n_ext = pool_count(4U);
    if (!c.ok) { return false; }
    ext.reserve(n_ext);
    for (crd::u32 i = 0; i < n_ext; ++i) { ext.push_back(c.i32v()); }
    const crd::u32 n_sfields = pool_count(sd::kTypeRec);
    if (!c.ok) { return false; }
    sfields.reserve(n_sfields);
    for (crd::u32 i = 0; i < n_sfields; ++i) { sfields.push_back(sd::read_type(c)); }
    const crd::u32 n_sbegin = pool_count(4U);
    if (!c.ok) { return false; }
    sbegin.reserve(n_sbegin);
    for (crd::u32 i = 0; i < n_sbegin; ++i) { sbegin.push_back(c.u32v()); }
    const crd::u32 n_stmts = pool_count(sd::kStmtRec);
    if (!c.ok) { return false; }
    stmts.reserve(n_stmts);
    for (crd::u32 i = 0; i < n_stmts; ++i) { stmts.push_back(sd::read_stmt(c)); }

    const KEntry decoded = sd::read_entry(c);
    if (!c.ok) { return false; }
    e = decoded;

    g.serial_restore(nodes.data(), nodes.size(), ext.data(), ext.size(), sfields.data(), sfields.size(), sbegin.data(), sbegin.size(),
                     stmts.data(), stmts.size(), static_cast<int>(nin));
    return true;
}

// ── REFLECTION — the descriptor/vertex/push/workgroup interface derived straight from the IR (no third-party reflector). ──
enum class BindKind : crd::u8
{
    StorageBuffer,
    UniformBuffer,
    Texture,
    Sampler,
    AccelStruct
};
struct ShaderBinding
{
    crd::u32 set     = 0;
    crd::u32 binding = 0;
    BindKind kind    = BindKind::StorageBuffer;
    bool     writable = false;
};
struct VertexAttr
{
    crd::u32 location = 0;
    DType    dtype    = DType::F32;
    int      comps    = 1;
};
inline constexpr int kMaxReflBindings = 32;
inline constexpr int kMaxReflVAttrs   = 16;
struct ShaderReflection
{
    KStage        stage         = KStage::Fragment;
    crd::u32      local_size[3] = {1, 1, 1};
    int           n_bindings    = 0;
    ShaderBinding bindings[kMaxReflBindings] = {};
    int           n_vattrs      = 0;
    VertexAttr    vattrs[kMaxReflVAttrs] = {};
    [[nodiscard]] bool is_kernel() const noexcept { return stage == KStage::Compute; }
};

// Walk the IR: BufferDecl/UniformBlock/Texture/Sampler/AccelStructDecl → descriptor bindings (deduped by set·binding·kind);
// StageIn (in a vertex stage) → vertex attributes; local_size from the entry. The renderer's binding layer wires straight from this.
[[nodiscard]] inline ShaderReflection reflect(const KGraph& g, const KEntry& e)
{
    // ⛔ EXPLICIT memset — the cook writes this POD RAW into the bundle's REFL chunk (`add_chunk(kReflChunk, {&refl,
    // sizeof(refl)})`), so its PADDING IS PART OF THE COOKED BYTES: the 3-byte hole after `stage`, the 2 in ShaderBinding,
    // the 3 in VertexAttr. Leaving them indeterminate made two cooks of the SAME graph differ (measured 2026-07-25: the
    // serial/parallel cache files diverged at file offset 1881..1883 — exactly `stage`'s hole — failing D10's
    // parallel == serial byte-identity and D12's recook identity). `ShaderReflection r{}` is NOT sufficient: MSVC
    // implements value-init of a class with member initializers as "run the implicit default ctor", which writes members
    // only and leaves the holes untouched — verified, the padding stayed garbage. memset is the one form that is actually
    // guaranteed here (the type is trivially copyable — see the static_assert in shader-cook/src/cook.cpp). The element
    // writes below are FIELD-BY-FIELD for the same reason: assigning a brace-built temporary can copy the temporary's
    // indeterminate padding over the zeroed slot.
    ShaderReflection r;
    std::memset(static_cast<void*>(&r), 0, sizeof(r));
    r.stage         = e.stage;
    r.local_size[0] = e.local_size[0];
    r.local_size[1] = e.local_size[1];
    r.local_size[2] = e.local_size[2];
    const int n = g.size();

    // ENTRY-SCOPED reflection: mark the nodes reachable from THIS entry's roots, so a shared graph (e.g. a VS+FS raster pair)
    // reflects only this entry's interface — the VS sees its 3 vertex attributes, not the FS's 2 varyings. No allocation: a
    // stack bitset (mark-on-push ⇒ each node queued once ⇒ the stack is bounded by n). A graph larger than the cap falls back to
    // graph-scoped (correct for the single-entry case that dominates; a shared graph that large doesn't occur for a shader).
    constexpr int cap = 8192;
    const bool    scoped = (n <= cap);
    crd::u8       reach[cap];
    if (scoped)
    {
        for (int i = 0; i < n; ++i) { reach[i] = 0U; }
        int        stk[cap];
        int        sp   = 0;
        const auto push = [&](int id) {
            if (id >= 0 && id < n && reach[static_cast<crd::usize>(id)] == 0U) { reach[static_cast<crd::usize>(id)] = 1U; stk[sp++] = id; }
        };
        push(e.position); push(e.frag_depth); push(e.discard_cond); push(e.shading_rate);
        push(e.storage_write_index); push(e.storage_write_value);
        for (int k = 0; k < e.n_out; ++k) { push(e.out[k].node); }
        if (e.is_kernel())
        {
            for (int s = e.kernel_body_begin; s < e.kernel_body_begin + e.kernel_body_count; ++s)
            {
                const KStmt& st = g.stmt(s);
                push(st.target); push(st.index); push(st.value); push(st.result);
                for (int k = 0; k < static_cast<int>(st.n_ext); ++k) { push(g.stmt_ext_operand(st, k)); }
            }
        }
        while (sp > 0)
        {
            const KNode& nd = g.node(stk[--sp]);
            push(nd.a); push(nd.b); push(nd.c); push(nd.d);
            for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { push(g.ext_operand(nd, k)); }
        }
    }

    const auto add_bind = [&](crd::u32 set, crd::u32 binding, BindKind kind, bool writable) {
        for (int i = 0; i < r.n_bindings; ++i) { if (r.bindings[i].set == set && r.bindings[i].binding == binding && r.bindings[i].kind == kind) { return; } } // dedup
        if (r.n_bindings < kMaxReflBindings)
        {
            ShaderBinding& b = r.bindings[r.n_bindings];
            b.set            = set;
            b.binding        = binding;
            b.kind           = kind;
            b.writable       = writable;
            ++r.n_bindings;
        }
    };
    for (int i = 0; i < n; ++i)
    {
        if (scoped && reach[static_cast<crd::usize>(i)] == 0U) { continue; } // only this entry's decls
        const KNode& nd = g.node(i);
        switch (nd.op)
        {
        case KOp::BufferDecl:     add_bind(nd.dset, static_cast<crd::u32>(nd.iidx), BindKind::StorageBuffer, (nd.axes & 1U) != 0U); break;
        case KOp::UniformBlock:   add_bind(nd.dset, static_cast<crd::u32>(nd.iidx), BindKind::UniformBuffer, false); break;
        case KOp::Texture:        add_bind(nd.dset, static_cast<crd::u32>(nd.iidx), BindKind::Texture, false); break;
        case KOp::Sampler:        add_bind(nd.dset, static_cast<crd::u32>(nd.iidx), BindKind::Sampler, false); break;
        case KOp::AccelStructDecl: add_bind(nd.dset, static_cast<crd::u32>(nd.iidx), BindKind::AccelStruct, false); break;
        case KOp::StageIn:
            if (e.stage == KStage::Vertex && r.n_vattrs < kMaxReflVAttrs)
            {
                VertexAttr& v = r.vattrs[r.n_vattrs];
                v.location    = static_cast<crd::u32>(nd.iidx);
                v.dtype       = nd.dtype();
                v.comps       = nd.comps();
                ++r.n_vattrs;
            }
            break;
        default: break;
        }
    }
    return r;
}

} // namespace crd::kir
