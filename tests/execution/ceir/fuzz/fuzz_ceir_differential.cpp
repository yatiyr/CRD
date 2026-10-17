// crd-fuzz-ceir-differential: the compiled execution tier (crd::ceir::plan) against the reference interpreter
// (crd::ceir::exec) on generated programs, under the bounded fuzz harness (DIAG.10a; docs/design/test-instruments.md).
//
// Input. Every input decodes to one program, never to a parse failure, so mutations reach the executors instead of a
// loader: a header (function count, the step budget, the host input script, each function's parameter and result
// counts, the entry arguments) and then each function's body as a stream of op choices over arith, core control flow
// and §20 cells, func.call (recursion included), async and task tokens, data-parallel bodies and the input ops. The
// generator keeps a program inside the module-validity set a cook enforces (dominance and isolation, the core yield
// counts, each token consumed exactly once); a program the verifiers still refuse is out of contract and not run.
//
// Oracles, none of them either executor's own verdict:
//   * the reference and the compiled plan agree on the run: its error class (the documented mapping between ExecError
//     and RunError), the returned values, every §20 cell, every map output (on success) and, on an error, the op the
//     error blames by stable id; a reference error with no compiled counterpart is a finding, as is a compile refusal
//     outside the documented tier differences (only a stateful or input-reading parallel body, refused whole);
//   * metamorphic twins: the plan run twice, a fresh reference session, and the program printed and parsed again and
//     serialized and loaded again (both compiled and run) give the same outcome;
//   * both executors return a typed result within the step budget: a crash, a hang or an allocation past the
//     harness budget is a finding.
#include <crd/fuzz/harness.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/time.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>

#include <cstdio>
#include <initializer_list>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Block;
using crd::ceir::Context;
using crd::ceir::Module;
using crd::ceir::OpId;
using crd::ceir::Operation;
using crd::ceir::Value;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::HashMap;
using crd::containers::String;
using crd::containers::StringView;
namespace exec = crd::ceir::exec;
namespace plan = crd::ceir::plan;

// ---- generator bounds -----------------------------------------------------------------------------------------------

constexpr u32 kMaxFunctions = 4U;   // functions f0 (the entry) .. f3
constexpr u32 kMaxOps       = 192U; // ops one program may hold, terminators included
constexpr u32 kMaxDepth     = 4U;   // region nesting below a function body
constexpr u32 kMaxScript    = 15U;  // host input values a script holds (16 selects no input source at all)
constexpr u64 kFuelBase     = 64U;  // the step budget is kFuelBase << (header byte % 12): 64 .. 131,072

// The op choices of a body stream (a byte modulo OpCount).
enum Code : u8
{
    OpConst = 0,
    OpAdd,
    OpMul,
    OpCmp,
    OpIf,
    OpFor,
    OpWhile,
    OpSwitch,
    OpScope,
    OpState,
    OpCall,
    OpLaunch,
    OpAwait,
    OpJoin,
    OpRace,
    OpCancel,
    OpContinuation,
    OpParallelFor,
    OpMapReduce,
    OpRandom,
    OpTime,
    OpEvent,
    OpCount,
};

// Edge values a value byte with the top bit set selects (index = byte & 0x7F); 0xFF is followed by eight raw bytes.
constexpr i64 kEdges[] = {
    INT64_MIN,
    INT64_MAX,
    INT64_MIN + 1,
    INT64_MAX - 1,
    i64{1} << 31U,
    -(i64{1} << 31U),
    i64{1} << 32U,
    (i64{1} << 32U) - 1,
    (i64{1} << 32U) + 1,
    i64{1} << 62U,
    -(i64{1} << 62U),
    INT64_MAX / 2,
};
constexpr u8 kEdgeCount = static_cast<u8>(sizeof(kEdges) / sizeof(kEdges[0]));
constexpr u8 kRawValue  = 0xFFU;

// ---- input decoding -------------------------------------------------------------------------------------------------

class Reader
{
public:
    Reader(const u8* data, usize size) noexcept : m_data(data), m_size(size) {}

    // The next byte, or 0 past the end (a short input still decodes to a program).
    [[nodiscard]] u8 byte() noexcept
    {
        return m_at < m_size ? m_data[m_at++] : u8{0};
    }

    // A small value [-64, 63] from a byte without the top bit; an edge value or eight raw bytes otherwise.
    [[nodiscard]] i64 value() noexcept
    {
        const u8 b = byte();
        if ((b & 0x80U) == 0U)
        {
            return static_cast<i64>(b) - 64;
        }
        if (b == kRawValue)
        {
            u64 v = 0U;
            for (u32 i = 0U; i < 8U; ++i)
            {
                v |= static_cast<u64>(byte()) << (8U * i);
            }
            return static_cast<i64>(v);
        }
        return kEdges[(b & 0x7FU) % kEdgeCount];
    }

private:
    const u8* m_data;
    usize     m_size;
    usize     m_at = 0U;
};

// The op kinds, interned once per Context, with every dialect a generated program uses registered.
struct Kinds
{
    OpId cst, addi, muli, cmpi;
    OpId cif, cfor, cwhile, cswitch, cmatch, cscope, cyield, state, delay, history;
    OpId alaunch, aawait, ajoin, arace, acancel, ascope;
    OpId tspawn, tmain, tworker, tgroup, tfiber, tcont, pfor, mreduce;
    OpId irandom, iclock, istep, ievent;
    explicit Kinds(Context& ctx)
        : cst(ctx.intern_op("arith", "const")), addi(ctx.intern_op("arith", "addi")),
          muli(ctx.intern_op("arith", "muli")), cmpi(ctx.intern_op("arith", "cmpi")),
          cif(ctx.intern_op("core", "if")), cfor(ctx.intern_op("core", "for")),
          cwhile(ctx.intern_op("core", "while")), cswitch(ctx.intern_op("core", "switch")),
          cmatch(ctx.intern_op("core", "match")), cscope(ctx.intern_op("core", "scope")),
          cyield(ctx.intern_op("core", "yield")), state(ctx.intern_op("core", "state")),
          delay(ctx.intern_op("core", "delay")), history(ctx.intern_op("core", "history")),
          alaunch(ctx.intern_op("async", "launch")), aawait(ctx.intern_op("async", "await")),
          ajoin(ctx.intern_op("async", "join")), arace(ctx.intern_op("async", "race")),
          acancel(ctx.intern_op("async", "cancel")), ascope(ctx.intern_op("async", "scope")),
          tspawn(ctx.intern_op("task", "spawn")), tmain(ctx.intern_op("task", "main_thread")),
          tworker(ctx.intern_op("task", "worker")), tgroup(ctx.intern_op("task", "group")),
          tfiber(ctx.intern_op("task", "fiber_wait")), tcont(ctx.intern_op("task", "continuation")),
          pfor(ctx.intern_op("task", "parallel_for")), mreduce(ctx.intern_op("task", "map_reduce")),
          irandom(crd::ceir::input::random_kind(ctx)), iclock(crd::ceir::input::clock_kind(ctx)),
          istep(crd::ceir::input::time_step_kind(ctx)), ievent(crd::ceir::input::event_kind(ctx))
    {
        (void)crd::ceir::arith::register_arith_ops(ctx);
        (void)crd::ceir::core::register_core_ops(ctx);
        (void)crd::ceir::func::register_dialect(ctx);
        (void)crd::ceir::async::register_async_ops(ctx);
        (void)crd::ceir::task::register_task_ops(ctx);
        (void)crd::ceir::input::register_input_ops(ctx);
    }
};

// The decoded header: what the generator and both executors share.
struct Header
{
    u32        functions = 1U;
    u64        max_steps = kFuelBase;
    bool       has_source = false;
    u32        params[kMaxFunctions]  = {};
    u32        results[kMaxFunctions] = {};
    Array<i64> args;
    Array<i64> script;
    explicit Header(crd::memory::IAllocator* a) : args(a), script(a) {}
};

// A token a block produced and has not consumed yet, with the number of values its body yields.
struct Token
{
    Value* value  = nullptr;
    u32    yields = 0U;
};

// A §20 cell whose feedback operand is chosen when its block ends (it may be any value the block defines after it).
struct Feedback
{
    Operation* op   = nullptr;
    usize      from = 0U; // the scope size when the cell was created: later same-block values start here
};

class Generator
{
public:
    Generator(Context& ctx, const Kinds& k, Reader& in, const Header& h)
        : m_ctx(ctx), m_k(k), m_in(in), m_h(h), m_alloc(ctx.allocator())
    {
    }

    [[nodiscard]] Module* build()
    {
        Module* const module = m_ctx.create_module();
        Block*        top    = module->body()->first_block();
        if (top == nullptr)
        {
            top = m_ctx.create_block(0U);
            module->body()->append(top);
        }
        Block* bodies[kMaxFunctions] = {};
        for (u32 f = 0U; f < m_h.functions; ++f)
        {
            char name[4] = {'f', static_cast<char>('0' + f), '\0', '\0'};
            Operation* const fn = crd::ceir::func::create_func(m_ctx, *module, StringView(name, 2U),
                                                               crd::ceir::Visibility::Public, m_h.params[f],
                                                               m_ctx.type_i32());
            CRD_FUZZ_REQUIRE(fn != nullptr); // the names f0, f1, ... are distinct in a fresh module
            top->append(fn);
            bodies[f] = &crd::ceir::func::func_body(*fn);
        }
        for (u32 f = 0U; f < m_h.functions; ++f)
        {
            Array<Value*> scope(m_alloc);
            for (u32 a = 0U; a < bodies[f]->num_args(); ++a)
            {
                scope.push_back(bodies[f]->arg(a));
            }
            fill(bodies[f], scope, 0U, false, m_h.results[f], End::Return);
        }
        return module;
    }

private:
    enum class End : u8
    {
        Return,    // func.return with the function's result count
        Yield,     // core.yield with exactly the owner's count (always emitted)
        Statement, // no terminator (a statement region of a core op)
    };

    [[nodiscard]] bool room() const noexcept { return m_ops < kMaxOps; }

    Operation* append(Block* b, Operation* op)
    {
        b->append(op);
        ++m_ops;
        return op;
    }

    Operation* make(Block* b, OpId kind, ConstSpan<Value*> operands, u32 results, u32 regions = 0U)
    {
        return append(b, m_ctx.create_operation(kind, operands, results, results > 0U ? m_ctx.type_i32()
                                                                                      : crd::ceir::TypeId{},
                                                regions));
    }

    Value* constant(Block* b, Array<Value*>& scope, i64 v)
    {
        Operation* const c = make(b, m_k.cst, {}, 1U);
        m_ctx.set_attr(c, "value", m_ctx.attr_int(v));
        scope.push_back(c->result(0U));
        return c->result(0U);
    }

    // A visible value; a scope with none gets a constant first.
    Value* pick(Block* b, Array<Value*>& scope)
    {
        const u8 at = m_in.byte();
        if (scope.empty())
        {
            return constant(b, scope, 0);
        }
        return scope[at % scope.size()];
    }

    void publish(Operation* op, Array<Value*>& scope)
    {
        for (u32 i = 0U; i < op->num_results(); ++i)
        {
            scope.push_back(op->result(i));
        }
    }

    // A region's single block, with `args` block arguments.
    Block* region_block(Operation* op, u32 region, u32 args)
    {
        Block* const b = args > 0U ? m_ctx.create_block(args, m_ctx.type_i32()) : m_ctx.create_block(0U);
        op->region(region)->append(b);
        return b;
    }

    // Fill a child block that sees `scope` (captures allowed): its own values leave the scope again at its end.
    void child(Block* b, Array<Value*>& scope, u32 depth, bool isolated, u32 yields, End end)
    {
        const usize mark = scope.size();
        for (u32 a = 0U; a < b->num_args(); ++a)
        {
            scope.push_back(b->arg(a));
        }
        fill(b, scope, depth, isolated, yields, end);
        scope.resize(mark);
    }

    // Fill an isolated body (a data-parallel map or combine): it sees only its own block arguments.
    void isolated_body(Block* b, u32 depth)
    {
        Array<Value*> scope(m_alloc);
        for (u32 a = 0U; a < b->num_args(); ++a)
        {
            scope.push_back(b->arg(a));
        }
        fill(b, scope, depth, true, 1U, End::Yield);
    }

    void fill(Block* b, Array<Value*>& scope, u32 depth, bool isolated, u32 yields, End end)
    {
        Array<Token>    tokens(m_alloc);
        Array<Feedback> feedback(m_alloc);
        const u32       count = m_in.byte() % (depth == 0U ? 12U : 6U);
        for (u32 i = 0U; i < count && room(); ++i)
        {
            op(b, scope, tokens, feedback, depth, isolated);
        }
        // Every token this block produced is consumed exactly once: what the stream left is cancelled.
        for (usize t = 0U; t < tokens.size(); ++t)
        {
            Value* operand[1] = {tokens[t].value};
            (void)make(b, m_k.acancel, ConstSpan<Value*>(operand, 1U), 0U);
        }
        // A cell's feedback: a value this block defined after the cell (a same-block back edge) or the init.
        for (usize f = 0U; f < feedback.size(); ++f)
        {
            const u8    choice = m_in.byte();
            const usize later  = scope.size() - feedback[f].from;
            if (later > 0U && (choice & 1U) == 0U)
            {
                feedback[f].op->set_operand(1U, scope[feedback[f].from + (choice >> 1U) % later]);
            }
        }
        if (end == End::Statement)
        {
            return;
        }
        Array<Value*> values(m_alloc);
        for (u32 i = 0U; i < yields; ++i)
        {
            values.push_back(pick(b, scope));
        }
        const ConstSpan<Value*> span(values.data(), values.size());
        if (end == End::Return)
        {
            b->append(crd::ceir::func::create_return(m_ctx, span));
        }
        else
        {
            b->append(m_ctx.create_operation(m_k.cyield, span, 0U));
        }
        ++m_ops;
    }

    // Take a token this block produced: `choice` selects which.
    Token take(Array<Token>& tokens, u8 choice)
    {
        const usize at = choice % tokens.size();
        const Token t  = tokens[at];
        tokens.erase(at);
        return t;
    }

    void op(Block* b, Array<Value*>& scope, Array<Token>& tokens, Array<Feedback>& feedback, u32 depth, bool isolated)
    {
        u8 code = static_cast<u8>(m_in.byte() % OpCount);
        // Nesting and the op budget end region ops; an isolated body holds no cell and reads no host input (both
        // would be refused whole by the parallel pre-flight, which calls may still reach).
        const bool regions = depth < kMaxDepth && m_ops + 8U < kMaxOps;
        const bool nested  = code == OpIf || code == OpFor || code == OpWhile || code == OpSwitch || code == OpScope ||
                            code == OpLaunch || code == OpContinuation || code == OpParallelFor || code == OpMapReduce;
        if ((nested && !regions) ||
            (isolated && (code == OpState || code == OpRandom || code == OpTime || code == OpEvent)))
        {
            code = OpConst;
        }
        switch (code)
        {
        case OpConst: (void)constant(b, scope, m_in.value()); break;
        case OpAdd:
        case OpMul:
        case OpCmp:
        {
            Value* operands[2] = {pick(b, scope), pick(b, scope)};
            const OpId kinds[] = {m_k.addi, m_k.muli, m_k.cmpi}; // OpAdd, OpMul, OpCmp in order
            Operation* const o = make(b, kinds[code - OpAdd], ConstSpan<Value*>(operands, 2U), 1U);
            if (code == OpCmp)
            {
                static const char* const kPredicates[] = {"eq",  "ne",  "slt", "sle", "sgt",
                                                          "sge", "ult", "ule", "ugt", "uge"};
                m_ctx.set_attr(o, "predicate", m_ctx.attr_string(kPredicates[m_in.byte() % 10U]));
            }
            publish(o, scope);
            break;
        }
        case OpIf:
        {
            Value* cond[1]     = {pick(b, scope)};
            const u32 results  = m_in.byte() % 3U;
            Operation* const o = make(b, m_k.cif, ConstSpan<Value*>(cond, 1U), results, 2U);
            child(region_block(o, 0U, 0U), scope, depth + 1U, isolated, results, End::Yield);
            child(region_block(o, 1U, 0U), scope, depth + 1U, isolated, results, End::Yield);
            publish(o, scope);
            break;
        }
        case OpFor:
        case OpParallelFor:
        {
            Value* range[3]    = {pick(b, scope), pick(b, scope), pick(b, scope)};
            Operation* const o = make(b, code == OpFor ? m_k.cfor : m_k.pfor, ConstSpan<Value*>(range, 3U), 0U, 1U);
            if (code == OpFor)
            {
                child(region_block(o, 0U, 1U), scope, depth + 1U, isolated, 0U, End::Statement);
            }
            else
            {
                isolated_body(region_block(o, 0U, 1U), depth + 1U);
            }
            break;
        }
        case OpMapReduce:
        {
            Value* range[4]    = {pick(b, scope), pick(b, scope), pick(b, scope), pick(b, scope)};
            Operation* const o = make(b, m_k.mreduce, ConstSpan<Value*>(range, 4U), 1U, 2U);
            isolated_body(region_block(o, 0U, 1U), depth + 1U);
            isolated_body(region_block(o, 1U, 2U), depth + 1U);
            publish(o, scope);
            break;
        }
        case OpWhile:
        {
            Operation* const o = make(b, m_k.cwhile, {}, 0U, 2U);
            child(region_block(o, 0U, 0U), scope, depth + 1U, isolated, 1U, End::Yield);
            child(region_block(o, 1U, 0U), scope, depth + 1U, isolated, 0U, End::Statement);
            break;
        }
        case OpSwitch:
        {
            Value* selector[1] = {pick(b, scope)};
            const u8  shape    = m_in.byte();
            const u32 arms     = 1U + shape % 3U;
            Operation* const o =
                make(b, (shape & 0x10U) != 0U ? m_k.cmatch : m_k.cswitch, ConstSpan<Value*>(selector, 1U), 0U, arms);
            for (u32 r = 0U; r < arms; ++r)
            {
                child(region_block(o, r, 0U), scope, depth + 1U, isolated, 0U, End::Statement);
            }
            break;
        }
        case OpScope:
        {
            const u8  shape    = m_in.byte();
            const OpId kinds[] = {m_k.cscope, m_k.ascope, m_k.tgroup};
            const u32 results  = (shape >> 2U) % 3U;
            Operation* const o = make(b, kinds[shape % 3U], {}, results, 1U);
            child(region_block(o, 0U, 0U), scope, depth + 1U, isolated, results, End::Yield);
            publish(o, scope);
            break;
        }
        case OpState:
        {
            const u8   shape   = m_in.byte();
            const OpId kinds[] = {m_k.state, m_k.delay, m_k.history};
            Value* const init  = pick(b, scope);
            Value* operands[2] = {init, init}; // the feedback is chosen at the block's end
            Operation* const o = make(b, kinds[shape % 3U], ConstSpan<Value*>(operands, 2U), 1U);
            if ((shape & 0x0CU) != 0U)
            {
                m_ctx.set_attr(o, "depth", m_ctx.attr_int(1 + static_cast<i64>((shape >> 4U) & 3U)));
            }
            publish(o, scope);
            feedback.push_back(Feedback{o, scope.size()});
            break;
        }
        case OpCall:
        {
            const u32     callee = m_in.byte() % m_h.functions;
            Array<Value*> args(m_alloc);
            for (u32 a = 0U; a < m_h.params[callee]; ++a)
            {
                args.push_back(pick(b, scope));
            }
            char name[4] = {'f', static_cast<char>('0' + callee), '\0', '\0'};
            const ConstSpan<Value*> span(args.data(), args.size());
            Operation* const        o = append(b, crd::ceir::func::create_call(m_ctx, StringView(name, 2U), span,
                                                                               m_h.results[callee], m_ctx.type_i32()));
            publish(o, scope);
            break;
        }
        case OpLaunch:
        {
            const u8   shape   = m_in.byte();
            const OpId kinds[] = {m_k.alaunch, m_k.tspawn, m_k.tmain, m_k.tworker};
            const u32  yields  = (shape >> 2U) % 3U;
            Operation* const o = make(b, kinds[shape % 4U], {}, 1U, 1U);
            child(region_block(o, 0U, 0U), scope, depth + 1U, isolated, yields, End::Yield);
            tokens.push_back(Token{o->result(0U), yields});
            break;
        }
        case OpAwait:
        {
            const u8   shape = m_in.byte();
            const OpId kind  = (shape & 1U) != 0U ? m_k.tfiber : m_k.aawait;
            if (!tokens.empty() && (shape & 6U) != 0U)
            {
                const Token t      = take(tokens, m_in.byte());
                Value* operand[1]  = {t.value};
                Operation* const o = make(b, kind, ConstSpan<Value*>(operand, 1U), (shape >> 3U) % (t.yields + 1U));
                publish(o, scope);
            }
            else // a forged handle: any value (a live handle number reads that token; anything else is BadToken)
            {
                Value* operand[1] = {pick(b, scope)};
                (void)make(b, kind, ConstSpan<Value*>(operand, 1U), 0U);
            }
            break;
        }
        case OpJoin:
        case OpRace:
        {
            const u32     want = m_in.byte() % 4U;
            Array<Value*> operands(m_alloc);
            u32           yields = 0U;
            for (u32 i = 0U; i < want && !tokens.empty(); ++i)
            {
                const Token t = take(tokens, m_in.byte());
                operands.push_back(t.value);
                yields += t.yields;
            }
            const ConstSpan<Value*> span(operands.data(), operands.size());
            Operation* const        o = make(b, code == OpJoin ? m_k.ajoin : m_k.arace, span, 1U);
            if (code == OpJoin)
            {
                tokens.push_back(Token{o->result(0U), yields});
            }
            else
            {
                publish(o, scope);
            }
            break;
        }
        case OpCancel:
        {
            if (!tokens.empty())
            {
                Value* operand[1] = {take(tokens, m_in.byte()).value};
                (void)make(b, m_k.acancel, ConstSpan<Value*>(operand, 1U), 0U);
            }
            break;
        }
        case OpContinuation:
        {
            const u8 shape = m_in.byte();
            if (tokens.empty())
            {
                break;
            }
            const Token t = take(tokens, m_in.byte());
            // Its body takes the antecedent's values as block arguments; one in eight takes a wrong count.
            const u32 args     = (shape & 7U) == 0U ? (t.yields + 1U) % 3U : t.yields;
            const u32 yields   = (shape >> 3U) % 3U;
            Value* operand[1]  = {t.value};
            Operation* const o = make(b, m_k.tcont, ConstSpan<Value*>(operand, 1U), 1U, 1U);
            child(region_block(o, 0U, args), scope, depth + 1U, isolated, yields, End::Yield);
            tokens.push_back(Token{o->result(0U), yields});
            break;
        }
        case OpRandom:
        {
            Operation* const o = make(b, m_k.irandom, {}, 1U);
            const u8         s = m_in.byte();
            const u8         n = m_in.byte();
            m_ctx.set_attr(o, "stream", m_ctx.attr_int(static_cast<i64>(s)));
            m_ctx.set_attr(o, "bound", m_ctx.attr_int(n == 0xFFU ? i64{0xFFFFFFFF} : static_cast<i64>(n) + 1));
            publish(o, scope);
            break;
        }
        case OpTime:
        {
            const u8         shape = m_in.byte();
            Operation* const o     = make(b, (shape & 0x80U) != 0U ? m_k.istep : m_k.iclock, {}, 1U);
            const StringView name  = crd::ceir::time::builtin_domain_name(shape % crd::ceir::time::kBuiltinDomainCount);
            m_ctx.set_attr(o, "domain", m_ctx.attr_string(name));
            publish(o, scope);
            break;
        }
        case OpEvent:
        {
            Operation* const o = make(b, m_k.ievent, {}, 5U);
            m_ctx.set_attr(o, "queue", m_ctx.attr_int(static_cast<i64>(m_in.byte() % 4U)));
            publish(o, scope);
            break;
        }
        default: break;
        }
    }

    Context&                  m_ctx;
    const Kinds&              m_k;
    Reader&                   m_in;
    const Header&             m_h;
    crd::memory::IAllocator*  m_alloc;
    u32                       m_ops = 0U;
};

void decode_header(Reader& in, Header& h)
{
    h.functions        = 1U + in.byte() % kMaxFunctions;
    h.max_steps        = kFuelBase << (in.byte() % 12U);
    const u32 script   = in.byte() % (kMaxScript + 2U);
    h.has_source       = script <= kMaxScript;
    for (u32 i = 0U; h.has_source && i < script; ++i)
    {
        h.script.push_back(in.value());
    }
    for (u32 f = 0U; f < h.functions; ++f)
    {
        const u8 shape = in.byte();
        h.params[f]    = shape % 3U;
        h.results[f]   = (shape / 3U) % 3U;
    }
    for (u32 a = 0U; a < h.params[0]; ++a)
    {
        h.args.push_back(in.value());
    }
}

// ---- running --------------------------------------------------------------------------------------------------------

// The host input both executors read: the script's values in read order, then nothing.
struct Script
{
    const Array<i64>* values = nullptr;
    usize             at     = 0U;

    static bool next(crd::ceir::input::InputKind /*kind*/, u32 /*channel*/, i64& out, void* user)
    {
        auto* const s = static_cast<Script*>(user);
        if (s->at >= s->values->size())
        {
            return false;
        }
        out = (*s->values)[s->at++];
        return true;
    }
};

// The error classes the two tiers share (plan.hpp names each RunError's reference counterpart).
enum class Kind : u8
{
    None = 0,
    BadForStep,
    SelectorOutOfRange,
    CondArity,
    FuelExhausted,
    BadToken,
    Arity, // the reference's BadArity at a continuation; the plan's ContinuationArity
    Cancelled,
    InputUnavailable,
    NestingTooDeep,
    Unmatched, // a reference error the compiled tier has no counterpart for: never expected from a compiled program
};

Kind kind_of(exec::ExecError e) noexcept
{
    switch (e)
    {
    case exec::ExecError::None: return Kind::None;
    case exec::ExecError::BadForStep: return Kind::BadForStep;
    case exec::ExecError::SelectorOutOfRange: return Kind::SelectorOutOfRange;
    case exec::ExecError::CondArity: return Kind::CondArity;
    case exec::ExecError::FuelExhausted: return Kind::FuelExhausted;
    case exec::ExecError::BadToken: return Kind::BadToken;
    case exec::ExecError::BadArity: return Kind::Arity;
    case exec::ExecError::Cancelled: return Kind::Cancelled;
    case exec::ExecError::InputUnavailable: return Kind::InputUnavailable;
    case exec::ExecError::NestingTooDeep: return Kind::NestingTooDeep;
    default: return Kind::Unmatched;
    }
}

Kind kind_of(plan::RunError e) noexcept
{
    switch (e)
    {
    case plan::RunError::None: return Kind::None;
    case plan::RunError::BadForStep: return Kind::BadForStep;
    case plan::RunError::SelectorOutOfRange: return Kind::SelectorOutOfRange;
    case plan::RunError::CondArity: return Kind::CondArity;
    case plan::RunError::FuelExhausted: return Kind::FuelExhausted;
    case plan::RunError::BadToken: return Kind::BadToken;
    case plan::RunError::ContinuationArity: return Kind::Arity;
    case plan::RunError::Cancelled: return Kind::Cancelled;
    case plan::RunError::InputUnavailable: return Kind::InputUnavailable;
    case plan::RunError::NestingTooDeep: return Kind::NestingTooDeep;
    default: return Kind::Unmatched;
    }
}

// One run's observable outcome, normalized so either tier's result compares field by field.
struct Outcome
{
    Kind               kind = Kind::None;
    StringView         error;
    Array<i64>         values;
    Array<i64>         cells; // dense cell order (the compiled plan's numbering)
    Array<Array<i64>>  maps;  // dense map order; filled on success only
    u64                fault = 0U; // the stable id of the op an error blames
    explicit Outcome(crd::memory::IAllocator* a) : values(a), cells(a), maps(a) {}
};

void copy_into(Array<i64>& dst, ConstSpan<i64> src)
{
    for (usize i = 0U; i < src.size(); ++i)
    {
        dst.push_back(src[i]);
    }
}

// The op each dense cell and map index of a compiled plan names, by stable id.
struct DenseOps
{
    Array<u64> cells;
    Array<u64> maps;
    explicit DenseOps(crd::memory::IAllocator* a) : cells(a), maps(a) {}
};

void dense_ops(const plan::CompiledPlan& p, DenseOps& out)
{
    out.cells.resize(p.num_cells, 0U);
    out.maps.resize(p.num_maps, 0U);
    for (usize s = 0U; s < p.seqs.size(); ++s)
    {
        const plan::Seq& seq = p.seqs[s];
        for (usize k = 0U; k < seq.instrs.size(); ++k)
        {
            const plan::Instr& in = seq.instrs[k];
            const u64          id = p.sites[seq.sites[k]].op.value;
            if (in.op == plan::Op::State)
            {
                out.cells[static_cast<usize>(in.imm)] = id;
            }
            else if (in.op == plan::Op::ParallelFor || in.op == plan::Op::MapReduce)
            {
                out.maps[static_cast<usize>(in.imm)] = id;
            }
        }
    }
}

void index_ops(const crd::ceir::Region& r, HashMap<u64, const Operation*>& out) // NOLINT(misc-no-recursion)
{
    for (const Block* b = r.first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            out.insert(op->stable_id().value, op);
            for (u32 i = 0U; i < op->num_regions(); ++i)
            {
                index_ops(*op->region(i), out);
            }
        }
    }
}

struct Program
{
    Context&      ctx;
    const Module& module;
    const Header& header;
};

void run_reference(const Program& p, const DenseOps& dense, const HashMap<u64, const Operation*>& ops,
                   Outcome& out)
{
    crd::memory::IAllocator* const alloc = p.ctx.allocator();
    exec::Interpreter              in(p.ctx, p.header.max_steps, alloc);
    exec::install_builtin_semantics(in);
    exec::install_async_semantics(in);
    exec::install_task_semantics(in);
    exec::install_input_semantics(in);
    Script                         script{&p.header.script, 0U};
    const crd::ceir::input::InputSource source{&Script::next, &script};
    if (p.header.has_source)
    {
        in.set_input_source(&source);
    }
    const exec::ExecResult r = in.invoke(p.module, "f0", ConstSpan<i64>(p.header.args.data(), p.header.args.size()));
    out.kind  = kind_of(r.error);
    out.error = exec::exec_error_name(r.error);
    out.fault = r.op != nullptr ? r.op->stable_id().value : 0U;
    copy_into(out.values, ConstSpan<i64>(r.values.data(), r.values.size()));
    for (usize c = 0U; c < dense.cells.size(); ++c)
    {
        const Operation* const* const op = ops.find(dense.cells[c]);
        i64                           v  = 0;
        if (op != nullptr && !in.cell_value(*op, v))
        {
            v = 0; // never evaluated: the plan reports 0
        }
        out.cells.push_back(v);
    }
    if (r.error == exec::ExecError::None)
    {
        for (usize m = 0U; m < dense.maps.size(); ++m)
        {
            const Operation* const* const op = ops.find(dense.maps[m]);
            Array<i64>                    map(alloc);
            if (op != nullptr)
            {
                copy_into(map, in.map_output(*op));
            }
            out.maps.push_back(std::move(map));
        }
    }
}

void run_plan(const plan::CompiledPlan& compiled, const Header& h, crd::memory::IAllocator* alloc, Outcome& out)
{
    Script                              script{&h.script, 0U};
    const crd::ceir::input::InputSource source{&Script::next, &script};
    const plan::RunResult r = plan::run(compiled, ConstSpan<i64>(h.args.data(), h.args.size()), alloc, {}, nullptr,
                                        h.has_source ? &source : nullptr, h.max_steps);
    out.kind  = kind_of(r.error);
    out.error = plan::run_error_name(r.error);
    out.fault = r.fault.valid() ? plan::instr_provenance(compiled, r.fault).op.value : 0U;
    copy_into(out.values, ConstSpan<i64>(r.values.data(), r.values.size()));
    copy_into(out.cells, ConstSpan<i64>(r.cells.data(), r.cells.size()));
    if (r.error == plan::RunError::None)
    {
        for (usize m = 0U; m < r.map_outputs.size(); ++m)
        {
            Array<i64> map(alloc);
            copy_into(map, ConstSpan<i64>(r.map_outputs[m].data(), r.map_outputs[m].size()));
            out.maps.push_back(std::move(map));
        }
    }
}

[[nodiscard]] bool same_values(const Array<i64>& a, const Array<i64>& b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize i = 0U; i < a.size(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

void print_values(const char* label, const Array<i64>& v)
{
    std::fprintf(stderr, "  %s [", label);
    for (usize i = 0U; i < v.size(); ++i)
    {
        std::fprintf(stderr, "%s%lld", i == 0U ? "" : ", ", static_cast<long long>(v[i]));
    }
    std::fprintf(stderr, "]\n");
}

// The program under test, printed when an oracle fails so a finding carries its own reproduction text.
Context*      g_ctx    = nullptr;
const Module* g_module = nullptr;

void dump_program()
{
    if (g_ctx == nullptr || g_module == nullptr)
    {
        return;
    }
    const String text = crd::ceir::print(*g_ctx, *g_module, g_ctx->allocator());
    std::fprintf(stderr, "fuzz: the program:\n%.*s\n", static_cast<int>(text.size()), text.data());
}

// The two outcomes must agree field by field; a disagreement ends the run naming both sides.
void require_same(const char* what, const Outcome& a, const char* a_name, const Outcome& b, const char* b_name)
{
    bool same = a.kind == b.kind && same_values(a.values, b.values) && same_values(a.cells, b.cells) &&
                a.maps.size() == b.maps.size() && (a.kind == Kind::None || a.fault == b.fault);
    for (usize m = 0U; same && m < a.maps.size(); ++m)
    {
        same = same_values(a.maps[m], b.maps[m]);
    }
    if (same)
    {
        return;
    }
    std::fprintf(stderr, "fuzz: %s disagree\n  %s: %.*s fault=%llu\n  %s: %.*s fault=%llu\n", what, a_name,
                 static_cast<int>(a.error.size()), a.error.data(), static_cast<unsigned long long>(a.fault), b_name,
                 static_cast<int>(b.error.size()), b.error.data(), static_cast<unsigned long long>(b.fault));
    print_values("values a", a.values);
    print_values("values b", b.values);
    print_values("cells a", a.cells);
    print_values("cells b", b.cells);
    for (usize m = 0U; m < a.maps.size() || m < b.maps.size(); ++m)
    {
        if (m < a.maps.size())
        {
            print_values("map a", a.maps[m]);
        }
        if (m < b.maps.size())
        {
            print_values("map b", b.maps[m]);
        }
    }
    dump_program();
    std::fflush(stderr);
    crd::fuzz::oracle_failed(what, __FILE__, __LINE__);
}

// The module-validity set a cook enforces before a program can run (program_cook.cpp, step 2).
[[nodiscard]] bool valid_program(const Context& ctx, const Module& m)
{
    if (ctx.find_structure_error(m).kind != crd::ceir::StructureErrorKind::None)
    {
        return false;
    }
    if (ctx.find_domain_violation(m).op != nullptr)
    {
        return false;
    }
    if (ctx.find_token_misuse(m).kind != crd::ceir::TokenMisuseKind::None)
    {
        return false;
    }
    if (ctx.find_borrowed_escape(m).escaping_use != nullptr)
    {
        return false;
    }
    const crd::ceir::SymbolTable* const syms = m.symbols();
    return syms != nullptr && crd::ceir::func::find_recursion_violation(ctx, m, *syms).kind ==
                                  crd::ceir::func::RecursionViolationKind::None;
}

// Compile and run a reloaded twin of the program in its own Context: it must compile exactly as the original did and
// give the same outcome.
void require_twin(const char* what, Context& twin, const Module* m, const Header& h,
                  const plan::CompileResult& original, const Outcome& expected)
{
    CRD_FUZZ_REQUIRE(m != nullptr);
    const plan::CompileResult cr = plan::compile(twin, *m, "f0", twin.allocator());
    if (cr.error != original.error)
    {
        std::fprintf(stderr, "fuzz: %s compiled as %.*s, the original as %.*s\n", what,
                     static_cast<int>(plan::compile_error_name(cr.error).size()),
                     plan::compile_error_name(cr.error).data(),
                     static_cast<int>(plan::compile_error_name(original.error).size()),
                     plan::compile_error_name(original.error).data());
    }
    CRD_FUZZ_REQUIRE(cr.error == original.error);
    if (!cr.ok())
    {
        return;
    }
    Outcome got(twin.allocator());
    run_plan(cr.plan, h, twin.allocator(), got);
    require_same(what, expected, "original", got, "twin");
}

// Run one decoded program through every oracle. Returns whether it was inside the contract and run.
bool differential(const u8* data, usize size)
{
    crd::fuzz::BudgetAllocator root;
    Context                    ctx(&root);
    const Kinds                kinds(ctx);
    Reader                     in(data, size);
    Header                     header(&root);
    decode_header(in, header);
    Generator     gen(ctx, kinds, in, header);
    Module* const module = gen.build();
    if (!valid_program(ctx, *module))
    {
        return false;
    }
    ctx.assign_stable_ids(*module);
    const Program program{ctx, *module, header};
    g_ctx    = &ctx;
    g_module = module;

    const plan::CompileResult compiled = plan::compile(ctx, *module, "f0", &root);
    DenseOps                  dense(&root);
    if (compiled.ok())
    {
        dense_ops(compiled.plan, dense);
    }
    HashMap<u64, const Operation*> ops(&root);
    index_ops(*module->body(), ops);

    Outcome reference(&root);
    run_reference(program, dense, ops, reference);
    Outcome again(&root); // a fresh reference session is deterministic
    run_reference(program, dense, ops, again);
    require_same("two reference sessions", reference, "first", again, "second");

    if (!compiled.ok())
    {
        // The documented tier difference: a parallel body that holds a cell or reads host input (directly or through
        // a call) is refused whole at compile, while the reference refuses it only when the op runs.
        const bool documented = compiled.error == plan::CompileError::ParallelStateful;
        if (!documented)
        {
            const StringView name = plan::compile_error_name(compiled.error);
            std::fprintf(stderr, "fuzz: the plan refused a valid program: %.*s (op %llu)\n",
                         static_cast<int>(name.size()), name.data(),
                         static_cast<unsigned long long>(compiled.op != nullptr ? compiled.op->stable_id().value
                                                                                : 0U));
        }
        CRD_FUZZ_REQUIRE(documented);
        CRD_FUZZ_REQUIRE(reference.kind != Kind::Unmatched ||
                         StringView(reference.error) == exec::exec_error_name(exec::ExecError::ParallelBodyStateful));
        return true;
    }
    if (reference.kind == Kind::Unmatched)
    {
        std::fprintf(stderr, "fuzz: the reference failed with %.*s, which a compiled program cannot\n",
                     static_cast<int>(reference.error.size()), reference.error.data());
    }
    CRD_FUZZ_REQUIRE(reference.kind != Kind::Unmatched);

    Outcome first(&root);
    run_plan(compiled.plan, header, &root, first);
    require_same("reference and plan", reference, "reference", first, "plan");
    Outcome second(&root);
    run_plan(compiled.plan, header, &root, second);
    require_same("two plan runs", first, "first", second, "second");

    // The binary twin: serialized and loaded again into a fresh Context.
    {
        const Array<u8>         blob = crd::ceir::serialize(ctx, *module, &root);
        Context                 twin(&root);
        const Kinds             twin_kinds(twin);
        const crd::ceir::ParseResult loaded = crd::ceir::deserialize(twin, ConstSpan<u8>(blob.data(), blob.size()));
        CRD_FUZZ_REQUIRE(loaded.ok);
        require_twin("the serialized twin", twin, loaded.module, header, compiled, first);
    }
    // The text twin: printed and parsed again into a fresh Context.
    {
        const String                 text = crd::ceir::print(ctx, *module, &root);
        Context                      twin(&root);
        const Kinds                  twin_kinds(twin);
        const crd::ceir::ParseResult parsed = crd::ceir::parse(twin, StringView(text.data(), text.size()));
        if (!parsed.ok)
        {
            std::fprintf(stderr, "fuzz: parse refused the printed program: %s at byte %zu\n", parsed.error,
                         static_cast<std::size_t>(parsed.error_offset));
        }
        CRD_FUZZ_REQUIRE(parsed.ok);
        require_twin("the printed twin", twin, parsed.module, header, compiled, first);
    }
    return true;
}

// ---- seeds ----------------------------------------------------------------------------------------------------------

// Writes inputs in the decoder's format, so each seed reads as the program it is. A body is its op count, then per op
// its code and the bytes that op reads (operand picks index the visible values in definition order), then a feedback
// choice per cell and a pick per yielded value.
class Enc
{
public:
    explicit Enc(crd::memory::IAllocator* a) : m_bytes(a) {}

    Enc& b(u8 v)
    {
        m_bytes.push_back(v);
        return *this;
    }
    Enc& bs(std::initializer_list<u8> list)
    {
        for (const u8 v : list)
        {
            m_bytes.push_back(v);
        }
        return *this;
    }
    // A value: a small one in one byte, any other as eight raw bytes.
    Enc& v(i64 x)
    {
        if (x >= -64 && x <= 63)
        {
            return b(static_cast<u8>(x + 64));
        }
        b(kRawValue);
        for (u32 i = 0U; i < 8U; ++i)
        {
            b(static_cast<u8>(static_cast<u64>(x) >> (8U * i)));
        }
        return *this;
    }
    // The header: the function count, the step-budget exponent, the host input script (or no source at all) and each
    // function's shape byte. The entry arguments follow with v().
    Enc& head(u32 functions, u32 fuel_shift, std::initializer_list<u8> shapes, std::initializer_list<i64> script = {},
              bool source = true)
    {
        b(static_cast<u8>(functions - 1U));
        b(static_cast<u8>(fuel_shift));
        b(static_cast<u8>(source ? script.size() : kMaxScript + 1U));
        for (const i64 x : script)
        {
            v(x);
        }
        return bs(shapes);
    }

    [[nodiscard]] const Array<u8>& bytes() const noexcept { return m_bytes; }

private:
    Array<u8> m_bytes;
};

// A function shape byte: `params` parameters, `results` results.
constexpr u8 shape(u32 params, u32 results) noexcept
{
    return static_cast<u8>(params + 3U * results);
}

bool seed_runs(const u8* data, usize size)
{
    return differential(data, size);
}

// Every seed is a program inside the contract, run by both tiers (a compile refusal of the documented kind included).
const char* const kSeeds[] = {
    "empty",
    "arith-predicates-edges",
    "if-yields",
    "for-cell-accumulator",
    "for-step-near-max",
    "for-bad-step",
    "while-cond-yield-fuel",
    "while-cell-counter",
    "switch-arms",
    "switch-selector-out-of-range",
    "scope-kinds",
    "cells-delay-history-calls",
    "call-chain",
    "recursion-unbounded",
    "recursion-at-bound",
    "recursion-past-bound",
    "map-reduce-recursion-at-bound",
    "map-reduce-recursion-past-bound",
    "map-reduce-reentered",
    "parallel-range-huge",
    "parallel-range-wide",
    "map-reduce-index-order",
    "map-reduce-fold-fuel",
    "tokens",
    "tokens-in-a-loop-with-a-parallel-op",
    "await-forged-dead",
    "await-forged-live",
    "continuation-arity",
    "host-inputs",
    "host-inputs-exhausted",
    "host-inputs-no-source",
    "parallel-body-stateful",
};

class Seeds
{
public:
    Seeds(crd::fuzz::SeedSink& sink, crd::memory::IAllocator* alloc) : m_sink(sink), m_alloc(alloc) {}

    // A seed: `write` fills a fresh encoder.
    template <typename Write> void add(const char* name, Write write)
    {
        Enc e(m_alloc);
        write(e);
        m_sink.add(name, e.bytes().data(), e.bytes().size());
    }

private:
    crd::fuzz::SeedSink&     m_sink;
    crd::memory::IAllocator* m_alloc;
};

// f0(n): m = n - 1; if (n) { call f0(m) } -- two nesting levels per call (the if and the call).
void recursion(Enc& e, i64 n)
{
    e.head(1U, 11U, {shape(1U, 0U)}).v(n);
    e.b(3U).b(OpConst).v(-1).bs({OpAdd, 0, 1, OpIf, 0, 0});
    e.bs({1, OpCall, 0, 2}); // then: call f0(m)
    e.b(0U);                // else: nothing
}

// f0(n): m = n - 1; if (n) { map_reduce(m, n, 1, m) { (iv): call f0(iv); yield iv } { (a, x): yield a } } -- three
// levels per call (the if, the map_reduce and the call), the call made from an isolated body on a sub-interpreter.
void map_reduce_recursion(Enc& e, i64 n)
{
    e.head(1U, 11U, {shape(1U, 0U)}).v(n);
    e.b(3U).b(OpConst).v(-1).bs({OpAdd, 0, 1, OpIf, 0, 0});
    e.b(2U).b(OpConst).v(1).bs({OpMapReduce, 2, 0, 3, 2}); // then: c = 1; map_reduce(m, n, c, m)
    e.bs({1, OpCall, 0, 0, 0});                            // map: call f0(iv); yield iv
    e.bs({0, 0});                                         // combine: yield a
    e.b(0U);                                              // else: nothing
}

// f0(): a parallel_for over [lo, hi) by `step` whose body yields its index.
void parallel_range(Enc& e, i64 lo, i64 hi, i64 step)
{
    e.head(1U, 6U, {shape(0U, 0U)});
    e.b(4U).b(OpConst).v(lo).b(OpConst).v(hi).b(OpConst).v(step).bs({OpParallelFor, 0, 1, 2});
    e.bs({0, 0}); // body: yield iv
}

// f0(): random, clock, time_step and event reads, returning random + clock + step and the event's code.
void host_inputs(Enc& e, std::initializer_list<i64> script, bool source)
{
    e.head(1U, 4U, {shape(0U, 2U)}, script, source);
    e.bs({6, OpRandom, 3, 9, OpTime, 0, OpTime, 0x80, OpEvent, 1, OpAdd, 0, 1, OpAdd, 8, 2});
    e.bs({9, 4});
}

void write_seeds(Seeds& s)
{
    s.add("empty", [](Enc& e) { e.head(1U, 4U, {shape(0U, 1U)}).bs({0, 0}); });
    // f0(a, b) over i64 edges: every predicate family (signed and unsigned disagree on INT64_MIN against 1), wrapping
    // add and multiply.
    s.add("arith-predicates-edges", [](Enc& e) {
        e.head(1U, 4U, {shape(2U, 2U)}).v(INT64_MIN).v(1);
        e.bs({10, OpCmp, 0, 1, 0, OpCmp, 0, 1, 2, OpCmp, 0, 1, 6, OpCmp, 1, 0, 9, OpAdd, 0, 1, OpMul, 0, 1});
        e.bs({OpCmp, 6, 7, 4, OpMul, 3, 4, OpAdd, 9, 5, OpAdd, 10, 8});
        e.bs({7, 11});
    });
    // f0(c): a two-result if, else branch taken.
    s.add("if-yields", [](Enc& e) {
        e.head(1U, 4U, {shape(1U, 2U)}).v(0);
        e.bs({2, OpIf, 0, 2});
        e.b(2U).b(OpConst).v(7).b(OpConst).v(9).bs({1, 2}); // then: yield 7, 9
        e.bs({1, OpAdd, 0, 0, 1, 0});                      // else: yield c + c, c
        e.bs({OpAdd, 1, 2, 3, 1});
    });
    // f0(n): for iv in [1, n): a depth-3 state cell fed back with cell + iv.
    s.add("for-cell-accumulator", [](Enc& e) {
        e.head(1U, 6U, {shape(1U, 1U)}).v(6);
        e.b(3U).b(OpConst).v(0).b(OpConst).v(1).bs({OpFor, 1, 0, 2});
        e.bs({2, OpState, 0x24, 1, OpAdd, 4, 3, 0}); // body: s = state(0) depth 3; s + iv feeds it back
        e.b(0U);
    });
    // A for loop whose step reaches past INT64_MAX: one iteration (the increment used to overflow).
    s.add("for-step-near-max", [](Enc& e) {
        e.head(1U, 6U, {shape(0U, 1U)});
        e.b(4U).b(OpConst).v(INT64_MAX - 1).b(OpConst).v(INT64_MAX).bs({OpFor, 0, 1, 1});
        e.bs({1, OpAdd, 2, 2});
        e.b(OpConst).v(5).b(2U);
    });
    s.add("for-bad-step", [](Enc& e) {
        e.head(1U, 4U, {shape(0U, 0U)});
        e.b(4U).b(OpConst).v(0).b(OpConst).v(5).b(OpConst).v(0).bs({OpFor, 0, 1, 2, 0});
    });
    // A while whose condition yields true forever at a 64-step budget: both tiers exhaust at the same op (the
    // reference used to charge the condition's core.yield a step).
    s.add("while-cond-yield-fuel", [](Enc& e) {
        e.head(1U, 0U, {shape(1U, 0U)}).v(1);
        e.bs({1, OpWhile, 1, OpCmp, 0, 0, 0, 1}); // cond: x == x
        e.b(1U).b(OpConst).v(3);                 // body
    });
    // A while counting a cell up to n inside its condition region.
    s.add("while-cell-counter", [](Enc& e) {
        e.head(1U, 8U, {shape(1U, 0U)}).v(5);
        e.b(3U).b(OpConst).v(0).b(OpConst).v(1).b(OpWhile);
        e.bs({3, OpState, 0, 1, OpAdd, 3, 2, OpCmp, 3, 0, 2, 0, 5}); // cond: s = state(0); s + 1 feeds it; yield s < n
        e.b(1U).b(OpConst).v(9);
    });
    s.add("switch-arms", [](Enc& e) {
        e.head(1U, 4U, {shape(1U, 0U)}).v(1);
        e.bs({2, OpSwitch, 0, 2});
        e.b(1U).b(OpConst).v(10).b(1U).b(OpConst).v(11).b(1U).b(OpConst).v(12);
        e.bs({OpSwitch, 0, 0x10});
        e.b(1U).b(OpConst).v(20).b(1U).b(OpConst).v(21);
    });
    s.add("switch-selector-out-of-range", [](Enc& e) {
        e.head(1U, 4U, {shape(1U, 0U)}).v(-1);
        e.bs({1, OpSwitch, 0, 1});
        e.b(1U).b(OpConst).v(10).b(1U).b(OpConst).v(11);
    });
    // core.scope (two results), async.scope (one) and task.group (none).
    s.add("scope-kinds", [](Enc& e) {
        e.head(1U, 4U, {shape(0U, 2U)});
        e.bs({3, OpScope, 9});
        e.b(2U).b(OpConst).v(1).b(OpConst).v(2).bs({0, 1});
        e.bs({OpScope, 4, 1, OpAdd, 0, 1, 2});
        e.bs({OpScope, 2, 1, OpMul, 2, 2});
        e.bs({2, 0});
    });
    // f0(n) loops calling f1 (a counter cell) and feeds a delay and a depth-3 history; cells persist across calls.
    s.add("cells-delay-history-calls", [](Enc& e) {
        e.head(2U, 6U, {shape(1U, 1U), shape(0U, 1U)}).v(4);
        e.b(4U).b(OpConst).v(0).b(OpConst).v(1).bs({OpFor, 1, 0, 2});
        e.bs({4, OpCall, 1, OpState, 0x01, 4, OpState, 0x26, 3, OpAdd, 5, 6, 2, 0}); // delay(r); history(iv) depth 3
        e.bs({OpCall, 1, 3});
        e.b(4U).b(OpConst).v(0).bs({OpState, 0, 0}).b(OpConst).v(1).bs({OpAdd, 1, 2, 2, 3}); // f1: c = state(0); c + 1
    });
    // f0(x) = r0 * r1 where (r0, r1) = f1(x, x); f1(a, b) = (f2(a), b); f2(a) = a + 1.
    s.add("call-chain", [](Enc& e) {
        e.head(3U, 4U, {shape(1U, 1U), shape(2U, 2U), shape(1U, 1U)}).v(5);
        e.bs({2, OpCall, 1, 0, 0, OpMul, 1, 2, 3});
        e.bs({1, OpCall, 2, 0, 2, 1});
        e.b(2U).b(OpConst).v(1).bs({OpAdd, 0, 1, 2});
    });
    // f0() { call f0 }: refused at the nesting bound before the native stack runs out, by both tiers at the same call.
    s.add("recursion-unbounded", [](Enc& e) { e.head(1U, 11U, {shape(0U, 0U)}).bs({1, OpCall, 0}); });
    s.add("recursion-at-bound", [](Enc& e) { recursion(e, 31); });
    s.add("recursion-past-bound", [](Enc& e) { recursion(e, 32); });
    s.add("map-reduce-recursion-at-bound", [](Enc& e) { map_reduce_recursion(e, 21); });
    s.add("map-reduce-recursion-past-bound", [](Enc& e) { map_reduce_recursion(e, 22); });
    // The map_reduce op runs again inside its own body: each activation keeps its own map.
    s.add("map-reduce-reentered", [](Enc& e) { map_reduce_recursion(e, 2); });
    // 2^31 indices: bounded by the step budget per index, never pre-sized.
    s.add("parallel-range-huge", [](Enc& e) { parallel_range(e, 0, i64{1} << 31U, 1); });
    // [INT64_MIN, INT64_MAX) by 2^62: four indices (the old count overflowed and wrapped to none).
    s.add("parallel-range-wide", [](Enc& e) { parallel_range(e, INT64_MIN, INT64_MAX, i64{1} << 62U); });
    // map iv * iv over [0, 5), fold acc * 31 + elem: a non-associative fold in index order.
    s.add("map-reduce-index-order", [](Enc& e) {
        e.head(1U, 6U, {shape(0U, 1U)});
        e.b(5U).b(OpConst).v(0).b(OpConst).v(5).b(OpConst).v(1).b(OpConst).v(0).bs({OpMapReduce, 0, 1, 2, 3});
        e.bs({1, OpMul, 0, 0, 1});
        e.b(3U).b(OpConst).v(31).bs({OpMul, 0, 2, OpAdd, 3, 1, 4});
        e.b(4U);
    });
    // map_reduce over [0, 40) at a 64-step budget: the map (one step per index) fits, the fold (two steps per element:
    // the step and the combine's add) runs out inside the combine body, at the same op in both tiers.
    s.add("map-reduce-fold-fuel", [](Enc& e) {
        e.head(1U, 0U, {shape(0U, 1U)});
        e.b(5U).b(OpConst).v(0).b(OpConst).v(40).b(OpConst).v(1).b(OpConst).v(0).bs({OpMapReduce, 0, 1, 2, 3});
        e.bs({0, 0});
        e.bs({1, OpAdd, 0, 1, 2});
        e.b(4U);
    });
    // launch (two values), spawn (one), join, await three, worker, continuation, main_thread, race, fiber_wait.
    s.add("tokens", [](Enc& e) {
        e.head(1U, 6U, {shape(0U, 1U)});
        e.b(11U).bs({OpLaunch, 8}).b(2U).b(OpConst).v(3).b(OpConst).v(4).bs({0, 1});
        e.bs({OpLaunch, 5}).b(1U).b(OpConst).v(10).b(0U);
        e.bs({OpJoin, 2, 0, 0, OpAwait, 26, 0});
        e.bs({OpLaunch, 7}).b(1U).b(OpConst).v(7).b(3U);
        e.bs({OpContinuation, 9, 0, 1, OpAdd, 3, 0, 4});
        e.bs({OpLaunch, 2, 0});
        e.bs({OpRace, 1, 1, OpAwait, 11, 0, OpAdd, 0, 4, OpAdd, 5, 3});
        e.b(6U);
    });
    // 8000 iterations that each launch a token and run a parallel_for: the compiled plan moved the whole run-wide
    // token store out and back for every parallel activation (quadratic: 7.7 s on win-debug, 0.15 s now).
    s.add("tokens-in-a-loop-with-a-parallel-op", [](Enc& e) {
        e.head(1U, 11U, {shape(0U, 0U)});
        e.b(4U).b(OpConst).v(0).b(OpConst).v(8000).b(OpConst).v(1).bs({OpFor, 0, 1, 2});
        e.b(4U).bs({OpLaunch, 0, 0}).b(OpConst).v(0).b(OpConst).v(1).bs({OpParallelFor, 4, 5, 2, 0, 0});
    });
    s.add("await-forged-dead", [](Enc& e) { e.head(1U, 4U, {shape(1U, 0U)}).v(5).bs({1, OpAwait, 0, 0}); });
    s.add("await-forged-live", [](Enc& e) {
        e.head(1U, 4U, {shape(1U, 0U)}).v(0).bs({2, OpLaunch, 0, 0, OpAwait, 0, 0});
    });
    s.add("continuation-arity", [](Enc& e) {
        e.head(1U, 4U, {shape(0U, 0U)});
        e.bs({2, OpLaunch, 4}).b(1U).b(OpConst).v(1).b(0U);
        e.bs({OpContinuation, 0, 0, 0});
    });
    s.add("host-inputs", [](Enc& e) { host_inputs(e, {12345, 1000, 16, 1 + (65 << 8)}, true); });
    s.add("host-inputs-exhausted", [](Enc& e) { host_inputs(e, {12345, 1000}, true); });
    s.add("host-inputs-no-source", [](Enc& e) { host_inputs(e, {}, false); });
    // A parallel body calling a function that holds a cell: refused whole at compile, refused by the reference as
    // ParallelBodyStateful when the op runs.
    s.add("parallel-body-stateful", [](Enc& e) {
        e.head(2U, 4U, {shape(0U, 0U), shape(1U, 1U)});
        e.b(4U).b(OpConst).v(0).b(OpConst).v(2).b(OpConst).v(1).bs({OpParallelFor, 0, 1, 2});
        e.bs({1, OpCall, 1, 0, 1});
        e.bs({1, OpState, 0, 0, 0, 1});
    });
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    (void)differential(data, size);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    crd::fuzz::BudgetAllocator root;
    crd::fuzz::VerdictSink     checked(sink, &seed_runs, kSeeds, sizeof(kSeeds) / sizeof(kSeeds[0]));
    Seeds                      seeds(checked, &root);
    write_seeds(seeds);
}
