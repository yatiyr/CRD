// lower.cpp — the CHIR->CEIR lowering (CEIR-32d, ADR-0128 D1/D5). See lower.hpp.

#include <crd/chir/lower.hpp>

#include <crd/ceir/ceir.hpp> // umbrella: Context / ir (Module/Operation/Block/Value/OpId) / dialect
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

namespace crd::chir
{
namespace
{
using crd::ceir::Block;
using crd::ceir::Context;
using crd::ceir::Module;
using crd::ceir::Operation;
using crd::ceir::OpId;
using crd::ceir::TypeId;
using crd::ceir::Value;
using crd::ceir::Visibility;
using crd::containers::Array;
using crd::containers::ConstSpan;

// The CEIR op kinds the lowering targets (interned once).
struct CeirOps
{
    OpId cst;
    OpId yield;
    OpId state;
    OpId parallel_for;
    OpId launch;
    OpId await;
    explicit CeirOps(Context& c)
        : cst(c.intern_op("arith", "const")), yield(c.intern_op("core", "yield")), state(c.intern_op("core", "state")),
          parallel_for(c.intern_op("task", "parallel_for")), launch(c.intern_op("async", "launch")),
          await(c.intern_op("async", "await"))
    {
    }
};

// The lowering walk. ⛔ It THREADS the CHIR dataflow: every CHIR out-pin that lowers to an SSA value is recorded, and
// each consumer resolves its in-pins THROUGH `m.edges()` — so a `parallel update (in entities = q.entities)` reads the
// actual producer (the query view), not a fabricated constant. An unconnected/unbacked in-pin falls back to a const.
// (The parity gate is then IDENTITY-sensitive: deleting the edges lowers to a DIFFERENT module — see test_chir_lower.)
class Lowering
{
public:
    Lowering(const SourceModel& m, Context& ctx, const CeirOps& o)
        : m_m(m), m_ctx(ctx), m_o(o), m_pins(ctx.allocator()), m_state_pins(ctx.allocator())
    {
    }

    Module* run()
    {
        const crd::u32 root = m_m.root();
        if (root == kInvalidNode) { return nullptr; }
        plan_state_pins(root); // ⛔ ADR-0128 D3: precompute each state cell's reserved-band, source-derived reload id
        Module* const mod   = m_ctx.create_module();
        Block*        mbody = mod->body()->first_block();
        if (mbody == nullptr)
        {
            mbody = m_ctx.create_block(0U);
            mod->body()->append(mbody);
        }
        const ChirNode& prog = m_m.node(root);
        for (crd::u32 ci = 0; ci < prog.children.size(); ++ci)
        {
            if (m_m.node(prog.children[ci]).kind == NodeKind::EventHandler)
            {
                lower_handler(*mod, mbody, root, prog.children[ci]);
            }
        }
        // ⛔ ADR-0128 D3 watermark-safety: reserve the band so assign_stable_ids gives every NON-state (sequential,
        // reload-invisible) op an id strictly ABOVE it — without this the sequential ids would track max(pinned)+1.
        if (m_state_pins.size() != 0U) { m_ctx.reserve_stable_id_floor(mod, kChirStateIdReserve); }
        return mod;
    }

private:
    // ── the CHIR out-pin -> CEIR Value table + the edge resolver ──
    struct PinVal
    {
        crd::u32 node = 0;
        crd::u32 pin  = 0;
        Value*   v    = nullptr;
    };
    // ⛔ ADR-0128 D3 (CEIR-32e): the reserved-band CEIR stable id a program-scope StateDecl's cell is pinned to.
    struct StatePin
    {
        crd::u32 node = 0;
        crd::u64 id   = 0;
    };
    void   record(crd::u32 node, crd::u32 pin, Value* v) { m_pins.push_back(PinVal{node, pin, v}); }
    Value* out_value(crd::u32 node, crd::u32 pin) const
    {
        for (crd::u32 i = 0; i < m_pins.size(); ++i)
        {
            if (m_pins[i].node == node && m_pins[i].pin == pin) { return m_pins[i].v; }
        }
        return nullptr;
    }
    // resolve an in-pin (node, in_pin) through the edges to the producing Value; nullptr if unconnected or unbacked.
    Value* in_value(crd::u32 node, crd::u32 in_pin) const
    {
        for (const Edge& e : m_m.edges())
        {
            if (e.to_node == node && e.to_pin == in_pin) { return out_value(e.from_node, e.from_pin); }
        }
        return nullptr;
    }
    [[nodiscard]] crd::u32 nth_in_pin(crd::u32 node, crd::u32 n) const // the index of the n-th In pin, or kInvalidNode
    {
        const ChirNode& nd = m_m.node(node);
        crd::u32        seen = 0;
        for (crd::u32 i = 0; i < nd.pins.size(); ++i)
        {
            if (nd.pins[i].dir == PinDir::In)
            {
                if (seen == n) { return i; }
                ++seen;
            }
        }
        return kInvalidNode;
    }

    // ── ADR-0128 D3 (CEIR-32e): the reload-stable state-cell id plan ──
    // Assign each program-scope StateDecl a source-derived, position-INDEPENDENT, watermark-safe CEIR stable id in the
    // reserved band [1, kChirStateIdReserve]. The seed is the decl's CHIR StableId (fnv1a(scope ‖ name) — already
    // position-independent), so a body/reorder/insert edit reproduces the SAME id (hot reload MIGRATES the value by id)
    // while a rename yields a DIFFERENT id (a fresh cell identity). Cells are processed in CHIR-StableId order and
    // linear-probed within the band, so a (negligible) hash collision resolves deterministically + position-independently
    // — never a silent alias, never a fail-to-lower.
    void plan_state_pins(crd::u32 program)
    {
        const ChirNode& prog = m_m.node(program);
        Array<crd::u32> decls(m_ctx.allocator());
        for (crd::u32 ci = 0; ci < prog.children.size(); ++ci)
        {
            if (m_m.node(prog.children[ci]).kind == NodeKind::StateDecl) { decls.push_back(prog.children[ci]); }
        }
        for (crd::u32 i = 1; i < decls.size(); ++i) // insertion sort by CHIR StableId (no std::sort) — the probe order
        {
            const crd::u32 key = decls[i];
            crd::u32       j   = i;
            while (j > 0U && m_m.node(decls[j - 1U]).id.value > m_m.node(key).id.value)
            {
                decls[j] = decls[j - 1U];
                --j;
            }
            decls[j] = key;
        }
        for (crd::u32 i = 0; i < decls.size(); ++i)
        {
            crd::u64 slot = crd::u64(1) + (m_m.node(decls[i]).id.value % kChirStateIdReserve);
            while (pin_taken(slot)) { slot = (slot % kChirStateIdReserve) + crd::u64(1); } // probe within [1, reserve]
            m_state_pins.push_back(StatePin{decls[i], slot});
        }
    }
    [[nodiscard]] bool pin_taken(crd::u64 id) const
    {
        for (crd::u32 i = 0; i < m_state_pins.size(); ++i)
        {
            if (m_state_pins[i].id == id) { return true; }
        }
        return false;
    }
    [[nodiscard]] crd::u64 pinned_id_for(crd::u32 node) const
    {
        for (crd::u32 i = 0; i < m_state_pins.size(); ++i)
        {
            if (m_state_pins[i].node == node) { return m_state_pins[i].id; }
        }
        return 0U; // an unplanned (e.g. non-program-scope) decl: StableId{0} is invalid => assign_stable_ids assigns it
    }
    // find the StateUpdate whose FIRST in-pin (the "cell") edges from this StateDecl's out-pin; kInvalidNode if none.
    [[nodiscard]] crd::u32 writing_update(crd::u32 st) const
    {
        crd::u32        st_out = kInvalidNode;
        const ChirNode& sn     = m_m.node(st);
        for (crd::u32 pi = 0; pi < sn.pins.size(); ++pi)
        {
            if (sn.pins[pi].dir == PinDir::Out) { st_out = pi; break; }
        }
        if (st_out == kInvalidNode) { return kInvalidNode; }
        for (const Edge& e : m_m.edges())
        {
            if (e.from_node == st && e.from_pin == st_out && m_m.node(e.to_node).kind == NodeKind::StateUpdate &&
                e.to_pin == nth_in_pin(e.to_node, 0U))
            {
                return e.to_node;
            }
        }
        return kInvalidNode;
    }

    // arith.const %v : t -> a Value (there is no core.const; arith.const is the corpus idiom). The bounds use !index to
    // match the induction var; state/async placeholders use i64.
    Value* konst(Block* b, crd::i64 v, TypeId t)
    {
        Operation* const op = m_ctx.create_operation(m_o.cst, {}, 1U, t);
        m_ctx.set_attr(op, "value", m_ctx.attr_int(v));
        b->append(op);
        return op->result(0U);
    }

    crd::u32 query_component_count(crd::u32 handler) const
    {
        const ChirNode& h = m_m.node(handler);
        for (crd::u32 ci = 0; ci < h.children.size(); ++ci)
        {
            const ChirNode& c = m_m.node(h.children[ci]);
            if (c.kind != NodeKind::Query) { continue; }
            for (crd::u32 ai = 0; ai < c.attrs.size(); ++ai)
            {
                if (m_m.str(c.attrs[ai].key) == StringView("components"))
                {
                    const StringView v = m_m.str(c.attrs[ai].val);
                    crd::u32         n = 1;
                    for (crd::usize i = 0; i < v.size(); ++i)
                    {
                        if (v[i] == '.') { ++n; }
                    }
                    return n;
                }
            }
            return 1U;
        }
        return 0U;
    }

    // Query -> the entity view = the func's block-args. Record each Query out-pin as a func param (the view the parallel
    // body reads). §143's `q.entities` -> func.arg(0); a Query with more out-pins than params clamps to the last.
    void lower_query(Block* fb, crd::u32 q)
    {
        const ChirNode& qn = m_m.node(q);
        const crd::u32  na = fb->num_args();
        crd::u32        oi = 0;
        for (crd::u32 pi = 0; pi < qn.pins.size(); ++pi)
        {
            if (qn.pins[pi].dir != PinDir::Out) { continue; }
            if (na != 0U) { record(q, pi, fb->arg(oi < na ? oi : na - 1U)); }
            ++oi;
        }
    }

    // ParallelFor -> task.parallel_for(0,1,1){ ^(%iv): core.yield <view> }. ⛔ D2: the body yields the RESOLVED first
    // in-pin (the query view, via the q->pf edge) — the structurally-correct read of the query result. Unconnected =>
    // yield %iv (self-contained). parallel_for has NO CEIR result, so pf's out-pins record nullptr (a documented CHIR-pin
    // -> no-CEIR-value mismatch: chir.md; self-containment seeding is the 32e/exec refinement).
    void lower_parallel_for(Block* fb, crd::u32 pf)
    {
        Value* const lo   = konst(fb, 0, m_ctx.type_index());
        Value* const hi   = konst(fb, 1, m_ctx.type_index());
        Value* const step = konst(fb, 1, m_ctx.type_index());
        Value* const pf_in[3] = {lo, hi, step};
        Operation* const pfo  = m_ctx.create_operation(m_o.parallel_for, ConstSpan<Value*>(pf_in, 3U), 0U, {}, 1U);
        Block* const     pb   = m_ctx.create_block(1U, m_ctx.type_index()); // ^(%iv : index)
        pfo->region(0)->append(pb);
        const crd::u32 first_in = nth_in_pin(pf, 0U);
        Value*         view     = (first_in != kInvalidNode) ? in_value(pf, first_in) : nullptr;
        if (view == nullptr) { view = pb->arg(0U); } // unconnected view => self-contained yield of %iv
        Value* const yv[1] = {view};
        pb->append(m_ctx.create_operation(m_o.yield, ConstSpan<Value*>(yv, 1U), 0U));
        fb->append(pfo);
    }

    // Await -> %tok = async.launch { core.yield 0 } ; %res = async.await(%tok). Records %res at the Await's out-pin so a
    // consumer (e.g. an update) can thread it. Token produced once + consumed once (well-formed).
    void lower_await(Block* fb, crd::u32 aw)
    {
        Operation* const lz = m_ctx.create_operation(m_o.launch, {}, 1U, m_ctx.type_i64(), 1U);
        Block* const     lb = m_ctx.create_block(0U);
        lz->region(0)->append(lb);
        Value* const ly[1] = {konst(lb, 0, m_ctx.type_i64())};
        lb->append(m_ctx.create_operation(m_o.yield, ConstSpan<Value*>(ly, 1U), 0U));
        fb->append(lz);
        Value* const tok[1] = {lz->result(0U)};
        Operation* const aop = m_ctx.create_operation(m_o.await, ConstSpan<Value*>(tok, 1U), 1U, m_ctx.type_i64());
        fb->append(aop);
        // record the result at the Await's first Out pin (if any).
        const ChirNode& an = m_m.node(aw);
        for (crd::u32 pi = 0; pi < an.pins.size(); ++pi)
        {
            if (an.pins[pi].dir == PinDir::Out)
            {
                record(aw, pi, aop->result(0U));
                break;
            }
        }
    }

    // StateDecl -> ONE core.state(%init, %next) -> %current cell (ADR-0128 D1: the cell belongs to the DECLARATION, so the
    // §143 `update state` write FOLDS into this cell's feedback rather than emitting a second cell — the CEIR-32e
    // unification of the 32d two-cell placeholder). The cell is PINNED to a source-derived, reload-stable id (D3, see
    // plan_state_pins). %init = konst 0 (no v1 initializer). %next = the §20 feedback = the value the `update state` writes:
    // resolved through the writing StateUpdate's `updated` in-pin (a parallel_for source has NO CEIR result, so it falls
    // back to konst 1 — DISTINCT from %init, so deleting the verb / its cell edge is OBSERVABLE); with NO writing update at
    // all, %next = %init (the cell simply holds). Carries the decl's source NAME in `chir_decl` (the 32d D3 seam this consumes).
    void lower_state_decl(Block* fb, crd::u32 st)
    {
        Value* const   init = konst(fb, 0, m_ctx.type_i64());
        Value*         next = init;
        const crd::u32 su   = writing_update(st);
        if (su != kInvalidNode)
        {
            const crd::u32 upd = nth_in_pin(su, 1U); // the update's SECOND in-pin = the `updated` value written to the cell
            next               = (upd != kInvalidNode) ? in_value(su, upd) : nullptr;
            if (next == nullptr) { next = konst(fb, 1, m_ctx.type_i64()); }
        }
        Value* const     st_in[2] = {init, next};
        Operation* const cell     = m_ctx.create_operation(m_o.state, ConstSpan<Value*>(st_in, 2U), 1U, m_ctx.type_i64());
        m_ctx.set_attr(cell, "chir_decl", m_ctx.attr_string(m_m.str(m_m.node(st).name)));
        m_ctx.pin_stable_id(cell, StableId{pinned_id_for(st)}); // ⛔ ADR-0128 D3 — the reload-stable source-derived cell id
        fb->append(cell);
        const ChirNode& sn = m_m.node(st);
        for (crd::u32 pi = 0; pi < sn.pins.size(); ++pi)
        {
            if (sn.pins[pi].dir == PinDir::Out)
            {
                record(st, pi, cell->result(0U));
                break;
            }
        }
    }

    // EventHandler -> func.func (params = the query view) + a time-domain attr; state cells for the program-scope
    // StateDecls, then the handler's body statements in source order.
    void lower_handler(Module& mod, Block* mbody, crd::u32 program, crd::u32 handler)
    {
        const ChirNode& h     = m_m.node(handler);
        const crd::u32  ncomp = query_component_count(handler);
        Operation* const fn =
            crd::ceir::func::create_func(m_ctx, mod, m_m.str(h.name), Visibility::Public, ncomp, m_ctx.type_i64());
        if (fn == nullptr) { return; } // a duplicate handler name — skip (create_func's no-overwrite contract)
        for (crd::u32 ai = 0; ai < h.attrs.size(); ++ai)
        {
            if (m_m.str(h.attrs[ai].key) == StringView("domain")) // the ADR-0116 typed time domain; NO new event op
            {
                m_ctx.set_attr(fn, "domain", m_ctx.attr_string(m_m.str(h.attrs[ai].val)));
            }
        }
        mbody->append(fn);
        Block* const fb = crd::ceir::func::func_body_block(fn);
        // ⛔ ADR-0128 D3 duplicate-pin guard: the program-scope state cells are emitted ONCE (in the FIRST handler's func).
        // Emitting them per-handler would pin the SAME source id onto two ops (a duplicate migration key = state
        // corruption). §143 has ONE handler; multi-handler SHARED program state is a named future (the cells live here).
        if (!m_decls_emitted)
        {
            const ChirNode& prog = m_m.node(program);
            for (crd::u32 ci = 0; ci < prog.children.size(); ++ci)
            {
                if (m_m.node(prog.children[ci]).kind == NodeKind::StateDecl) { lower_state_decl(fb, prog.children[ci]); }
            }
            m_decls_emitted = true;
        }
        for (crd::u32 ci = 0; ci < h.children.size(); ++ci)
        {
            const crd::u32 child = h.children[ci];
            switch (m_m.node(child).kind)
            {
            case NodeKind::Query: lower_query(fb, child); break;
            case NodeKind::ParallelFor: lower_parallel_for(fb, child); break;
            case NodeKind::Await: lower_await(fb, child); break;
            case NodeKind::StateUpdate:    // ⛔ CEIR-32e collapse: folds into its cell's %next (lower_state_decl); no op
            case NodeKind::StateDecl:      // handled above (program scope, once)
            case NodeKind::Program:        // never a handler child
            case NodeKind::EventHandler: break; // nested handlers are not a v1 construct
            }
        }
        fb->append(crd::ceir::func::create_return(m_ctx, {}));
    }

    const SourceModel& m_m;
    Context&           m_ctx;
    const CeirOps&     m_o;
    Array<PinVal>      m_pins;
    Array<StatePin>    m_state_pins;   // ADR-0128 D3: program-scope StateDecl -> reserved-band reload id
    bool               m_decls_emitted = false; // the program-scope decls are pinned+emitted once (dup-pin guard)
};
} // namespace

Module* lower_chir(const SourceModel& m, Context& ctx)
{
    // register the dialects this lowering targets (idempotent — a re-register returns the existing dialect).
    (void)crd::ceir::func::register_dialect(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::task::register_task_ops(ctx);
    (void)crd::ceir::async::register_async_ops(ctx);
    const CeirOps o(ctx);
    return Lowering(m, ctx, o).run();
}

} // namespace crd::chir
