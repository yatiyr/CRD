// CEIR-1f - the BINARY round-trip gate: serialize -> deserialize -> serialize is BYTE-EXACT, and it agrees with the
// text form (print o deserialize o serialize == print). The blob is a PURE FUNCTION OF MODULE CONTENT - the same graph
// built in a clean vs a pre-polluted Context serializes byte-equal (no Context-history leakage). The binary form (unlike
// the text form) carries Region::kind and SourceLoc; both survive, checked structurally / by PATH. Unknown chunks are
// forward-skipped; a version mismatch or a truncated blob is rejected with a byte offset. Host-only. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp / std::strcmp

#include "rich_graph.hpp" // crd::ceir::test::build_rich

using namespace crd::ceir;
using crd::ceir::test::build_rich;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::u32; // crd:: scalar aliases are not pulled in by `using namespace crd::ceir`
using crd::u8;
using crd::usize;
using ByteArray = crd::containers::Array<u8>;

namespace
{
[[nodiscard]] bool blob_equal(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool         text_equal(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// A small module exercising the two things build_rich does NOT: Region::kind (an SsaCfg module body AND an SsaCfg op
// region) and a SourceLoc (provenance). The binary form must carry both.
Module* build_kinds(Context& ctx)
{
    const u32     file = ctx.register_file("path/to/src.ceir");
    Module* const m    = ctx.create_module(RegionKind::SsaCfg); // module body is SsaCfg
    Block* const  top  = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const op = ctx.create_operation(ctx.intern_op("scf", "exec"), {}, 0U, {}, 1U);
    ctx.set_region_kind(op->region(0), RegionKind::SsaCfg); // op region is SsaCfg too
    op->set_loc(SourceLoc{file, 12U, 5U});
    Block* const inner = ctx.create_block(0U);
    op->region(0)->append(inner);
    inner->append(ctx.create_operation(ctx.intern_op("scf", "yield"), {}, 0U));
    top->append(op);
    return m;
}
} // namespace

TEST_CASE("ceir binary: a rich graph serializes, deserializes, and re-serializes byte-exact", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob1 = serialize(ctx, *build_rich(ctx), &root);
    REQUIRE(blob1.size() > 12U); // header + chunks

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(blob1));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    const ByteArray blob2 = serialize(ctx2, *pr.module, &root);
    CHECK(blob_equal(blob1, blob2));

    // and the two serial forms agree: the binary-loaded module prints identically to the original
    CHECK(text_equal(print(ctx, *build_rich(ctx), &root), print(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir binary: the blob is a pure function of module content, not of Context history", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;

    Context         clean(&root);
    const ByteArray a = serialize(clean, *build_rich(clean), &root);

    Context dirty(&root); // pollute the interned tables BEFORE building the same graph
    (void)dirty.register_file("noise.txt");
    (void)dirty.intern_op("noise", "op");
    (void)dirty.attr_string("noise-string");
    (void)dirty.attr_int(999999);
    (void)dirty.attr_symbol("noise-symbol");
    // ...including the TYPE table: noise types shift the Context TypeId numbering, so build_rich's types get DIFFERENT
    // TypeIds here than in `clean`. The TYPE pool is derived from the module WALK (first-use order) + BODY/ATTR hold
    // pool INDICES, never Context ids — so the blob must still be byte-identical (the CEIR-3a content-purity proof).
    (void)dirty.type_f64();
    (void)dirty.type_vector(dirty.type_i64(), 8U);
    const ByteArray b = serialize(dirty, *build_rich(dirty), &root);

    CHECK(blob_equal(a, b)); // identical content -> identical bytes, regardless of Context state
}

TEST_CASE("ceir binary: region kinds and source locations survive a load into a dirty context", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob = serialize(ctx, *build_kinds(ctx), &root);

    Context ctx2(&root); // pre-dirty the target so file ids / interns cannot line up by luck
    (void)ctx2.register_file("unrelated.txt");
    (void)ctx2.intern_op("other", "thing");

    const ParseResult pr = deserialize(ctx2, span(blob));
    REQUIRE(pr.ok);

    // Region::kind survives (structural — print cannot see it)
    CHECK(pr.module->body()->kind() == RegionKind::SsaCfg);
    Operation* const op = pr.module->body()->first_block()->first_op();
    REQUIRE(op != nullptr);
    REQUIRE(op->num_regions() == 1U);
    CHECK(op->region(0)->kind() == RegionKind::SsaCfg);

    // SourceLoc survives BY PATH, not by raw file id (the dirty context assigns different ids)
    const SourceLoc loc = op->loc();
    CHECK(loc.line == 12U);
    CHECK(loc.col == 5U);
    CHECK(ctx2.file_path(loc.file_id) == crd::containers::StringView("path/to/src.ceir"));

    CHECK(blob_equal(blob, serialize(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir binary: a func's symbol identity resolves after a binary load", "[ceir][binary][symbol]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob = serialize(ctx, *build_rich(ctx), &root);

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(blob));
    REQUIRE(pr.ok);

    const SymbolEntry* const e = pr.module->symbols()->lookup("callee_fn");
    REQUIRE(e != nullptr); // the parser/loader rebuilt the SymbolTable from the sym_name attr
    CHECK(e->op->kind() == func::func_kind(ctx2));
    CHECK(e->visibility == Visibility::Public);
}

TEST_CASE("ceir binary: an unknown chunk is forward-skipped", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob = serialize(ctx, *build_rich(ctx), &root);

    // Splice a synthetic 'XXXX' chunk right after the 12-byte header and bump chunk_count. A conformant reader skips
    // it by its length and loads the rest unchanged.
    ByteArray spliced(&root);
    for (usize i = 0; i < 12U; ++i) { spliced.push_back(blob[i]); }
    u32 cc = static_cast<u32>(spliced[8]) | (static_cast<u32>(spliced[9]) << 8U) |
             (static_cast<u32>(spliced[10]) << 16U) | (static_cast<u32>(spliced[11]) << 24U);
    cc += 1U;
    spliced[8]  = static_cast<u8>(cc & 0xFFU);
    spliced[9]  = static_cast<u8>((cc >> 8U) & 0xFFU);
    spliced[10] = static_cast<u8>((cc >> 16U) & 0xFFU);
    spliced[11] = static_cast<u8>((cc >> 24U) & 0xFFU);
    spliced.push_back(static_cast<u8>('X')); // fourcc 'XXXX'
    spliced.push_back(static_cast<u8>('X'));
    spliced.push_back(static_cast<u8>('X'));
    spliced.push_back(static_cast<u8>('X'));
    spliced.push_back(static_cast<u8>(3U)); // payload size = 3, little-endian
    spliced.push_back(static_cast<u8>(0U));
    spliced.push_back(static_cast<u8>(0U));
    spliced.push_back(static_cast<u8>(0U));
    spliced.push_back(static_cast<u8>(0xAAU)); // 3 payload bytes
    spliced.push_back(static_cast<u8>(0xBBU));
    spliced.push_back(static_cast<u8>(0xCCU));
    for (usize i = 12U; i < blob.size(); ++i) { spliced.push_back(blob[i]); }

    Context           ctx2(&root);
    const ParseResult pr = deserialize(ctx2, span(spliced));
    REQUIRE(pr.ok); // the unknown chunk did not break the load
    CHECK(text_equal(print(ctx, *build_rich(ctx), &root), print(ctx2, *pr.module, &root)));
}

TEST_CASE("ceir binary: malformed blobs are rejected with a byte offset", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob = serialize(ctx, *build_rich(ctx), &root);

    // version mismatch -> rejected at offset 4 (the version word)
    {
        ByteArray bad(&root);
        for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
        bad[4] = static_cast<u8>(bad[4] + 99U); // corrupt the version
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(bad));
        CHECK_FALSE(pr.ok);
        CHECK(pr.module == nullptr);
        CHECK(pr.error_offset == 4U);
    }
    // wrong magic -> rejected at offset 0
    {
        ByteArray bad(&root);
        for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
        bad[0] = static_cast<u8>(bad[0] + 1U);
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(bad));
        CHECK_FALSE(pr.ok);
        CHECK(pr.error_offset == 0U);
    }
    // truncated (only the first 6 bytes) -> rejected, never a crash
    {
        ByteArray tiny(&root);
        for (usize i = 0; i < 6U && i < blob.size(); ++i) { tiny.push_back(blob[i]); }
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(tiny));
        CHECK_FALSE(pr.ok);
    }
    // truncated mid-body (header + a fraction of the chunks) -> rejected, never a crash
    {
        ByteArray half(&root);
        for (usize i = 0; i < blob.size() / 2U; ++i) { half.push_back(blob[i]); }
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(half));
        CHECK_FALSE(pr.ok);
    }
    // empty input -> rejected
    {
        ByteArray         empty(&root);
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(empty));
        CHECK_FALSE(pr.ok);
        CHECK(pr.error_offset == 0U);
    }
    // trailing junk after the last declared chunk -> rejected (every byte must belong to a chunk)
    {
        ByteArray trailing(&root);
        for (usize i = 0; i < blob.size(); ++i) { trailing.push_back(blob[i]); }
        trailing.push_back(static_cast<u8>(0x7FU)); // one extra byte the chunk_count does not account for
        Context           c2(&root);
        const ParseResult pr = deserialize(c2, span(trailing));
        CHECK_FALSE(pr.ok);
        CHECK(pr.error_offset == blob.size());
    }
}

TEST_CASE("ceir binary: a structurally-invalid TYPE record is rejected", "[ceir][binary]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const ByteArray              blob = serialize(ctx, *build_rich(ctx), &root); // rich graph carries typed values

    auto read_u32 = [](const ByteArray& b, usize off) -> u32 {
        return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1U]) << 8U) |
               (static_cast<u32>(b[off + 2U]) << 16U) | (static_cast<u32>(b[off + 3U]) << 24U);
    };
    const u32 type_fourcc = static_cast<u32>('T') | (static_cast<u32>('Y') << 8U) | (static_cast<u32>('P') << 16U) |
                            (static_cast<u32>('E') << 24U);

    // walk chunk headers (from byte 12: after magic+version+count) to the TYPE payload — OFFSET-INDEPENDENT of the
    // other chunks' sizes, so this survives future format tweaks.
    usize pos          = 12U;
    usize type_payload = 0U;
    while (pos + 8U <= blob.size())
    {
        const u32 fourcc = read_u32(blob, pos);
        const u32 size   = read_u32(blob, pos + 4U);
        if (fourcc == type_fourcc)
        {
            type_payload = pos + 8U;
            break;
        }
        pos += 8U + size;
    }
    REQUIRE(type_payload != 0U);

    // the first record (a scalar — child-first ordering) begins after the u32 record count; its child-count `nch` sits
    // at +[kind(1)+is_signed(1)+fkind(1)+count(4)+cols(4)] = +11. Inflate it -> the decoder must reject (overruns), not
    // drive an out-of-bounds members[] in a consumer.
    ByteArray bad(&root);
    for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
    const usize nch_off = type_payload + 4U + 11U;
    REQUIRE(nch_off + 4U <= bad.size());
    bad[nch_off]      = 0xFFU;
    bad[nch_off + 1U] = 0xFFU;
    bad[nch_off + 2U] = 0xFFU;
    bad[nch_off + 3U] = 0xFFU;
    Context           c2(&root);
    const ParseResult pr = deserialize(c2, span(bad));
    CHECK_FALSE(pr.ok); // rejected, never a crash
}

TEST_CASE("ceir binary: a keyword-mapped scalar out of range (image dim) is rejected", "[ceir][binary]")
{
    // A deterministic 2-record TYPE pool: an image<d2,!f32> value -> record 0 = f32 (child-first), record 1 = image.
    // Inflate the image's dim code (a keyword-table scalar) past Cube -> the decoder must reject (else the printer would
    // map it to nothing and the text form would be silently lossy).
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m   = ctx.create_module();
    Block* const                 top = ctx.create_block(0U);
    m->body()->append(top);
    top->append(ctx.create_operation(ctx.intern_op("t", "x"), {}, 1U, ctx.type_image(ImageDim::Dim2D, ctx.type_f32())));
    const ByteArray blob = serialize(ctx, *m, &root);

    auto read_u32 = [](const ByteArray& b, usize off) -> u32 {
        return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1U]) << 8U) |
               (static_cast<u32>(b[off + 2U]) << 16U) | (static_cast<u32>(b[off + 3U]) << 24U);
    };
    const u32 type_fourcc = static_cast<u32>('T') | (static_cast<u32>('Y') << 8U) | (static_cast<u32>('P') << 16U) |
                            (static_cast<u32>('E') << 24U);
    usize pos          = 12U;
    usize type_payload = 0U;
    while (pos + 8U <= blob.size())
    {
        if (read_u32(blob, pos) == type_fourcc)
        {
            type_payload = pos + 8U;
            break;
        }
        pos += 8U + read_u32(blob, pos + 4U);
    }
    REQUIRE(type_payload != 0U);

    // record 0 (f32) is 23 bytes; record 1 (image) `count`=dim is at +[kind+is_signed+fkind = 3] within it.
    const usize dim_off = type_payload + 4U + 23U + 3U;
    ByteArray   bad(&root);
    for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
    REQUIRE(dim_off + 4U <= bad.size());
    REQUIRE(bad[dim_off] == 0x01U); // sanity: the record's dim byte is Dim2D (=1) — a layout shift would patch the wrong byte
    bad[dim_off] = 0x63U;           // 99 -> past ImageDim::Cube
    Context           c2(&root);
    const ParseResult pr = deserialize(c2, span(bad));
    CHECK_FALSE(pr.ok); // "invalid image dim"
}

TEST_CASE("ceir binary: an out-of-range ownership qualifier is rejected", "[ceir][binary]")
{
    // A deterministic 2-record TYPE pool: qual<borrow,!f32> -> record 0 = f32 (child-first), record 1 = qualified with
    // count = BorrowedView (=2). Inflate the ownership code (a keyword-table scalar) past TransientArena -> the decoder
    // must reject (else the printer would map it to nothing and the text form would be silently lossy).
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m   = ctx.create_module();
    Block* const                 top = ctx.create_block(0U);
    m->body()->append(top);
    top->append(ctx.create_operation(ctx.intern_op("t", "x"), {}, 1U,
                                     ctx.type_qualified(OwnershipKind::BorrowedView, ctx.type_f32())));
    const ByteArray blob = serialize(ctx, *m, &root);

    auto read_u32 = [](const ByteArray& b, usize off) -> u32 {
        return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1U]) << 8U) |
               (static_cast<u32>(b[off + 2U]) << 16U) | (static_cast<u32>(b[off + 3U]) << 24U);
    };
    const u32 type_fourcc = static_cast<u32>('T') | (static_cast<u32>('Y') << 8U) | (static_cast<u32>('P') << 16U) |
                            (static_cast<u32>('E') << 24U);
    usize pos          = 12U;
    usize type_payload = 0U;
    while (pos + 8U <= blob.size())
    {
        if (read_u32(blob, pos) == type_fourcc)
        {
            type_payload = pos + 8U;
            break;
        }
        pos += 8U + read_u32(blob, pos + 4U);
    }
    REQUIRE(type_payload != 0U);

    // record 0 (f32) is 23 bytes; record 1 (qualified) `count`=ownership is at +[kind+is_signed+fkind = 3] within it.
    const usize own_off = type_payload + 4U + 23U + 3U;
    ByteArray   bad(&root);
    for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
    REQUIRE(own_off + 4U <= bad.size());
    REQUIRE(bad[own_off] == 0x02U); // sanity: the record's ownership byte is BorrowedView (=2) — a layout shift patches wrong
    bad[own_off] = 0x63U;           // 99 -> past OwnershipKind::TransientArena (=8)
    Context           c2(&root);
    const ParseResult pr = deserialize(c2, span(bad));
    CHECK_FALSE(pr.ok); // "invalid ownership qualifier"
}

TEST_CASE("ceir binary: a NON-CANONICAL TYPE record (a name on an Int) is rejected", "[ceir][binary]")
{
    // A minimal module: an op named "t.x" with an i32 result. Encode order interns the op NAME first (STRP[0]="t.x",
    // non-empty), then i32's empty name (STRP[1]=""). The single TYPE record (i32) stores name_strp=1 (the ""). Repoint
    // it to STRP[0] -> the Int gains a name -> non-canonical (the printer ignores an Int's name, so it would print
    // identically to the canonical !i32 -> form-agreement broken) -> the decoder's canonical check must reject.
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    Module* const                m   = ctx.create_module();
    Block* const                 top = ctx.create_block(0U);
    m->body()->append(top);
    top->append(ctx.create_operation(ctx.intern_op("t", "x"), {}, 1U, ctx.type_i32()));
    const ByteArray blob = serialize(ctx, *m, &root);

    auto read_u32 = [](const ByteArray& b, usize off) -> u32 {
        return static_cast<u32>(b[off]) | (static_cast<u32>(b[off + 1U]) << 8U) |
               (static_cast<u32>(b[off + 2U]) << 16U) | (static_cast<u32>(b[off + 3U]) << 24U);
    };
    const u32 type_fourcc = static_cast<u32>('T') | (static_cast<u32>('Y') << 8U) | (static_cast<u32>('P') << 16U) |
                            (static_cast<u32>('E') << 24U);
    usize pos          = 12U;
    usize type_payload = 0U;
    while (pos + 8U <= blob.size())
    {
        if (read_u32(blob, pos) == type_fourcc)
        {
            type_payload = pos + 8U;
            break;
        }
        pos += 8U + read_u32(blob, pos + 4U);
    }
    REQUIRE(type_payload != 0U);

    // record 0 (i32) name_strp is at +[4 record-count][kind+is_signed+fkind+count+cols+nch = 15] = +19; it holds 1 ("").
    const usize name_off = type_payload + 4U + 15U;
    ByteArray   bad(&root);
    for (usize i = 0; i < blob.size(); ++i) { bad.push_back(blob[i]); }
    REQUIRE(name_off + 4U <= bad.size());
    REQUIRE(read_u32(bad, name_off) == 1U); // sanity: the canonical Int points at the empty string
    bad[name_off] = 0x00U;                   // repoint to STRP[0] = "t.x" (non-empty)
    Context           c2(&root);
    const ParseResult pr = deserialize(c2, span(bad));
    CHECK_FALSE(pr.ok); // non-canonical Int rejected
}
