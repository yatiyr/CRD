#pragma once

// ckir_serialize.hpp — D1 (D-007): the IR-as-crdr foundation. Serialize a CKIR `KGraph` + `KEntry` to a versioned byte blob (the
// on-disk shader-graph resource that REPLACES stored GLSL/HLSL — ADR-0101 "the IR is the source of truth for all shaders"), and
// deserialize it back into a byte-identical graph, so re-emitting from the loaded graph produces BIT-IDENTICAL backend source. Plus
// `reflect()` — the descriptor-set layout, vertex-input layout, push-constant size + workgroup size derived straight from OUR IR
// (no SPIRV-Cross; own-format mandate). The cook (D2) turns this resource into per-backend bytecode; D3 adds variants; D4 loads it
// with zero runtime compile; D5 hot-reloads it.
//
// FORMAT (little-endian, ADR-0037): [FourCC 'KGPH'][version][sizeof-manifest: KNode·KStmt·KType·KEntry][n_inputs] then the five POD
// pools (nodes · ext · struct-fields · struct-begins · stmts), each [count][raw bytes], then the KEntry raw bytes. A COOK ARTIFACT
// is regenerable from source, so a POD-pool container guarded by a version + a struct-layout (sizeof) manifest is correct AND fast:
// a layout drift is DETECTED (clean reject ⇒ recook), never silently mis-read. Every pool element is trivially copyable.

#include <crd/kir/ckir.hpp>

#include <crd/containers/span.hpp> // ConstSpan (ckir.hpp brings Array but not span)

#include <type_traits>

namespace crd::kir
{

inline constexpr crd::u32 kShaderGraphFourCC =
    (static_cast<crd::u32>('K')) | (static_cast<crd::u32>('G') << 8U) | (static_cast<crd::u32>('P') << 16U) | (static_cast<crd::u32>('H') << 24U);
inline constexpr crd::u32 kShaderGraphVersion = 1U;

static_assert(std::is_trivially_copyable_v<KNode>, "KNode must be trivially copyable to pool-serialize");
static_assert(std::is_trivially_copyable_v<KStmt>, "KStmt must be trivially copyable");
static_assert(std::is_trivially_copyable_v<KType>, "KType must be trivially copyable");
static_assert(std::is_trivially_copyable_v<KEntry>, "KEntry must be trivially copyable");

// Serialize (KGraph, KEntry) → a versioned byte blob (allocated from `a`).
[[nodiscard]] inline crd::containers::Array<crd::u8> serialize_graph(const KGraph& g, const KEntry& e, crd::memory::IAllocator* a)
{
    crd::containers::Array<crd::u8> out(a);
    const auto w32 = [&](crd::u32 v) { out.push_back(static_cast<crd::u8>(v & 0xFFU)); out.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU)); out.push_back(static_cast<crd::u8>((v >> 16U) & 0xFFU)); out.push_back(static_cast<crd::u8>((v >> 24U) & 0xFFU)); };
    const auto wbytes = [&](const void* p, crd::u64 n) { const auto* b = static_cast<const crd::u8*>(p); for (crd::u64 i = 0; i < n; ++i) { out.push_back(b[i]); } };
    w32(kShaderGraphFourCC);
    w32(kShaderGraphVersion);
    w32(static_cast<crd::u32>(sizeof(KNode)));
    w32(static_cast<crd::u32>(sizeof(KStmt)));
    w32(static_cast<crd::u32>(sizeof(KType)));
    w32(static_cast<crd::u32>(sizeof(KEntry)));
    w32(static_cast<crd::u32>(g.n_inputs()));
    const auto wpool_nodes = [&](const auto& arr, crd::u64 elem) { w32(static_cast<crd::u32>(arr.size())); wbytes(arr.data(), static_cast<crd::u64>(arr.size()) * elem); };
    wpool_nodes(g.serial_nodes(), sizeof(KNode));
    wpool_nodes(g.serial_ext(), sizeof(crd::i32));
    wpool_nodes(g.serial_sfields(), sizeof(KType));
    wpool_nodes(g.serial_sbegin(), sizeof(crd::u32));
    wpool_nodes(g.serial_stmts(), sizeof(KStmt));
    wbytes(&e, sizeof(KEntry));
    return out;
}

// Deserialize a blob into a FRESH `g` (uses g's allocator for the pools) + `e`. Returns false on a bad magic / version / layout
// (a struct-layout drift ⇒ the sizeof manifest mismatches ⇒ clean reject, recook) / truncation.
[[nodiscard]] inline bool deserialize_graph(crd::containers::ConstSpan<crd::u8> in, KGraph& g, KEntry& e)
{
    crd::u64   pos = 0;
    const auto r32 = [&](crd::u32& v) -> bool {
        if (pos + 4U > in.size()) { return false; }
        v = static_cast<crd::u32>(in[pos]) | (static_cast<crd::u32>(in[pos + 1U]) << 8U) | (static_cast<crd::u32>(in[pos + 2U]) << 16U) | (static_cast<crd::u32>(in[pos + 3U]) << 24U);
        pos += 4U;
        return true;
    };
    crd::u32 magic = 0;
    crd::u32 ver   = 0;
    crd::u32 szn   = 0;
    crd::u32 szs   = 0;
    crd::u32 szt   = 0;
    crd::u32 sze   = 0;
    if (!r32(magic) || magic != kShaderGraphFourCC) { return false; }
    if (!r32(ver) || ver != kShaderGraphVersion) { return false; }
    if (!r32(szn) || szn != sizeof(KNode)) { return false; }
    if (!r32(szs) || szs != sizeof(KStmt)) { return false; }
    if (!r32(szt) || szt != sizeof(KType)) { return false; }
    if (!r32(sze) || sze != sizeof(KEntry)) { return false; }
    crd::u32 nin = 0;
    if (!r32(nin)) { return false; }

    auto*      al    = g.serial_nodes().allocator();
    const auto rpool = [&]<typename T>(crd::containers::Array<T>& arr) -> bool {
        crd::u32 cnt = 0;
        if (!r32(cnt)) { return false; }
        const crd::u64 nbytes = static_cast<crd::u64>(cnt) * sizeof(T);
        if (pos + nbytes > in.size()) { return false; }
        arr.clear();
        arr.resize(cnt, T{});
        auto* dst = reinterpret_cast<crd::u8*>(arr.data());
        for (crd::u64 i = 0; i < nbytes; ++i) { dst[i] = in[pos + i]; }
        pos += nbytes;
        return true;
    };
    crd::containers::Array<KNode>    nodes(al);
    crd::containers::Array<crd::i32> ext(al);
    crd::containers::Array<KType>    sfields(al);
    crd::containers::Array<crd::u32> sbegin(al);
    crd::containers::Array<KStmt>    stmts(al);
    if (!rpool(nodes) || !rpool(ext) || !rpool(sfields) || !rpool(sbegin) || !rpool(stmts)) { return false; }
    if (pos + sizeof(KEntry) > in.size()) { return false; }
    auto* ep = reinterpret_cast<crd::u8*>(&e);
    for (crd::u64 i = 0; i < sizeof(KEntry); ++i) { ep[i] = in[pos + i]; }
    pos += sizeof(KEntry);

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
    ShaderReflection r;
    r.stage         = e.stage;
    r.local_size[0] = e.local_size[0];
    r.local_size[1] = e.local_size[1];
    r.local_size[2] = e.local_size[2];
    const int n = g.size();

    // ENTRY-SCOPED reflection: mark the nodes reachable from THIS entry's roots, so a shared graph (e.g. a VS+FS raster pair)
    // reflects only this entry's interface — the VS sees its 3 vertex attributes, not the FS's 2 varyings. No allocation: a
    // stack bitset (mark-on-push ⇒ each node queued once ⇒ the stack is bounded by n). A graph larger than the cap falls back to
    // graph-scoped (correct for the single-entry case that dominates; a shared graph that large doesn't occur for a shader).
    constexpr int kCap = 8192;
    const bool    scoped = (n <= kCap);
    crd::u8       reach[kCap];
    if (scoped)
    {
        for (int i = 0; i < n; ++i) { reach[i] = 0U; }
        int        stk[kCap];
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
        if (r.n_bindings < kMaxReflBindings) { r.bindings[r.n_bindings] = {set, binding, kind, writable}; ++r.n_bindings; }
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
            if (e.stage == KStage::Vertex && r.n_vattrs < kMaxReflVAttrs) { r.vattrs[r.n_vattrs] = {static_cast<crd::u32>(nd.iidx), nd.dtype(), nd.comps()}; ++r.n_vattrs; }
            break;
        default: break;
        }
    }
    return r;
}

} // namespace crd::kir
