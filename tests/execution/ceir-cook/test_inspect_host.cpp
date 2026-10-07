// DIAG.8b: the INSPECT HOST (inspect_host.hpp), the composition the headless and sandbox consumers share. It runs
// the committed authored program assets/ceir/inspect_demo.ceir: the ReloadSet cooks the text under a file name, the
// host compiles `main` and runs it on its own executing thread while this test thread is the controller. Covered: the
// committed asset is the builder's printed form (anti-drift through the printer); a line breakpoint stops at its
// authored line with typed snapshots (a quantity carries its unit; the op at the stop is not yet computed; a loop body
// value is out of scope; a caller's value is out of scope from the callee's frame); step into and out move between
// frames; cancel at a pause ends the run Cancelled; a hot reload installs a new generation and requests naming the
// replaced one are refused before any work, while a load or start during an execution is refused Busy; a rejected
// reload keeps the last good generation; a failed cook names its line; and destroying the host while the execution is
// paused cancels and joins it. Expected lines come from scanning the text itself, never from the parser or the host.
// ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace insp = crd::ceir::inspect;

namespace
{
constexpr const char* kFile   = "ceir/inspect_demo.ceir";
constexpr const char* kAsset  = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";
constexpr u32         kWaitMs = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
const AssetId         kId{4700U};

void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)core::register_core_ops(ctx);
    (void)func::register_dialect(ctx);
}
void registrar(Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
}

Operation* konst(Context& ctx, Block* b, i64 v, TypeId type)
{
    Operation* const op = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, type);
    ctx.set_attr(op, "value", ctx.attr_int(v));
    b->append(op);
    return op;
}

Operation* binop(Context& ctx, Block* b, const char* name, Value* x, Value* y)
{
    Value*           in[2] = {x, y};
    Operation* const op =
        ctx.create_operation(ctx.intern_op("arith", name), ConstSpan<Value*>(in, 2U), 1U, ctx.type_i32());
    b->append(op);
    return op;
}

// The anti-drift oracle of the committed asset:
// @scale(%p) { %h = muli(%p, %p); return %h }
// @main(%n) { %zero = 0; %one = 1; %two = 2; %len = 7 : qty<i32,L>; %a = addi(%one,%two); %s = addi(%a,%a);
//             %t = call @scale(%s); core.for(%zero, %n, %one) { %x = addi(%iv,%s); yield }; return %t }
// main(n) returns ((1 + 2) * 2)^2 = 36 for any n >= 0.
Module* build_program(Context& ctx)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));

    Operation* const scale = func::create_func(ctx, *m, "scale", Visibility::Public, 1U, ctx.type_i32());
    m->body()->first_block()->append(scale);
    Block* const     sb    = func::func_body_block(scale);
    Operation* const h     = binop(ctx, sb, "muli", sb->arg(0U), sb->arg(0U));
    Value*           hr[1] = {h->result(0U)};
    sb->append(func::create_return(ctx, ConstSpan<Value*>(hr, 1U)));

    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 1U, ctx.type_i32());
    m->body()->first_block()->append(fn);
    Block* const     body = func::func_body_block(fn);
    Operation* const zero = konst(ctx, body, 0, ctx.type_i32());
    Operation* const one  = konst(ctx, body, 1, ctx.type_i32());
    Operation* const two  = konst(ctx, body, 2, ctx.type_i32());
    QuantityDim      length;
    length.exp[0] = 1;
    (void)konst(ctx, body, 7, ctx.type_quantity(ctx.type_i32(), length));
    Operation* const a       = binop(ctx, body, "addi", one->result(0U), two->result(0U));
    Operation* const s       = binop(ctx, body, "addi", a->result(0U), a->result(0U));
    Value*           args[1] = {s->result(0U)};
    Operation* const t = func::create_call(ctx, "scale", ConstSpan<Value*>(args, 1U), 1U, ctx.type_i32());
    body->append(t);
    Value*           range[3] = {zero->result(0U), body->arg(0U), one->result(0U)};
    Operation* const loop =
        ctx.create_operation(ctx.intern_op("core", "for"), ConstSpan<Value*>(range, 3U), 0U, {}, 1U);
    Block* const loop_body = ctx.create_block(1U, ctx.type_i32());
    loop->region(0)->append(loop_body);
    (void)binop(ctx, loop_body, "addi", loop_body->arg(0U), s->result(0U));
    loop_body->append(ctx.create_operation(ctx.intern_op("core", "yield"), {}, 0U));
    body->append(loop);
    Value* rv[1] = {t->result(0U)};
    body->append(func::create_return(ctx, ConstSpan<Value*>(rv, 1U)));
    return m;
}

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

String read_asset(crd::memory::IAllocator* alloc)
{
    String        out(alloc);
    std::ifstream f(kAsset, std::ios::binary);
    REQUIRE(f.good());
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
    {
        out.append(StringView(buf, static_cast<usize>(f.gcount())));
    }
    return out;
}

// The 1-based line holding the n-th occurrence of `needle` (the independent oracle).
u32 line_of(StringView text, const char* needle, u32 nth)
{
    const usize nlen = StringView(needle).size();
    u32         line = 1U;
    u32         seen = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle) && seen++ == nth)
        {
            return line;
        }
    }
    return 0U;
}

// `text` with its first `from` replaced by `to` (the test's own edit of the authored source).
String replace_first(StringView text, StringView from, StringView to, crd::memory::IAllocator* alloc)
{
    String out(alloc);
    usize  i    = 0U;
    bool   done = false;
    while (i < text.size())
    {
        if (!done && i + from.size() <= text.size() && StringView(text.data() + i, from.size()) == from)
        {
            out.append(to);
            i += from.size();
            done = true;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    REQUIRE(done);
    return out;
}

bool contains(const String& s, StringView n)
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

// The authored lines the tests name, scanned from the committed text.
struct Lines
{
    u32 h    = 0U; // muli in @scale
    u32 two  = 0U; // the third const of @main
    u32 len  = 0U; // the quantity const
    u32 a    = 0U;
    u32 s    = 0U;
    u32 call = 0U;
    u32 loop = 0U;
    u32 x    = 0U; // the loop body's addi
    explicit Lines(StringView t)
        : h(line_of(t, "arith.muli", 0U)), two(line_of(t, "arith.const", 2U)), len(line_of(t, "qty<", 0U)),
          a(line_of(t, "arith.addi", 0U)), s(line_of(t, "arith.addi", 1U)), call(line_of(t, "func.call", 0U)),
          loop(line_of(t, "core.for", 0U)), x(line_of(t, "arith.addi", 2U))
    {
        REQUIRE(h != 0U);
        REQUIRE(two != 0U);
        REQUIRE(len != 0U);
        REQUIRE(a != 0U);
        REQUIRE(s != 0U);
        REQUIRE(call != 0U);
        REQUIRE(loop != 0U);
        REQUIRE(x != 0U);
    }
};

insp::ValueSnapshot read(InspectHost& host, u64 gen, u32 line, crd::memory::IAllocator* alloc)
{
    insp::ValueSnapshot v(alloc);
    REQUIRE(host.session().snapshot(gen, insp::ValueRef{host.op_at_line(kFile, line), 0U}, v, kWaitMs) ==
            insp::Refusal::None);
    return v;
}

u32 stop_line(const InspectHost& host, const insp::StopRecord& stop)
{
    const Origin* const o = host.stop_origin(stop);
    return (o != nullptr) ? o->loc.line : 0U;
}
} // namespace

TEST_CASE("diag 8b: the committed inspect demo is the builder's printed program", "[ceir][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = read_asset(&root);
    Context                            built(&root);
    register_dialects(built);
    const String expected = print(built, *build_program(built), &root);
    Context      parsed(&root);
    register_dialects(parsed);
    const ParseResult pr = parse(parsed, sv(text), parsed.register_file(kFile));
    REQUIRE(pr.ok);
    CHECK(sv(print(parsed, *pr.module, &root)) == sv(expected));
}

TEST_CASE("diag 8b: the inspect host stops, snapshots and steps an authored program", "[ceir][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::memory::GrowableTlsfAllocator ctl;
    const String                       text = read_asset(&root);
    const Lines                        ln(sv(text));

    InspectHost host(&root, &registrar, nullptr);
    const i64   n = 3;
    CHECK(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::NotBound); // nothing loaded yet
    const HostLoadResult lr = host.load(kId, sv(text), StringView(kFile), StringView("main"));
    REQUIRE(lr.ok());
    const u64 gen = lr.generation;
    REQUIRE(gen != 0U);
    CHECK(host.generation() == gen);
    u32 bp = 0U;
    REQUIRE(host.add_line_breakpoint(kFile, ln.call, bp) == insp::Refusal::None);
    REQUIRE(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(host.binds().size() == 1U);
    CHECK(host.binds()[bp].status == insp::BindStatus::Bound);
    CHECK(host.binds()[bp].first_op == host.op_at_line(kFile, ln.call));

    insp::StopRecord stop;
    REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.reason == insp::StopReason::Breakpoint);
    CHECK(stop.breakpoint == bp);
    CHECK(stop.depth == 0U);
    CHECK(stop_line(host, stop) == ln.call);
    CHECK(host.load(kId, sv(text), StringView(kFile), StringView("main")).status == HostLoad::Busy);
    CHECK(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::Busy);

    const insp::ValueSnapshot a = read(host, gen, ln.a, &ctl);
    CHECK(a.status == insp::ValueStatus::Available);
    CHECK(a.bits == 3);
    CHECK(contains(a.type_text, "i32"));
    CHECK_FALSE(a.has_unit);
    const insp::ValueSnapshot len = read(host, gen, ln.len, &ctl);
    CHECK(len.status == insp::ValueStatus::Available);
    CHECK(len.bits == 7);
    CHECK(len.has_unit);
    CHECK(len.unit.exp[0] == 1);
    CHECK(read(host, gen, ln.call, &ctl).status == insp::ValueStatus::NotYetComputed); // the op at the stop
    CHECK(read(host, gen, ln.x, &ctl).status == insp::ValueStatus::OutOfScope);        // the loop body's value

    REQUIRE(host.session().resume(gen, insp::Resume::StepInto) == insp::Refusal::None);
    REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.reason == insp::StopReason::Step);
    CHECK(stop.depth == 1U);
    CHECK(stop_line(host, stop) == ln.h);
    CHECK(read(host, gen, ln.h, &ctl).status == insp::ValueStatus::NotYetComputed);
    CHECK(read(host, gen, ln.a, &ctl).status == insp::ValueStatus::OutOfScope); // the caller's frame

    REQUIRE(host.session().resume(gen, insp::Resume::StepOut) == insp::Refusal::None);
    REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.depth == 0U);
    CHECK(stop_line(host, stop) == ln.loop);
    const insp::ValueSnapshot t = read(host, gen, ln.call, &ctl);
    CHECK(t.status == insp::ValueStatus::Available);
    CHECK(t.bits == 36);

    REQUIRE(host.session().resume(gen, insp::Resume::Continue) == insp::Refusal::None);
    CHECK(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::Finished);
    REQUIRE(host.wait_finished(kWaitMs));
    CHECK_FALSE(host.running());
    REQUIRE(host.result().ok());
    REQUIRE(host.result().values.size() == 1U);
    CHECK(host.result().values[0] == 36);

    // The same generation runs again: the rebind at start resets the finished session, so the next wait sees this run.
    REQUIRE(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(host.session().wait_for_stop(gen, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.breakpoint == bp);
    REQUIRE(host.session().cancel(gen) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    CHECK(host.result().error == plan::RunError::Cancelled);
}

TEST_CASE("diag 8b: the inspect host reloads, refuses the replaced generation and keeps the last good one",
          "[ceir][inspect][reload][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    crd::memory::GrowableTlsfAllocator ctl;
    const String                       text = read_asset(&root);
    const Lines                        ln(sv(text));
    InspectHost                        host(&root, &registrar, nullptr);
    const HostLoadResult               first = host.load(kId, sv(text), StringView(kFile), StringView("main"));
    REQUIRE(first.ok());
    const u64 gen1 = first.generation;

    // A body edit (the third const 2 -> 4) hot-swaps a new generation.
    const String edited = replace_first(sv(text), "{value = 2}", "{value = 4}", &root);
    const HostLoadResult swap = host.load(kId, sv(edited), StringView(kFile), StringView("main"));
    REQUIRE(swap.ok());
    CHECK(swap.decision == ReloadDecision::HotSwap);
    const u64 gen2 = swap.generation;
    REQUIRE(gen2 != gen1);
    insp::StopRecord stop;
    CHECK(host.session().wait_for_stop(gen1, 1U, stop) == insp::Refusal::StaleGeneration);
    CHECK(host.session().request_pause(gen1) == insp::Refusal::StaleGeneration);

    u32 bp = 0U;
    REQUIRE(host.add_line_breakpoint(kFile, ln.s, bp) == insp::Refusal::None);
    const i64 n = 2;
    REQUIRE(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(host.session().wait_for_stop(gen2, kWaitMs, stop) == insp::Refusal::None);
    CHECK(stop.generation == gen2);
    CHECK(stop_line(host, stop) == ln.s);
    insp::ValueSnapshot old(&ctl);
    CHECK(host.session().snapshot(gen1, insp::ValueRef{host.op_at_line(kFile, ln.two), 0U}, old, kWaitMs) ==
          insp::Refusal::StaleGeneration);
    CHECK(host.session().resume(gen1, insp::Resume::Continue) == insp::Refusal::StaleGeneration);
    CHECK(host.session().cancel(gen1) == insp::Refusal::StaleGeneration);
    CHECK(read(host, gen2, ln.two, &ctl).bits == 4); // generation 2's value, read from generation 2's frame
    REQUIRE(host.session().resume(gen2, insp::Resume::Continue) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    REQUIRE(host.result().ok());
    CHECK(host.result().values[0] == 100); // ((1 + 4) * 2)^2

    // A contract change (the exported entry is renamed) is rejected: the last good generation and its plan stay
    // installed and run.
    const String renamed = replace_first(sv(edited), "sym_name = \"main\"", "sym_name = \"entry\"", &root);
    const HostLoadResult rejected = host.load(kId, sv(renamed), StringView(kFile), StringView("entry"));
    CHECK(rejected.status == HostLoad::Rejected);
    CHECK(rejected.decision == ReloadDecision::ContractChange);
    CHECK(rejected.generation == gen2);
    CHECK(host.generation() == gen2);
    REQUIRE(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
    REQUIRE(host.session().wait_for_stop(gen2, kWaitMs, stop) == insp::Refusal::None);
    REQUIRE(host.session().resume(gen2, insp::Resume::Continue) == insp::Refusal::None);
    REQUIRE(host.wait_finished(kWaitMs));
    CHECK(host.result().values[0] == 100);

    // A source that does not parse names its line; nothing changes.
    const String broken = replace_first(sv(edited), "arith.muli", "arith.muli(", &root);
    const HostLoadResult bad = host.load(kId, sv(broken), StringView(kFile), StringView("main"));
    CHECK(bad.status == HostLoad::CookFailed);
    CHECK(bad.cook_site.line == ln.h);
    CHECK(host.generation() == gen2);
}

TEST_CASE("diag 8b: destroying the inspect host cancels and joins a paused execution", "[ceir][inspect][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = read_asset(&root);
    const Lines                        ln(sv(text));
    {
        InspectHost host(&root, &registrar, nullptr);
        REQUIRE(host.load(kId, sv(text), StringView(kFile), StringView("main")).ok());
        u32 bp = 0U;
        REQUIRE(host.add_line_breakpoint(kFile, ln.a, bp) == insp::Refusal::None);
        const i64 n = 1;
        REQUIRE(host.start(ConstSpan<i64>(&n, 1U)) == insp::Refusal::None);
        insp::StopRecord stop;
        REQUIRE(host.session().wait_for_stop(host.generation(), kWaitMs, stop) == insp::Refusal::None);
        CHECK(host.running());
    } // the destructor cancels the paused execution and joins its thread: reaching here is the check
    SUCCEED();
}
