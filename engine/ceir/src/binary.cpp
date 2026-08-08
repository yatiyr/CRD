#include <crd/ceir/binary.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/detail/string_view_hash.hpp>
#include <crd/ceir/detail/symbol_registration.hpp>
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/hash.hpp> // fnv1a_64 — the stable content hash over the serialized blob
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir
{
namespace
{
using ByteArray = containers::Array<u8>;

[[nodiscard]] constexpr u32 make_fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8U) |
           (static_cast<u32>(static_cast<u8>(c)) << 16U) | (static_cast<u32>(static_cast<u8>(d)) << 24U);
}
constexpr u32 kChunkStrp = make_fourcc('S', 'T', 'R', 'P'); // string pool
constexpr u32 kChunkSrcm = make_fourcc('S', 'R', 'C', 'M'); // source-file map (indices into STRP)
constexpr u32 kChunkAttr = make_fourcc('A', 'T', 'T', 'R'); // attribute-value pool
constexpr u32 kChunkBody = make_fourcc('B', 'O', 'D', 'Y'); // the region graph

// ── writer helpers (little-endian, field-by-field — every hole is removed by construction) ──
void put_u8(ByteArray& b, u8 v) { b.push_back(v); }
void put_u32(ByteArray& b, u32 v)
{
    for (u32 s = 0; s < 32U; s += 8U) { b.push_back(static_cast<u8>((v >> s) & 0xFFU)); }
}
void put_u64(ByteArray& b, u64 v)
{
    for (u32 s = 0; s < 64U; s += 8U) { b.push_back(static_cast<u8>((v >> s) & 0xFFU)); }
}
void put_i64(ByteArray& b, i64 v) { put_u64(b, static_cast<u64>(v)); }
void put_str(ByteArray& b, containers::StringView s)
{
    put_u32(b, static_cast<u32>(s.size()));
    for (usize i = 0; i < s.size(); ++i) { b.push_back(static_cast<u8>(s[i])); }
}

// A bounds-checked read cursor (the CKIR-serialize idiom): any short read latches `ok=false` and yields zeroes, so
// callers check once. Byte order matches the writers above.
struct Cursor
{
    containers::ConstSpan<u8> in;
    u64                       pos = 0;
    bool                      ok  = true;

    [[nodiscard]] bool have(u64 n) noexcept
    {
        if (!ok || pos + n > in.size()) { ok = false; }
        return ok;
    }
    [[nodiscard]] u8 u8v() noexcept { return have(1U) ? in[pos++] : static_cast<u8>(0); }
    [[nodiscard]] u32 u32v() noexcept
    {
        if (!have(4U)) { return 0U; }
        u32 v = 0;
        for (u32 i = 0; i < 4U; ++i) { v |= static_cast<u32>(in[pos + i]) << (i * 8U); }
        pos += 4U;
        return v;
    }
    [[nodiscard]] u64 u64v() noexcept
    {
        if (!have(8U)) { return 0U; }
        u64 v = 0;
        for (u32 i = 0; i < 8U; ++i) { v |= static_cast<u64>(in[pos + i]) << (i * 8U); }
        pos += 8U;
        return v;
    }
    [[nodiscard]] i64 i64v() noexcept { return static_cast<i64>(u64v()); }
};

// "dialect.op" → (dialect, op) on the FIRST '.', reproducing intern_op's concatenation exactly (mirrors parse.cpp).
[[nodiscard]] bool split_op_name(containers::StringView full, containers::StringView& dialect,
                                 containers::StringView& op) noexcept
{
    for (usize i = 0; i < full.size(); ++i)
    {
        if (full[i] == '.')
        {
            dialect = containers::StringView(full.data(), i);
            op      = containers::StringView(full.data() + i + 1U, full.size() - i - 1U);
            return !dialect.empty() && !op.empty();
        }
    }
    return false;
}

// ─────────────────────────────────────────── ENCODER ───────────────────────────────────────────
// Two passes over the module: (0) assign SSA value ids by the SAME fixed pre-order as the printer; (1) walk again
// emitting the BODY payload while interning strings/attrs/files into their pools ON DEMAND (first-use order). All
// pool references in BODY are pool INDICES, never Context ids — that is what makes the blob content-pure.
class Serializer
{
public:
    Serializer(Context& ctx, memory::IAllocator* alloc)
        : m_ctx(ctx), m_alloc(alloc), m_ids(alloc), m_strp(alloc), m_strp_index(alloc), m_srcm(alloc),
          m_srcm_index(alloc), m_attr(alloc), m_attr_index(alloc), m_body(alloc)
    {
    }

    [[nodiscard]] ByteArray run(const Module& module)
    {
        assign_ids(module.body());   // pass 0
        encode_region(module.body()); // pass 1 → fills the pools + m_body

        ByteArray out(m_alloc);
        put_u32(out, kBinaryMagic);
        put_u32(out, kBinaryVersion);
        put_u32(out, 4U); // chunk_count: STRP, SRCM, ATTR, BODY
        emit_strp(out);
        emit_srcm(out);
        emit_attr(out);
        emit_chunk(out, kChunkBody, m_body);
        return out;
    }

private:
    // pass 0 — identical to print.cpp::assign_ids (block-args → op-results → recurse op-regions)
    void assign_ids(Region* r)
    {
        if (r == nullptr) { return; }
        for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (u32 i = 0; i < b->num_args(); ++i) { m_ids.insert(b->arg(i), m_next++); }
            for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
            {
                for (u32 i = 0; i < op->num_results(); ++i) { m_ids.insert(op->result(i), m_next++); }
                for (u32 i = 0; i < op->num_regions(); ++i) { assign_ids(op->region(i)); }
            }
        }
    }

    [[nodiscard]] u32 intern_str(containers::StringView s)
    {
        if (const u32* const p = m_strp_index.find(s)) { return *p; }
        const auto idx = static_cast<u32>(m_strp.size());
        m_strp.push_back(s);
        m_strp_index.insert(s, idx);
        return idx;
    }
    [[nodiscard]] u32 intern_attr(AttrId id)
    {
        if (const u32* const p = m_attr_index.find(id.value)) { return *p; }
        const auto idx = static_cast<u32>(m_attr.size());
        m_attr.push_back(id);
        m_attr_index.insert(id.value, idx);
        // Eagerly pool the value's backing string so STRP is COMPLETE before any chunk is emitted (STRP is written
        // before ATTR). A String/SymbolRef value's text becomes a STRP entry the ATTR chunk then references by index.
        const AttrValue v = m_ctx.attr_value(id);
        if (v.kind == AttrKind::String || v.kind == AttrKind::SymbolRef) { (void)intern_str(v.s); }
        return idx;
    }
    [[nodiscard]] u32 intern_srcm(u32 file_id) // returns the 0-based SRCM index for a (nonzero) file id
    {
        if (const u32* const p = m_srcm_index.find(file_id)) { return *p; }
        const auto idx = static_cast<u32>(m_srcm.size());
        m_srcm.push_back(intern_str(m_ctx.file_path(file_id))); // SRCM stores the STRP index of the path
        m_srcm_index.insert(file_id, idx);
        return idx;
    }

    void encode_region(Region* r)
    {
        put_u8(m_body, static_cast<u8>(r->kind())); // ⛔ the region kind as a byte (a cast — I6 forbids a kind-method dispatch)
        u32 nb = 0;
        for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region()) { ++nb; }
        put_u32(m_body, nb);
        for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region()) { encode_block(b); }
    }
    void encode_block(Block* b)
    {
        put_u32(m_body, b->num_args());
        put_u32(m_body, b->num_args() > 0U ? b->arg(0)->type().value : 0U); // ONE arg type (uniform block model)
        u32 nops = 0;
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { ++nops; }
        put_u32(m_body, nops);
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block()) { encode_op(op); }
    }
    void encode_op(Operation* op)
    {
        put_u32(m_body, intern_str(m_ctx.op_name(op->kind())));
        put_u32(m_body, op->num_operands());
        for (u32 i = 0; i < op->num_operands(); ++i)
        {
            const u32* const id = m_ids.find(op->operand(i));
            CRD_ASSERT_MSG(id != nullptr, "serializing an operand whose defining value was not numbered (foreign/dangling)");
            put_u32(m_body, id != nullptr ? *id : 0U);
        }
        put_u32(m_body, op->num_results());
        put_u32(m_body, op->num_results() > 0U ? op->result(0)->type().value : 0U);
        put_u32(m_body, op->num_regions());
        put_u32(m_body, op->num_attrs());
        for (u32 i = 0; i < op->num_attrs(); ++i)
        {
            put_u32(m_body, intern_str(op->attr_name(i)));
            put_u32(m_body, intern_attr(op->attr_id_at(i)));
        }
        const SourceLoc loc = op->loc();
        put_u32(m_body, loc.file_id != 0U ? intern_srcm(loc.file_id) + 1U : 0U); // 1-based ref; 0 = no source file
        put_u32(m_body, loc.line);
        put_u32(m_body, loc.col);
        for (u32 i = 0; i < op->num_regions(); ++i) { encode_region(op->region(i)); }
    }

    void emit_chunk(ByteArray& out, u32 fourcc, const ByteArray& payload)
    {
        put_u32(out, fourcc);
        put_u32(out, static_cast<u32>(payload.size()));
        for (usize i = 0; i < payload.size(); ++i) { out.push_back(payload[i]); }
    }
    void emit_strp(ByteArray& out)
    {
        ByteArray p(m_alloc);
        put_u32(p, static_cast<u32>(m_strp.size()));
        for (usize i = 0; i < m_strp.size(); ++i) { put_str(p, m_strp[i]); }
        emit_chunk(out, kChunkStrp, p);
    }
    void emit_srcm(ByteArray& out)
    {
        ByteArray p(m_alloc);
        put_u32(p, static_cast<u32>(m_srcm.size()));
        for (usize i = 0; i < m_srcm.size(); ++i) { put_u32(p, m_srcm[i]); } // STRP index of each path
        emit_chunk(out, kChunkSrcm, p);
    }
    void emit_attr(ByteArray& out)
    {
        ByteArray p(m_alloc);
        put_u32(p, static_cast<u32>(m_attr.size()));
        for (usize i = 0; i < m_attr.size(); ++i) { encode_attr_value(p, m_ctx.attr_value(m_attr[i])); }
        emit_chunk(out, kChunkAttr, p);
    }
    void encode_attr_value(ByteArray& p, const AttrValue& v)
    {
        put_u8(p, static_cast<u8>(v.kind));
        switch (v.kind) // v.kind is a MEMBER enum (allowed); the I6 rule targets an op-kind METHOD dispatch
        {
        case AttrKind::Int:       put_i64(p, v.i); break;
        case AttrKind::Float:     put_u64(p, v.f); break; // v.f is ALREADY the f64 bit pattern — NaN payloads survive
        case AttrKind::Bool:      put_u8(p, v.b ? 1U : 0U); break;
        case AttrKind::String:                            // both text kinds emit the STRP index (kind byte distinguishes)
        case AttrKind::SymbolRef: put_u32(p, intern_str(v.s)); break;
        case AttrKind::Type:      put_u32(p, v.t.value); break;
        }
    }

    Context&                                                                      m_ctx;
    memory::IAllocator*                                                           m_alloc;
    containers::HashMap<Value*, u32>                                              m_ids;        // pass-0 SSA numbering
    containers::Array<containers::StringView>                                     m_strp;       // string pool
    containers::HashMap<containers::StringView, u32, detail::StringViewHash>      m_strp_index; // string → pool idx
    containers::Array<u32>                                                        m_srcm;       // srcm idx → STRP idx
    containers::HashMap<u32, u32>                                                 m_srcm_index; // file_id → srcm idx
    containers::Array<AttrId>                                                     m_attr;       // attr pool
    containers::HashMap<u32, u32>                                                 m_attr_index; // AttrId.value → idx
    ByteArray                                                                     m_body;
    u32                                                                           m_next = 0U;
};

// ─────────────────────────────────────────── DECODER ───────────────────────────────────────────
// Scans chunks (skipping unknown FourCCs by their length), decodes the STRP/SRCM/ATTR pools, then rebuilds the graph
// from BODY in the SAME creation order the encoder walked, so value ids are implicit. Operands that reference a value
// not yet created (a Graph-region use-before-def) defer to a fixup pass, mirroring the text parser.
class Deserializer
{
public:
    Deserializer(Context& ctx, containers::ConstSpan<u8> bytes)
        : m_ctx(ctx), m_bytes(bytes), m_strings(ctx.allocator()), m_files(ctx.allocator()), m_attrs(ctx.allocator()),
          m_values(ctx.allocator()), m_fixups(ctx.allocator())
    {
    }

    [[nodiscard]] ParseResult run()
    {
        if (!parse_header() || !scan_chunks() || !decode_strp() || !decode_srcm() || !decode_attr())
        {
            return err_result();
        }
        m_module = m_ctx.create_module();
        m_bc     = Cursor{m_body};
        decode_region(m_module->body());
        if (m_ok && !m_bc.ok) { fail(m_body_off + m_bc.pos, "truncated BODY chunk"); }
        resolve_fixups();
        if (!m_ok) { return err_result(); }
        return ParseResult{m_module, true, 0U, ""};
    }

private:
    void fail(u64 off, const char* msg) noexcept
    {
        if (m_ok)
        {
            m_ok      = false;
            m_err_off = off;
            m_err     = msg;
        }
    }
    [[nodiscard]] ParseResult err_result() const noexcept
    {
        return ParseResult{nullptr, false, static_cast<usize>(m_err_off), m_err};
    }

    [[nodiscard]] bool parse_header() noexcept
    {
        Cursor c{m_bytes};
        const u32 magic = c.u32v();
        if (!c.ok) { fail(0U, "truncated header"); return false; }
        if (magic != kBinaryMagic) { fail(0U, "not a CEIR binary blob (bad magic)"); return false; }
        const u32 version = c.u32v();
        if (!c.ok) { fail(4U, "truncated header"); return false; }
        if (version != kBinaryVersion) { fail(4U, "unsupported CEIR binary version"); return false; }
        m_chunk_count = c.u32v();
        if (!c.ok) { fail(8U, "truncated header"); return false; }
        m_scan_pos = c.pos; // 12
        return true;
    }

    // Index chunks by FourCC; an unknown FourCC is simply skipped by its length (forward compatibility).
    [[nodiscard]] bool scan_chunks() noexcept
    {
        Cursor c{m_bytes};
        c.pos = m_scan_pos;
        for (u32 i = 0; i < m_chunk_count; ++i)
        {
            const u64 hdr_off = c.pos;
            const u32 fourcc  = c.u32v();
            const u32 size    = c.u32v();
            if (!c.ok) { fail(hdr_off, "truncated chunk header"); return false; }
            if (c.pos + size > m_bytes.size()) { fail(c.pos, "chunk payload overruns the blob"); return false; }
            const containers::ConstSpan<u8> payload = m_bytes.subspan(c.pos, size);
            if (fourcc == kChunkStrp) { m_strp = payload; m_strp_off = c.pos; m_has_strp = true; }
            else if (fourcc == kChunkSrcm) { m_srcm = payload; m_srcm_off = c.pos; }
            else if (fourcc == kChunkAttr) { m_attr = payload; m_attr_off = c.pos; }
            else if (fourcc == kChunkBody) { m_body = payload; m_body_off = c.pos; m_has_body = true; }
            // else: unknown chunk — skipped by length (forward compatibility)
            c.pos += size;
        }
        // Every byte must belong to a declared chunk — trailing junk is a malformed blob (forward-compat is served by
        // the unknown-chunk skip WITHIN the declared count, not by a permissive tail).
        if (c.pos != m_bytes.size()) { fail(c.pos, "trailing bytes after the last chunk"); return false; }
        if (!m_has_strp) { fail(0U, "missing STRP chunk"); return false; }
        if (!m_has_body) { fail(0U, "missing BODY chunk"); return false; }
        return true;
    }

    [[nodiscard]] bool decode_strp() noexcept
    {
        Cursor    c{m_strp};
        const u32 n = c.u32v();
        for (u32 i = 0; i < n; ++i)
        {
            const u32 len = c.u32v();
            if (!c.have(len)) { fail(m_strp_off + c.pos, "truncated STRP entry"); return false; }
            m_strings.push_back(containers::StringView(reinterpret_cast<const char*>(m_strp.data() + c.pos), len));
            c.pos += len;
        }
        if (!c.ok) { fail(m_strp_off + c.pos, "malformed STRP chunk"); return false; }
        return true;
    }

    [[nodiscard]] bool decode_srcm() noexcept
    {
        if (m_srcm.size() == 0) { return true; } // optional
        Cursor    c{m_srcm};
        const u32 n = c.u32v();
        for (u32 i = 0; i < n; ++i)
        {
            const u32 sidx = c.u32v();
            if (!c.ok) { break; }
            if (sidx >= m_strings.size()) { fail(m_srcm_off + c.pos, "SRCM path index out of range"); return false; }
            m_files.push_back(m_ctx.register_file(m_strings[sidx]));
        }
        if (!c.ok) { fail(m_srcm_off + c.pos, "malformed SRCM chunk"); return false; }
        return true;
    }

    [[nodiscard]] bool decode_attr() noexcept
    {
        if (m_attr.size() == 0) { return true; } // optional
        Cursor    c{m_attr};
        const u32 n = c.u32v();
        for (u32 i = 0; i < n; ++i)
        {
            const u8 kind = c.u8v();
            if (!c.ok) { break; }
            AttrId id{};
            if (kind == static_cast<u8>(AttrKind::Int)) { id = m_ctx.attr_int(c.i64v()); }
            else if (kind == static_cast<u8>(AttrKind::Float))
            {
                AttrValue v;
                v.kind = AttrKind::Float;
                v.f    = c.u64v(); // the raw f64 bit pattern (NaN/signed-zero exact — no f64 register round-trip)
                id     = m_ctx.intern_attr(v);
            }
            else if (kind == static_cast<u8>(AttrKind::Bool)) { id = m_ctx.attr_bool(c.u8v() != 0U); }
            else if (kind == static_cast<u8>(AttrKind::String) || kind == static_cast<u8>(AttrKind::SymbolRef))
            {
                const u32 sidx = c.u32v();
                if (sidx >= m_strings.size()) { fail(m_attr_off + c.pos, "ATTR string index out of range"); return false; }
                id = kind == static_cast<u8>(AttrKind::String) ? m_ctx.attr_string(m_strings[sidx])
                                                               : m_ctx.attr_symbol(m_strings[sidx]);
            }
            else if (kind == static_cast<u8>(AttrKind::Type)) { id = m_ctx.attr_type(TypeId{c.u32v()}); }
            else { fail(m_attr_off + c.pos, "unknown attribute kind"); return false; }
            m_attrs.push_back(id);
        }
        if (!c.ok) { fail(m_attr_off + c.pos, "malformed ATTR chunk"); return false; }
        return true;
    }

    // ── BODY (the region graph) — creation order == the encoder's pre-order, so value ids are implicit ──
    void register_value(Value* v) noexcept { m_values.push_back(v); }
    [[nodiscard]] Value* resolve(u32 id) const noexcept
    {
        return static_cast<usize>(id) < m_values.size() ? m_values[id] : nullptr;
    }
    [[nodiscard]] containers::StringView str(u32 idx) noexcept
    {
        if (static_cast<usize>(idx) >= m_strings.size())
        {
            fail(m_body_off + m_bc.pos, "BODY string index out of range");
            return {};
        }
        return m_strings[idx];
    }
    [[nodiscard]] AttrId attr_at(u32 idx) noexcept
    {
        if (static_cast<usize>(idx) >= m_attrs.size())
        {
            fail(m_body_off + m_bc.pos, "BODY attribute index out of range");
            return {};
        }
        return m_attrs[idx];
    }

    void decode_region(Region* r)
    {
        const u8 kind = m_bc.u8v();
        if (kind > static_cast<u8>(RegionKind::SsaCfg)) { fail(m_body_off + m_bc.pos, "invalid region kind"); return; }
        m_ctx.set_region_kind(r, static_cast<RegionKind>(kind));
        const u32 nb = m_bc.u32v();
        if (!m_bc.ok) { return; }
        // Each block is >= 12 bytes (num_args + arg_type + num_ops) — a block count that overruns the chunk is
        // malformed, so reject BEFORE the loop rather than spin (defence-in-depth over the per-block ok-break).
        if (!m_bc.have(static_cast<u64>(nb) * 12U)) { fail(m_body_off + m_bc.pos, "block count overruns the chunk"); return; }
        for (u32 i = 0; i < nb && m_ok; ++i) { decode_block(r); }
    }
    void decode_block(Region* r)
    {
        const u32 num_args = m_bc.u32v();
        const u32 arg_type = m_bc.u32v();
        const u32 num_ops  = m_bc.u32v();
        if (!m_bc.ok) { fail(m_body_off + m_bc.pos, "truncated block header"); return; }
        // num_args costs no stream bytes (create_block allocates from it) — cap it against the v1 limit.
        if (num_args > kMaxDecodeCount) { fail(m_body_off + m_bc.pos, "block-arg count exceeds the v1 cap"); return; }
        Block* const b = m_ctx.create_block(num_args, TypeId{arg_type});
        r->append(b);
        for (u32 i = 0; i < num_args; ++i) { register_value(b->arg(i)); }
        for (u32 i = 0; i < num_ops && m_ok; ++i) { decode_op(b); }
    }
    void decode_op(Block* b)
    {
        const u32 name_idx     = m_bc.u32v();
        const u32 num_operands = m_bc.u32v();
        // Each operand is 4 stream bytes — a count that overruns the chunk is malformed. Guard BEFORE the loop: the
        // Cursor latches to zero on a short read but does NOT stop the loop, so a hostile 4e9 would push 4e9 zeros.
        if (!m_bc.have(static_cast<u64>(num_operands) * 4U)) { fail(m_body_off + m_bc.pos, "operand count overruns the chunk"); return; }
        containers::Array<u32>    operand_ids(m_ctx.allocator());
        containers::Array<Value*> operand_vals(m_ctx.allocator());
        for (u32 i = 0; i < num_operands; ++i)
        {
            const u32 id = m_bc.u32v();
            operand_ids.push_back(id);
            operand_vals.push_back(resolve(id));
        }
        const u32 num_results = m_bc.u32v();
        const u32 result_type = m_bc.u32v();
        const u32 num_regions = m_bc.u32v();
        const u32 num_attrs   = m_bc.u32v();
        // num_results costs no stream bytes (create_operation allocates from it) — cap it; each attr is 8 stream bytes
        // and each region is >= 5 (kind + block count) — bound those by the chunk length.
        if (num_results > kMaxDecodeCount) { fail(m_body_off + m_bc.pos, "result count exceeds the v1 cap"); return; }
        if (!m_bc.have(static_cast<u64>(num_attrs) * 8U)) { fail(m_body_off + m_bc.pos, "attribute count overruns the chunk"); return; }
        if (!m_bc.have(static_cast<u64>(num_regions) * 5U)) { fail(m_body_off + m_bc.pos, "region count overruns the chunk"); return; }
        containers::Array<u32> attr_name_idx(m_ctx.allocator());
        containers::Array<u32> attr_val_idx(m_ctx.allocator());
        for (u32 i = 0; i < num_attrs; ++i)
        {
            attr_name_idx.push_back(m_bc.u32v());
            attr_val_idx.push_back(m_bc.u32v());
        }
        const u32 srcm_ref = m_bc.u32v();
        const u32 line     = m_bc.u32v();
        const u32 col      = m_bc.u32v();
        if (!m_bc.ok) { fail(m_body_off + m_bc.pos, "truncated operation record"); return; }

        containers::StringView dialect;
        containers::StringView opname;
        if (!split_op_name(str(name_idx), dialect, opname))
        {
            if (m_ok) { fail(m_body_off + m_bc.pos, "operation name is not 'dialect.op'"); }
            return;
        }
        const OpId kind = m_ctx.intern_op(dialect, opname);

        Operation* const op = m_ctx.create_operation(
            kind, containers::ConstSpan<Value*>(operand_vals.data(), operand_vals.size()), num_results,
            TypeId{result_type}, num_regions);
        b->append(op);
        for (u32 i = 0; i < num_results; ++i) { register_value(op->result(i)); }
        for (usize i = 0; i < operand_ids.size(); ++i)
        {
            if (operand_vals[i] == nullptr) { m_fixups.push_back(Fixup{op, static_cast<u32>(i), operand_ids[i]}); }
        }
        for (usize i = 0; i < attr_name_idx.size(); ++i)
        {
            const containers::StringView an = str(attr_name_idx[i]);
            const AttrId                 av = attr_at(attr_val_idx[i]);
            if (!m_ok) { return; }
            m_ctx.set_attr(op, an, av);
        }
        u32 file_id = 0U;
        if (srcm_ref != 0U)
        {
            if (srcm_ref - 1U >= m_files.size()) { fail(m_body_off + m_bc.pos, "source-loc file ref out of range"); return; }
            file_id = m_files[srcm_ref - 1U];
        }
        op->set_loc(SourceLoc{file_id, line, col});

        if (!detail::register_symbol(m_ctx, *m_module, op)) { fail(m_body_off + m_bc.pos, "duplicate symbol definition"); return; }
        for (u32 i = 0; i < num_regions && m_ok; ++i) { decode_region(op->region(i)); }
    }

    void resolve_fixups() noexcept
    {
        if (!m_ok) { return; }
        for (usize i = 0; i < m_fixups.size(); ++i)
        {
            const Fixup& f = m_fixups[i];
            Value* const v = resolve(f.id);
            if (v == nullptr) { fail(m_body_off, "operand references an undefined SSA value"); return; }
            f.op->set_operand(f.idx, v);
        }
    }

    struct Fixup
    {
        Operation* op  = nullptr;
        u32        idx = 0;
        u32        id  = 0;
    };

    Context&                                 m_ctx;
    containers::ConstSpan<u8>                 m_bytes;
    Module*                                  m_module = nullptr;
    u32                                      m_chunk_count = 0;
    u64                                      m_scan_pos    = 0;

    containers::ConstSpan<u8> m_strp;
    containers::ConstSpan<u8> m_srcm;
    containers::ConstSpan<u8> m_attr;
    containers::ConstSpan<u8> m_body;
    u64                       m_strp_off = 0;
    u64                       m_srcm_off = 0;
    u64                       m_attr_off = 0;
    u64                       m_body_off = 0;
    bool                      m_has_strp = false;
    bool                      m_has_body = false;

    containers::Array<containers::StringView> m_strings; // STRP → views into the blob
    containers::Array<u32>                    m_files;   // SRCM idx → target file_id
    containers::Array<AttrId>                 m_attrs;   // ATTR idx → target AttrId
    containers::Array<Value*>                 m_values;  // SSA id → value (creation order)
    containers::Array<Fixup>                  m_fixups;

    Cursor      m_bc; // the BODY read cursor
    bool        m_ok      = true;
    u64         m_err_off = 0;
    const char* m_err     = "";
};
} // namespace

containers::Array<u8> serialize(Context& ctx, const Module& module, memory::IAllocator* alloc)
{
    Serializer s(ctx, alloc);
    return s.run(module);
}

ParseResult deserialize(Context& ctx, containers::ConstSpan<u8> bytes)
{
    Deserializer d(ctx, bytes);
    return d.run();
}

u64 stable_hash(Context& ctx, const Module& module, memory::IAllocator* scratch)
{
    const containers::Array<u8> blob = serialize(ctx, module, scratch);
    return containers::fnv1a_64(blob.data(), blob.size());
}
} // namespace crd::ceir
