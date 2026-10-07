// DIAG.8a: authored-source provenance through the COOK and the hot-reload SUPERVISOR. A program cooked from text under
// a named file keeps every op's authored position in its blob ('ORIG'), so the generation a ReloadSet installs resolves
// a runtime fault to `file:line:col`. A reload whose text does not cook reports where it failed (parse position, or the
// offending op's authored line) as plain values copied out of the transient cook Context, and the installed generation
// keeps navigating to its own file. A reformat-only reload is NoChange (same content hash, handles stay current) but
// moves the installed positions to the new text; an origin-free candidate leaves them alone. Expected positions come
// from scanning the text itself, never from the parser. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kFileA = "programs/diag/reload_a.ceir";
constexpr const char* kFileB = "programs/diag/reload_b.ceir";
constexpr const char* kFileC = "programs/diag/reload_c.ceir";

void register_dialects(Context& c)
{
    (void)arith::register_arith_ops(c);
    (void)core::register_core_ops(c);
    (void)func::register_dialect(c);
}
void registrar(Context& c, void* /*user*/)
{
    register_dialects(c);
}

Operation* konst(Context& c, Block* b, i64 v)
{
    Operation* const op = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_i32());
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}

// @main(%step) -> i32 { %lo = 0; %hi = `hi`; %s = addi(%hi, %hi); [an op of `stray_dialect`]; core.for(%lo, %hi, %step)
// { yield }; return %s }. A zero step faults at the core.for. `stray_dialect` (when non-empty) appends an op of a
// dialect no registrar installs, which parses but fails the cook's registration check.
Module* build_main(Context& c, i64 hi_value, const char* stray_dialect)
{
    Module* const    m  = c.create_module();
    Operation* const fn = func::create_func(c, *m, "main", Visibility::Public, 1U, c.type_i32());
    m->body()->append(c.create_block(0U));
    m->body()->first_block()->append(fn);
    Block* const     body = func::func_body_block(fn);
    Operation* const lo   = konst(c, body, 0);
    Operation* const hi   = konst(c, body, hi_value);
    Value*           ab[2] = {hi->result(0U), hi->result(0U)};
    Operation* const s = c.create_operation(c.intern_op("arith", "addi"), ConstSpan<Value*>(ab, 2U), 1U, c.type_i32());
    body->append(s);
    if (stray_dialect != nullptr)
    {
        body->append(c.create_operation(c.intern_op(stray_dialect, "probe"), {}, 0U));
    }
    Value*           range[3] = {lo->result(0U), hi->result(0U), body->arg(0U)};
    Operation* const loop = c.create_operation(c.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const     loop_body = c.create_block(1U, c.type_i32());
    loop->region(0)->append(loop_body);
    loop_body->append(c.create_operation(c.intern_op("core", "yield"), {}, 0U));
    body->append(loop);
    Value* rv[1] = {s->result(0U)};
    body->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));
    return m;
}

// The canonical text of build_main (any text the parser accepts would do).
String source_main(crd::memory::GrowableTlsfAllocator& root, i64 hi_value, const char* stray_dialect)
{
    Context c(&root);
    register_dialects(c);
    return print(c, *build_main(c, hi_value, stray_dialect), &root);
}

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

// The 1-based line holding the first occurrence of `needle`, and the 1-based column of that line's first non-blank
// character (where an op starts) — the independent oracle.
struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
TextPos find_op(StringView text, const char* needle)
{
    const usize nlen       = StringView(needle).size();
    u32         line       = 1U;
    usize       line_start = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle))
        {
            usize first = line_start;
            while (text[first] == ' ' || text[first] == '\t')
            {
                ++first;
            }
            return TextPos{line, static_cast<u32>(first - line_start + 1U)};
        }
    }
    return TextPos{};
}

// Double every line break and indent each line by two more spaces: the same program, every op on another line.
String reformat(StringView text, crd::memory::IAllocator* alloc)
{
    String out(alloc);
    for (usize i = 0; i < text.size(); ++i)
    {
        out.push_back(text[i]);
        if (text[i] == '\n')
        {
            out.append("\n  ");
        }
    }
    return out;
}

// Replace the first line containing `needle` with a line the parser rejects; returns the broken text and that line.
String break_line(StringView text, const char* needle, crd::memory::IAllocator* alloc, u32& out_line)
{
    out_line           = find_op(text, needle).line;
    String out(alloc);
    u32    line = 1U;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (line == out_line)
        {
            out.append("    )(");
            while (i < text.size() && text[i] != '\n')
            {
                ++i;
            }
            if (i < text.size())
            {
                out.push_back('\n');
            }
            ++line;
            continue;
        }
        out.push_back(text[i]);
        if (text[i] == '\n')
        {
            ++line;
        }
    }
    return out;
}

// Compile the generation's @main and run it with a zero step; the fault's closest-known authored origin.
struct FaultSite
{
    StringView file;
    u32        line = 0U;
    u32        col  = 0U;
    bool       node = false; // the origin names a valid op identity
};
FaultSite fault_site(Context& ctx, Module& m, crd::memory::IAllocator* alloc)
{
    FaultSite                 out;
    const plan::CompileResult cr = plan::compile(ctx, m, "main", alloc);
    REQUIRE(cr.ok());
    const i64             args[1] = {0};
    const plan::RunResult rr      = plan::run(cr.plan, ConstSpan<i64>(args, 1U), alloc);
    CHECK(rr.error == plan::RunError::BadForStep);
    REQUIRE(rr.fault.valid());
    CHECK(cr.plan.seqs[rr.fault.seq].instrs[rr.fault.instr].op == plan::Op::For);
    const Provenance p = plan::instr_provenance(cr.plan, rr.fault);
    CHECK(p.gap == ProvenanceGap::None);
    const Origin* const o = p.primary();
    REQUIRE(o != nullptr);
    out.file = ctx.file_path(o->loc.file_id);
    out.line = o->loc.line;
    out.col  = o->loc.col;
    out.node = o->node.valid();
    return out;
}

void check_fault_at(const FaultSite& f, const char* file, TextPos at)
{
    CHECK(f.file == StringView(file));
    CHECK(f.line == at.line);
    CHECK(f.col == at.col);
    CHECK(f.node);
}
} // namespace

TEST_CASE("diag 8a: a text cook names its file in the blob and a failed cook names its authored line", "[ceir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = source_main(root, 3, nullptr);
    const TextPos                      at_for = find_op(sv(text), "core.for");
    REQUIRE(at_for.line != 0U);

    SECTION("the cooked blob carries the file and every position into a fresh Context")
    {
        Context ctx(&root);
        register_dialects(ctx);
        const CookResult cr = cook_program_text(ctx, sv(text), StringView(kFileA), 77U, &root, &root);
        REQUIRE(cr.ok());
        // The file changes no content hash: the unnamed cook of the same text is the same program.
        Context plain(&root);
        register_dialects(plain);
        const CookResult pr = cook_program_text(plain, sv(text), 77U, &root, &root);
        REQUIRE(pr.ok());
        CHECK(pr.content_hash == cr.content_hash);
        CHECK(pr.interface_hash == cr.interface_hash);

        Context fresh(&root);
        register_dialects(fresh);
        const ReadResult rr = read_program(fresh, ConstSpan<u8>(cr.blob.data(), cr.blob.size()), &root);
        REQUIRE(rr.ok());
        check_fault_at(fault_site(fresh, *rr.module, &root), kFileA, at_for);
    }

    SECTION("a parse failure reports the line, column and file it stopped at")
    {
        u32          broken = 0U;
        const String bad    = break_line(sv(text), "arith.addi", &root, broken);
        Context      ctx(&root);
        register_dialects(ctx);
        const CookResult cr = cook_program_text(ctx, sv(bad), StringView(kFileB), 78U, &root, &root);
        CHECK(cr.error == CookError::ParseFailed);
        CHECK(cr.op == nullptr);
        CHECK(cr.site.gap == ProvenanceGap::None);
        CHECK(cr.site.line == broken);
        CHECK(cr.site.col == 5U); // the ')' after the four-space indent
        CHECK(ctx.file_path(cr.site.file_id) == StringView(kFileB));
        CHECK_FALSE(cr.site.op.valid());
    }

    SECTION("a verifier failure names the offending op's authored line")
    {
        const String  stray = source_main(root, 3, "nosuch");
        const TextPos at    = find_op(sv(stray), "nosuch.probe");
        REQUIRE(at.line != 0U);
        Context ctx(&root);
        register_dialects(ctx);
        const CookResult cr = cook_program_text(ctx, sv(stray), StringView(kFileB), 79U, &root, &root);
        CHECK(cr.error == CookError::UnregisteredOp);
        REQUIRE(cr.op != nullptr);
        CHECK(cr.site.gap == ProvenanceGap::None);
        CHECK(cr.site.line == at.line);
        CHECK(cr.site.col == at.col);
        CHECK(ctx.file_path(cr.site.file_id) == StringView(kFileB));
    }

    SECTION("a builder module with no positions reports the gap, not a zero line")
    {
        Context ctx(&root);
        register_dialects(ctx);
        Module* const    m  = build_main(ctx, 3, "nosuch");
        const CookResult cr = cook_program(ctx, *m, 80U, &root, &root);
        CHECK(cr.error == CookError::UnregisteredOp);
        CHECK(cr.site.gap == ProvenanceGap::NoSourceLocation);
        CHECK(cr.site.line == 0U);
        CHECK(cr.site.file_id == 0U);
    }
}

TEST_CASE("diag 8a: a failed source reload points at its text while the installed generation keeps its file",
          "[ceir][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{4100U};
    const String                       text   = source_main(root, 3, nullptr);
    const TextPos                      at_for = find_op(sv(text), "core.for");
    REQUIRE(set.add_source(id, sv(text), StringView(kFileA)).ok());
    Generation* const   installed = set.generation(id);
    const ProgramHandle h         = set.handle(id);
    REQUIRE(installed != nullptr);
    check_fault_at(fault_site(*installed->ctx, *installed->program.module, &root), kFileA, at_for);

    // A reload that does not parse: the position is the broken line of the NEW text; nothing is installed.
    u32                broken = 0U;
    const String       bad    = break_line(sv(text), "core.for", &root, broken);
    const ReloadResult r1     = set.reload_source(id, sv(bad), StringView(kFileB));
    CHECK(r1.cook_error == CookError::ParseFailed);
    CHECK_FALSE(r1.installed);
    CHECK(r1.cook_site.gap == ProvenanceGap::None);
    CHECK(r1.cook_site.line == broken);
    CHECK(r1.cook_site.col == 5U);
    CHECK(r1.cook_site.file_id != 0U);

    // A reload that parses but fails the registration check: the offending op's authored line in the NEW text.
    const String       stray = source_main(root, 3, "nosuch");
    const TextPos      at    = find_op(sv(stray), "nosuch.probe");
    const ReloadResult r2    = set.reload_source(id, sv(stray), StringView(kFileB));
    CHECK(r2.cook_error == CookError::UnregisteredOp);
    CHECK_FALSE(r2.installed);
    CHECK(r2.cook_site.gap == ProvenanceGap::None);
    CHECK(r2.cook_site.line == at.line);
    CHECK(r2.cook_site.col == at.col);

    // The installed generation is the same one, and its runtime fault still navigates to its own file.
    CHECK(set.generation(id) == installed);
    CHECK(set.is_current(id, h));
    check_fault_at(fault_site(*installed->ctx, *installed->program.module, &root), kFileA, at_for);

    // A first add whose text does not cook reports the same way and adds nothing.
    const AddResult ar = set.add_source(AssetId{4200U}, sv(stray), StringView(kFileB));
    CHECK(ar.error == AddError::CookFailed);
    CHECK(ar.cook_error == CookError::UnregisteredOp);
    CHECK(ar.cook_site.line == at.line);
    CHECK_FALSE(set.contains(AssetId{4200U}));
}

TEST_CASE("diag 8a: a reformat-only reload keeps the generation and moves its authored positions",
          "[ceir][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{4300U};
    const String                       text = source_main(root, 3, nullptr);
    REQUIRE(set.add_source(id, sv(text), StringView(kFileA)).ok());
    Generation* const   installed = set.generation(id);
    const ProgramHandle h         = set.handle(id);
    const u64           content   = set.program(id)->content_hash;

    const String  moved    = reformat(sv(text), &root);
    const TextPos at_moved = find_op(sv(moved), "core.for");
    REQUIRE(at_moved.line != find_op(sv(text), "core.for").line);
    const ReloadResult rr = set.reload_source(id, sv(moved), StringView(kFileC));
    CHECK(rr.cook_error == CookError::Ok);
    CHECK(rr.decision == ReloadDecision::NoChange);
    CHECK_FALSE(rr.installed);
    CHECK(set.generation(id) == installed);
    CHECK(set.is_current(id, h));
    CHECK(set.program(id)->content_hash == content);
    check_fault_at(fault_site(*installed->ctx, *installed->program.module, &root), kFileC, at_moved);

    SECTION("an origin-free candidate with the same content leaves the positions alone")
    {
        Context builder(&root);
        register_dialects(builder);
        const CookResult cr = cook_program(builder, *build_main(builder, 3, nullptr), id.value, &root, &root);
        REQUIRE(cr.ok());
        REQUIRE(cr.content_hash == content); // text and builder are the same program (§121)
        const ReloadResult nr = set.reload(id, ConstSpan<u8>(cr.blob.data(), cr.blob.size()));
        CHECK(nr.decision == ReloadDecision::NoChange);
        check_fault_at(fault_site(*installed->ctx, *installed->program.module, &root), kFileC, at_moved);
    }

    SECTION("a body edit hot-swaps to a generation that navigates to the new file")
    {
        const String       edited  = source_main(root, 4, nullptr);
        const TextPos      at_edit = find_op(sv(edited), "core.for");
        const ReloadResult er      = set.reload_source(id, sv(edited), StringView(kFileB));
        CHECK(er.decision == ReloadDecision::HotSwap);
        REQUIRE(er.installed);
        Generation* const next = set.generation(id);
        REQUIRE(next != installed);
        check_fault_at(fault_site(*next->ctx, *next->program.module, &root), kFileB, at_edit);
    }
}

namespace
{
bool has(const String& s, StringView n)
{
    const StringView hay(s.data(), s.size());
    for (usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, n.size()) == n)
        {
            return true;
        }
    }
    return false;
}

// "<file>:<line>:<col>" for the expected text position.
String file_line_col(const char* file, TextPos at, crd::memory::IAllocator* alloc)
{
    String s(alloc);
    s.append(file);
    const u32 parts[2] = {at.line, at.col};
    for (const u32 v : parts)
    {
        s.push_back(':');
        char  buf[10];
        usize k = 0U;
        u32   n = v;
        do
        {
            buf[k++] = static_cast<char>('0' + (n % 10U));
            n /= 10U;
        } while (n != 0U);
        while (k > 0U)
        {
            s.push_back(buf[--k]);
        }
    }
    return s;
}
} // namespace

TEST_CASE("diag 8a: a fault is located in the generation that ran it, through a hot swap and a drain",
          "[ceir][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{4500U};
    const String                       text_a = source_main(root, 3, nullptr);
    const String                       text_b = reformat(sv(source_main(root, 4, nullptr)), &root);
    const TextPos                      at_a   = find_op(sv(text_a), "core.for");
    const TextPos                      at_b   = find_op(sv(text_b), "core.for");
    REQUIRE(at_a.line != at_b.line);
    const String where_a = file_line_col(kFileA, at_a, &root);
    const String where_b = file_line_col(kFileB, at_b, &root);

    REQUIRE(set.add_source(id, sv(text_a), StringView(kFileA)).ok());
    const ProgramHandle h1      = set.handle(id);
    Generation* const   g1      = set.generation(id);
    const u64           hash_a  = g1->program.content_hash;
    CHECK(h1.generation.value == 1U);
    // A plan built from generation 1; it keeps running after generation 2 is installed.
    const plan::CompileResult p1 = plan::compile(*g1->ctx, *g1->program.module, "main", &root);
    REQUIRE(p1.ok());
    const i64             zero[1] = {0};
    const plan::RunResult r1      = plan::run(p1.plan, ConstSpan<i64>(zero, 1U), &root);
    REQUIRE(r1.fault.valid());
    const Provenance f1 = plan::instr_provenance(p1.plan, r1.fault);

    const GenerationSite s1 = set.locate(h1, f1, &root);
    CHECK(s1.asset == id);
    CHECK(s1.generation.value == 1U);
    CHECK(s1.state == GenerationState::Current);
    CHECK(s1.content_hash == hash_a);
    CHECK(has(s1.where, sv(where_a)));

    // A body edit in another file hot-swaps to generation 2.
    const ReloadResult rr = set.reload_source(id, sv(text_b), StringView(kFileB));
    REQUIRE(rr.installed);
    const ProgramHandle h2 = set.handle(id);
    Generation* const   g2 = set.generation(id);
    CHECK(h2.generation.value == 2U);
    CHECK(g2->program.content_hash != hash_a);

    SECTION("the old plan's fault names generation 1 and file A; the new generation's names 2 and file B")
    {
        const GenerationSite old_site = set.locate(h1, f1, &root);
        CHECK(old_site.generation.value == 1U);
        CHECK(old_site.state == GenerationState::Retiring);
        CHECK(old_site.content_hash == hash_a);
        CHECK(has(old_site.where, sv(where_a)));
        CHECK_FALSE(has(old_site.where, StringView(kFileB)));
        const String r = render_generation_site(old_site, &root);
        CHECK(has(r, StringView("asset#4500 gen#1 retiring ")));

        // The interpreter on generation 2: its error op is located in generation 2.
        exec::Interpreter in(*g2->ctx);
        exec::install_builtin_semantics(in);
        const exec::ExecResult er = in.invoke(*g2->program.module, "main", ConstSpan<i64>(zero, 1U));
        CHECK(er.error == exec::ExecError::BadForStep);
        const GenerationSite new_site = set.locate(h2, er.op, &root);
        CHECK(new_site.generation.value == 2U);
        CHECK(new_site.state == GenerationState::Current);
        CHECK(new_site.content_hash == g2->program.content_hash);
        CHECK(has(new_site.where, sv(where_b)));
        CHECK(has(render_generation_site(new_site, &root), StringView("asset#4500 gen#2 current ")));

        // A failed reload changes neither: the installed generation keeps its number and its file.
        u32          broken = 0U;
        const String bad    = break_line(sv(text_b), "core.for", &root, broken);
        CHECK(set.reload_source(id, sv(bad), StringView(kFileC)).cook_error == CookError::ParseFailed);
        const GenerationSite kept = set.locate(h2, er.op, &root);
        CHECK(kept.generation.value == 2U);
        CHECK(kept.state == GenerationState::Current);
        CHECK(has(kept.where, sv(where_b)));
    }

    SECTION("after a drain the old generation is gone: named by number only, with no file")
    {
        set.drain();
        const GenerationSite gone = set.locate(h1, f1, &root);
        CHECK(gone.asset == id);
        CHECK(gone.generation.value == 1U);
        CHECK(gone.state == GenerationState::Gone);
        CHECK(gone.content_hash == 0U);
        CHECK(gone.where.empty());
        const String r = render_generation_site(gone, &root);
        CHECK(StringView(r.data(), r.size()) == StringView("asset#4500 gen#1 gone"));
        // A handle of another asset, or one never minted, is not attributed to this program.
        CHECK(set.locate(ProgramHandle{}, f1, &root).state == GenerationState::Gone);
    }
}
