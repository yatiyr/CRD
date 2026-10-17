// DIAG.10c: the wrong-but-noncrashing result oracle at the CEIR boundary (crd/ceir/cook/result_oracle.hpp). A program
// whose edit makes it return a wrong value runs to completion with no error; an independent reference (closed-form
// C++) or an invariant (an integer domain) catches the value, and the finding names the generation that produced it,
// current or retiring or gone, and the op that defined the result in that generation's own file. The correct
// generation is never blamed. Expected positions come from scanning each text, never from the parser. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/result_oracle.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/core/value_domain.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::i64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kFileA = "programs/diag/oracle_a.ceir";
constexpr const char* kFileB = "programs/diag/oracle_b.ceir";

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

// The specification of @main(%x): (x + 1, x * x). A variant may get either wrong without any error:
// @main(%x) { %k = const `k`; %a = addi(%x, %k); %b = muli(%x, `square` ? %x : %k); return %a, %b[, %x] }.
// `echo` returns the parameter itself as a third result (a result no op defines).
struct Variant
{
    i64  k      = 1;
    bool square = true;
    bool echo   = false;
};

Module* build_main(Context& c, Variant v)
{
    Module* const    m  = c.create_module();
    Operation* const fn = func::create_func(c, *m, "main", Visibility::Public, 1U, c.type_i32());
    REQUIRE(fn != nullptr);
    m->body()->append(c.create_block(0U));
    m->body()->first_block()->append(fn);
    Block* const     body = &func::func_body(*fn);
    Operation* const k    = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_i32());
    c.set_attr(k, "value", c.attr_int(v.k));
    body->append(k);
    Value*           xa[2] = {body->arg(0U), k->result(0U)};
    Operation* const a = c.create_operation(c.intern_op("arith", "addi"), ConstSpan<Value*>(xa, 2U), 1U, c.type_i32());
    body->append(a);
    Value*           xb[2] = {body->arg(0U), v.square ? body->arg(0U) : k->result(0U)};
    Operation* const b = c.create_operation(c.intern_op("arith", "muli"), ConstSpan<Value*>(xb, 2U), 1U, c.type_i32());
    body->append(b);
    Value* rv[3] = {a->result(0U), b->result(0U), body->arg(0U)};
    body->append(func::create_return(c, ConstSpan<Value*>(rv, v.echo ? 3U : 2U)));
    return m;
}

String source_main(crd::memory::GrowableTlsfAllocator& root, Variant v)
{
    Context c(&root);
    register_dialects(c);
    return print(c, *build_main(c, v), &root);
}

StringView sv(const String& s)
{
    return StringView(s.data(), s.size());
}

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

// "<file>:<line>:<col>" of the first line holding `needle`: its line, and the column of its first non-blank
// character (where the op starts). The independent oracle for `where`.
String position_of(StringView text, const char* needle, const char* file, crd::memory::IAllocator* alloc)
{
    const usize nlen       = StringView(needle).size();
    u32         line       = 1U;
    usize       line_start = 0U;
    u32         col        = 0U;
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
            col = static_cast<u32>(first - line_start + 1U);
            break;
        }
    }
    REQUIRE(col != 0U);
    String s(alloc);
    s.append(file);
    const u32 parts[2] = {line, col};
    for (const u32 v : parts)
    {
        s.push_back(':');
        char  buf[10];
        usize n = 0U;
        u32   r = v;
        do
        {
            buf[n++] = static_cast<char>('0' + (r % 10U));
            r /= 10U;
        } while (r != 0U);
        while (n > 0U)
        {
            s.push_back(buf[--n]);
        }
    }
    return s;
}

// The specification, computed in C++ (wrapping like the executors).
struct Reference
{
    i64 v[2] = {0, 0};
    explicit Reference(i64 x)
    {
        v[0] = static_cast<i64>(static_cast<u64>(x) + 1U);
        v[1] = static_cast<i64>(static_cast<u64>(x) * static_cast<u64>(x));
    }
    [[nodiscard]] ResultExpectation expectation() const
    {
        ResultExpectation e{};
        e.reference     = ConstSpan<i64>(v, 2U);
        e.has_reference = true;
        return e;
    }
};

// Run @main(x) on the reference interpreter over a generation; the run must succeed (the result is wrong, not failed).
crd::containers::Array<i64> interpret(Generation& g, i64 x, crd::memory::IAllocator* alloc)
{
    exec::Interpreter in(*g.ctx);
    exec::install_builtin_semantics(in);
    const i64              args[1] = {x};
    const exec::ExecResult er      = in.invoke(*g.program.module, "main", ConstSpan<i64>(args, 1U));
    REQUIRE(er.ok());
    crd::containers::Array<i64> out(alloc);
    for (usize i = 0; i < er.values.size(); ++i)
    {
        out.push_back(er.values[i]);
    }
    return out;
}

ConstSpan<i64> span(const crd::containers::Array<i64>& a)
{
    return ConstSpan<i64>(a.data(), a.size());
}

constexpr i64 kInputs[] = {0, 3, -5, 1000, 2};
} // namespace

TEST_CASE("diag10c: a hot-swapped generation's wrong result is caught by a reference and blamed on it",
          "[ceir][reload][diag10c]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{5100U};
    const String                       good = source_main(root, Variant{});
    Variant                            off_by_one{};
    off_by_one.k       = 2;
    const String wrong = source_main(root, off_by_one);
    REQUIRE(set.add_source(id, sv(good), StringView(kFileA)).ok());
    const ProgramHandle h1 = set.handle(id);
    Generation* const   g1 = set.generation(id);
    REQUIRE(g1 != nullptr);

    // Generation 1 meets the specification on every input: nothing is blamed.
    for (const i64 x : kInputs)
    {
        const auto          values = interpret(*g1, x, &root);
        const ResultFinding f      = check_program_results(set, h1, "main", span(values), Reference(x).expectation(),
                                                           &root);
        CHECK(f.check.verdict == ResultVerdict::Agrees);
        CHECK_FALSE(f.check.wrong());
        CHECK(f.site.asset.value == 0U);
        CHECK(f.site.where.empty());
        const String r = render_result_finding(f, &root);
        CHECK(StringView(r.data(), r.size()) == StringView("agrees"));
    }
    // A plan built from generation 1 keeps running after the swap.
    const plan::CompileResult p1 = plan::compile(*g1->ctx, *g1->program.module, "main", &root);
    REQUIRE(p1.ok());

    // The edit cooks, keeps the contract and hot-swaps: generation 2 returns x + 2 with no error anywhere.
    const ReloadResult rr = set.reload_source(id, sv(wrong), StringView(kFileB));
    CHECK(rr.decision == ReloadDecision::HotSwap);
    REQUIRE(rr.installed);
    const ProgramHandle h2 = set.handle(id);
    Generation* const   g2 = set.generation(id);
    REQUIRE(g2 != nullptr);
    REQUIRE(h2.generation.value == 2U);
    const String addi_b = position_of(sv(wrong), "arith.addi", kFileB, &root);

    const auto          values = interpret(*g2, 3, &root);
    const ResultFinding f =
        check_program_results(set, h2, "main", span(values), Reference(3).expectation(), &root);
    CHECK(f.check.verdict == ResultVerdict::DiffersFromReference);
    CHECK(f.check.index == 0U);
    CHECK(f.check.value == 5);
    CHECK(f.check.expected == 4);
    CHECK(f.site.asset == id);
    CHECK(f.site.generation.value == 2U);
    CHECK(f.site.state == GenerationState::Current);
    CHECK(f.site.content_hash == g2->program.content_hash);
    CHECK(has(f.site.where, sv(addi_b)));
    const String r = render_result_finding(f, &root);
    CHECK(has(r, StringView("differs-from-reference result#0 = 5, expected 4 at asset#5100 gen#2 current ")));
    CHECK(has(r, sv(addi_b)));

    // The old plan still runs generation 1 and still agrees: the swap is not blamed on the generation before it.
    const i64             three[1] = {3};
    const plan::RunResult old      = plan::run(p1.plan, ConstSpan<i64>(three, 1U), &root);
    REQUIRE(old.ok());
    const ConstSpan<i64> old_values(old.values.data(), old.values.size());
    const ResultFinding  kept = check_program_results(set, h1, "main", old_values, Reference(3).expectation(), &root);
    CHECK(kept.check.verdict == ResultVerdict::Agrees);
}

TEST_CASE("diag10c: a plan built from a wrong generation is blamed on that generation after the fix",
          "[ceir][reload][diag10c]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{5200U};
    Variant                            not_squared{};
    not_squared.square = false; // returns x * 1
    const String wrong = source_main(root, not_squared);
    const String fixed = source_main(root, Variant{});
    REQUIRE(set.add_source(id, sv(wrong), StringView(kFileA)).ok());
    const ProgramHandle h1     = set.handle(id);
    Generation* const   g1     = set.generation(id);
    REQUIRE(g1 != nullptr);
    const u64           hash_1 = g1->program.content_hash;
    const String        muli_a = position_of(sv(wrong), "arith.muli", kFileA, &root);
    const plan::CompileResult p1 = plan::compile(*g1->ctx, *g1->program.module, "main", &root);
    REQUIRE(p1.ok());

    const ReloadResult rr = set.reload_source(id, sv(fixed), StringView(kFileB));
    REQUIRE(rr.installed);
    const ProgramHandle h2 = set.handle(id);
    Generation* const   g2 = set.generation(id);
    REQUIRE(g2 != nullptr);

    // The fixed generation agrees on every input, on both executors.
    for (const i64 x : kInputs)
    {
        const auto values = interpret(*g2, x, &root);
        CHECK(check_program_results(set, h2, "main", span(values), Reference(x).expectation(), &root).check.verdict ==
              ResultVerdict::Agrees);
    }

    // The old plan runs to completion and returns x for x * x: blamed on generation 1, retiring, in file A.
    const i64             three[1] = {3};
    const plan::RunResult old      = plan::run(p1.plan, ConstSpan<i64>(three, 1U), &root);
    REQUIRE(old.ok());
    REQUIRE(old.values.size() == 2U);
    const ConstSpan<i64> got(old.values.data(), old.values.size());
    const ResultFinding  f = check_program_results(set, h1, "main", got, Reference(3).expectation(), &root);
    CHECK(f.check.verdict == ResultVerdict::DiffersFromReference);
    CHECK(f.check.index == 1U);
    CHECK(f.check.value == 3);
    CHECK(f.check.expected == 9);
    CHECK(f.site.generation.value == 1U);
    CHECK(f.site.state == GenerationState::Retiring);
    CHECK(f.site.content_hash == hash_1);
    CHECK(has(f.site.where, sv(muli_a)));
    CHECK_FALSE(has(f.site.where, StringView(kFileB)));
    CHECK(has(render_result_finding(f, &root), StringView("result#1 = 3, expected 9 at asset#5200 gen#1 retiring ")));

    // On an input where the wrong generation happens to agree (x = 1), nothing is blamed.
    const i64             one[1] = {1};
    const plan::RunResult lucky  = plan::run(p1.plan, ConstSpan<i64>(one, 1U), &root);
    REQUIRE(lucky.ok());
    CHECK(check_program_results(set, h1, "main", ConstSpan<i64>(lucky.values.data(), lucky.values.size()),
                                Reference(1).expectation(), &root)
              .check.verdict == ResultVerdict::Agrees);

    // Once drained, generation 1 is named by number alone: its Context, and with it the file, is gone.
    set.drain();
    const ResultFinding gone = check_program_results(set, h1, "main", got, Reference(3).expectation(), &root);
    CHECK(gone.check.verdict == ResultVerdict::DiffersFromReference);
    CHECK(gone.site.generation.value == 1U);
    CHECK(gone.site.state == GenerationState::Gone);
    CHECK(gone.site.content_hash == 0U);
    CHECK(gone.site.where.empty());
    const String r = render_result_finding(gone, &root);
    CHECK(StringView(r.data(), r.size()) ==
          StringView("differs-from-reference result#1 = 3, expected 9 at asset#5200 gen#1 gone"));
}

TEST_CASE("diag10c: an invariant catches a wrapped result without any reference", "[ceir][reload][diag10c]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{5300U};
    const String                       text = source_main(root, Variant{});
    REQUIRE(set.add_source(id, sv(text), StringView(kFileA)).ok());
    const ProgramHandle h    = set.handle(id);
    Generation* const   g    = set.generation(id);
    REQUIRE(g != nullptr);
    const String        muli = position_of(sv(text), "arith.muli", kFileA, &root);

    // For x >= 0 both results are non-negative: x + 1 and x * x. Large x wraps x * x past 2^63 to a negative value.
    ResultExpectation e{};
    e.domain.name    = "non-negative";
    e.domain.minimum = 0;
    const i64 small  = 3037000499; // floor(sqrt(2^63 - 1)): its square fits
    const i64 large  = 3037000500; // its square wraps
    CHECK(check_program_results(set, h, "main", span(interpret(*g, small, &root)), e, &root).check.verdict ==
          ResultVerdict::Agrees);

    const auto values = interpret(*g, large, &root);
    REQUIRE(values.size() == 2U);
    CHECK(values[1] < 0); // the program ran to completion and returned it
    const ResultFinding f = check_program_results(set, h, "main", span(values), e, &root);
    CHECK(f.check.verdict == ResultVerdict::OutOfDomain);
    CHECK(f.check.index == 1U);
    CHECK(f.check.value == values[1]);
    CHECK(f.check.violations == static_cast<u32>(crd::ValueViolation::BelowMinimum));
    CHECK(f.site.state == GenerationState::Current);
    CHECK(has(f.site.where, sv(muli)));
    CHECK(has(render_result_finding(f, &root), StringView("[below-minimum] at asset#5300 gen#1 current ")));

    // The compiled plan wraps the same way and is judged the same way.
    const plan::CompileResult p = plan::compile(*g->ctx, *g->program.module, "main", &root);
    REQUIRE(p.ok());
    const i64             args[1] = {large};
    const plan::RunResult run     = plan::run(p.plan, ConstSpan<i64>(args, 1U), &root);
    REQUIRE(run.ok());
    const ResultFinding pf =
        check_program_results(set, h, "main", ConstSpan<i64>(run.values.data(), run.values.size()), e, &root);
    CHECK(pf.check.verdict == ResultVerdict::OutOfDomain);
    CHECK(pf.check.index == 1U);

    // The domain is checked before the reference at each index: a value both out of the domain and different from
    // the reference is OutOfDomain; above the maximum is named as such.
    const Reference   spec_large(large);
    ResultExpectation both = spec_large.expectation();
    both.domain.minimum    = 0;
    CHECK(check_results(span(values), both).verdict == ResultVerdict::OutOfDomain);
    ResultExpectation capped{};
    capped.domain.maximum = 3;
    const auto          four  = interpret(*g, 3, &root); // (4, 9)
    const ResultCheck   above = check_results(span(four), capped);
    CHECK(above.verdict == ResultVerdict::OutOfDomain);
    CHECK(above.index == 0U);
    CHECK(above.violations == static_cast<u32>(crd::ValueViolation::AboveMaximum));
}

TEST_CASE("diag10c: count mismatches, invalid domains and results no op defines", "[ceir][reload][diag10c]")
{
    crd::memory::GrowableTlsfAllocator root;
    ReloadSet                          set(&root, &registrar, nullptr);
    const AssetId                      id{5400U};
    Variant                            echo{};
    echo.echo         = true;
    const String text = source_main(root, echo);
    REQUIRE(set.add_source(id, sv(text), StringView(kFileA)).ok());
    const ProgramHandle h      = set.handle(id);
    Generation* const   g      = set.generation(id);
    REQUIRE(g != nullptr);
    const String        ret_at = position_of(sv(text), "func.return", kFileA, &root);
    const auto          values = interpret(*g, 3, &root); // (4, 9, 3)
    REQUIRE(values.size() == 3U);

    SECTION("three results against a two-value reference: the count, blamed at the third result's return")
    {
        const ResultFinding f = check_program_results(set, h, "main", span(values), Reference(3).expectation(), &root);
        CHECK(f.check.verdict == ResultVerdict::CountMismatch);
        CHECK(f.check.index == 2U);
        CHECK(f.check.value == 3);
        CHECK(f.check.expected == 0);
        CHECK(f.check.count == 3U);
        CHECK(f.check.expected_count == 2U);
        CHECK(has(f.site.where, sv(ret_at))); // the parameter has no defining op: the return names it
        CHECK(has(render_result_finding(f, &root),
                  StringView("count-mismatch result#2 = 3, expected 0 (3 results, expected 2) at asset#5400 gen#1 ")));
    }

    SECTION("two results against a three-value reference: blamed at the return, past its operands")
    {
        const i64         three[3] = {4, 9, 3};
        ResultExpectation e{};
        e.reference     = ConstSpan<i64>(three, 3U);
        e.has_reference = true;
        const ResultCheck c = check_results(ConstSpan<i64>(values.data(), 2U), e);
        CHECK(c.verdict == ResultVerdict::CountMismatch);
        CHECK(c.index == 2U);
        CHECK(c.value == 0);
        CHECK(c.expected == 3);
        CHECK(check_results(span(values), e).verdict == ResultVerdict::Agrees);
        const GenerationSite past = set.locate_result(h, "main", 7U, &root);
        CHECK(has(past.where, sv(ret_at)));
    }

    SECTION("an invalid domain judges nothing and blames nothing")
    {
        const Reference   spec(3);
        ResultExpectation e = spec.expectation();
        e.domain.minimum    = 10;
        e.domain.maximum    = 9;
        const ResultFinding f = check_program_results(set, h, "main", span(values), e, &root);
        CHECK(f.check.verdict == ResultVerdict::InvalidDomain);
        CHECK(f.check.wrong());
        CHECK(f.site.asset.value == 0U);
        const String r = render_result_finding(f, &root);
        CHECK(StringView(r.data(), r.size()) == StringView("invalid-domain"));
    }

    SECTION("an entry the generation does not define names the generation and no op")
    {
        const GenerationSite s = set.locate_result(h, "nosuch", 0U, &root);
        CHECK(s.generation.value == 1U);
        CHECK(s.state == GenerationState::Current);
        CHECK_FALSE(s.where.empty());
        CHECK_FALSE(has(s.where, StringView(kFileA)));
        // A handle the set never minted is not attributed to this program.
        CHECK(set.locate_result(ProgramHandle{}, "main", 0U, &root).state == GenerationState::Gone);
    }

    SECTION("every verdict has its own name")
    {
        const ResultVerdict all[] = {ResultVerdict::Agrees, ResultVerdict::InvalidDomain, ResultVerdict::CountMismatch,
                                     ResultVerdict::OutOfDomain, ResultVerdict::DiffersFromReference};
        for (usize i = 0; i < 5U; ++i)
        {
            CHECK(result_verdict_name(all[i]) != StringView("?"));
            for (usize j = 0; j < i; ++j)
            {
                CHECK(result_verdict_name(all[i]) != result_verdict_name(all[j]));
            }
        }
    }
}
