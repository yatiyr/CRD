// ceir_audio_render.cpp — CEIR-31a-1b-ii: the CEIR audio executor (see ceir_audio_render.hpp).

#include <crd/audio/ceir_audio_render.hpp>

#include <crd/audio/audio_kernels.hpp> // apply_source / apply_gain / apply_biquad / apply_delay / apply_compressor (SHARED)
#include <crd/ceir/attr.hpp>           // AttrValue / AttrKind
#include <crd/ceir/id.hpp>             // AttrId
#include <crd/ceir/ir.hpp>             // Region / Block / Operation / Value
#include <crd/ceir/type.hpp>           // Type / DimKind (the 31z shape-vs-frames execute-time check)
#include <crd/containers/hash_map.hpp>

namespace crd::audio
{
namespace
{
    using crd::ceir::Block;
    using crd::ceir::Operation;
    using crd::ceir::Region;
    using crd::containers::StringView;

    // the single func.func in the module body (the audio graph lives in its body region). null if absent.
    Operation* find_main_func(const crd::ceir::Context& ctx, const crd::ceir::Module& m)
    {
        Region* const body = m.body();
        if (body == nullptr) { return nullptr; }
        for (Block* b = body->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
            {
                if (ctx.op_name(op->kind()) == StringView("func.func")) { return op; }
            }
        }
        return nullptr;
    }

    const GraphSourceBinding* binding_for(crd::containers::ConstSpan<CeirSourceBinding> bindings, StringView name)
    {
        for (const CeirSourceBinding& b : bindings)
        {
            if (b.name == name) { return &b.binding; }
        }
        return nullptr;
    }

    // Attr readers that check `.valid()` FIRST (absent reads as Int 0 -- the attr-reader-check-valid scar) + the exact
    // KIND: the module already passed find_audio_misuse + the generated verify_*, but the executor rejects at the read
    // site anyway (a present-but-wrong-kind attr is a `false`, never a silent degenerate coefficient).
    [[nodiscard]] bool read_f32(const crd::ceir::Context& ctx, const Operation* op, const char* name, crd::f32& out)
    {
        const crd::ceir::AttrId a = op->attr(name);
        if (!a.valid()) { return false; }
        const crd::ceir::AttrValue v = ctx.attr_value(a);
        if (v.kind != crd::ceir::AttrKind::Float) { return false; }
        out = static_cast<crd::f32>(v.as_float());
        return true;
    }
    [[nodiscard]] bool read_i64(const crd::ceir::Context& ctx, const Operation* op, const char* name, crd::i64& out)
    {
        const crd::ceir::AttrId a = op->attr(name);
        if (!a.valid()) { return false; }
        const crd::ceir::AttrValue v = ctx.attr_value(a);
        if (v.kind != crd::ceir::AttrKind::Int) { return false; }
        out = v.i;
        return true;
    }
    [[nodiscard]] bool read_sym(const crd::ceir::Context& ctx, const Operation* op, const char* name, StringView& out)
    {
        const crd::ceir::AttrId a = op->attr(name);
        if (!a.valid()) { return false; }
        const crd::ceir::AttrValue v = ctx.attr_value(a);
        if (v.kind != crd::ceir::AttrKind::SymbolRef) { return false; }
        out = v.s;
        return true;
    }

    // The ceir.audio op set -- the SINGLE enumeration (PASS 1 admits, PASS 2 dispatches + asserts the set matches). ⛔ a
    // new node goes HERE + in the PASS 2 dispatch, and the PASS 2 final-else assert catches a mismatch (the widening-enum
    // scar: two enumerations of one set drift silently -- 31a-2a added audio.delay to the dispatch but not the allow-list).
    // All six process a stereo bus; the graph-global sample_rate audio.compressor needs is a func.func attr (read once in
    // execute_audio_graph_ceir), NOT a body op -- a 0-result meta op would false-read as a second unconsumed sink.
    [[nodiscard]] bool is_audio_op(StringView nm)
    {
        return nm == StringView("audio.source") || nm == StringView("audio.gain") || nm == StringView("audio.send") ||
               nm == StringView("audio.biquad") || nm == StringView("audio.mix") || nm == StringView("audio.delay") ||
               nm == StringView("audio.compressor");
    }
} // namespace

crd::i64 execute_audio_graph_ceir(const crd::ceir::Context& ctx, const crd::ceir::Module& module,
                                  crd::containers::ConstSpan<CeirSourceBinding> bindings, crd::i64 frames,
                                  crd::containers::Array<crd::f32>& out)
{
    out.clear();
    if (frames <= 0) { return 0; }
    Operation* const fn = find_main_func(ctx, module);
    if (fn == nullptr || fn->num_regions() == 0U) { return 0; }
    Block* const body = fn->region(0)->first_block();
    if (body == nullptr) { return 0; }

    // the graph-global sample_rate (an OPTIONAL func attr -- rides the GRAPH not this exec call, so the authored DSP carries
    // the rate it was designed at; audio.compressor's ms->coeff needs it). ABSENT -> 48000 (the AudioGraphResource default);
    // present-but-non-Int-or-<=0 -> graceful reject (NOT a silent default -- the graceful-reject scar). Read ONCE, not per op.
    crd::u32 sample_rate = 48000U;
    {
        const crd::ceir::AttrId sra = fn->attr("sample_rate");
        if (sra.valid())
        {
            const crd::ceir::AttrValue srv = ctx.attr_value(sra);
            if (srv.kind != crd::ceir::AttrKind::Int || srv.i <= 0) { return 0; }
            sample_rate = static_cast<crd::u32>(srv.i);
        }
    }

    crd::memory::IAllocator* const alloc = out.allocator();
    const crd::usize              span  = static_cast<crd::usize>(frames) * 2U;

    // PASS 1: assign each audio op a stereo-bus slot (authoring order = a valid topological order for the DAG); a
    // non-audio op in the body is a malformed audio graph.
    crd::containers::HashMap<const Operation*, crd::u32> slot(alloc);
    crd::u32                                             n = 0;
    for (Operation* op = body->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (!is_audio_op(ctx.op_name(op->kind()))) { return 0; } // a non-audio op in the body = a malformed audio graph
        slot.insert(op, n++);
    }
    if (n == 0) { return 0; }

    crd::containers::Array<crd::f32> bus(alloc); // one [frames x 2] stereo bus per node, flat (render_graph's shape)
    bus.resize(static_cast<crd::usize>(n) * span, 0.0F);
    crd::containers::HashMap<const Operation*, bool> consumed(alloc);

    // PASS 2: every node SUMS its input operands (operand order == edge order, the non-associative f32 sum), then applies
    // its op kernel. Attr reads narrow float attrs through f32 -- the apply_* signatures ENFORCE this (the AGRF stores
    // cutoff/q/gain_db as f32, so the coefficients must be computed from the f32-narrowed value).
    for (Operation* op = body->first_op(); op != nullptr; op = op->next_in_block())
    {
        const crd::u32  my   = *slot.find(op);
        crd::f32* const mine = bus.data() + static_cast<crd::usize>(my) * span;
        for (crd::u32 i = 0; i < op->num_operands(); ++i)
        {
            Operation* const   src = op->operand(i)->defining_op();
            const crd::u32* const s = (src != nullptr) ? slot.find(src) : nullptr;
            if (s == nullptr) { return 0; } // an operand that is not an earlier audio op
            if (*s >= my) { return 0; }     // ⛔ 31z (2): a BACK-EDGE (operand defined at/after this op) = a feedback cycle.
                                            // This single-pass topological walk cannot run graph feedback (the source bus is
                                            // not yet computed), and render_graph refuses cycles too (Kahn, audio_resources.cpp).
                                            // The delay/biquad StateEdge trait makes the 5d VERIFIER accept a delay-headed
                                            // back-edge (structural legality); the EXECUTOR refuses to RUN it (no bit-exact
                                            // oracle — sample-interleaved SCC eval is a future capability, not a close item).
            const crd::f32* const theirs = bus.data() + static_cast<crd::usize>(*s) * span;
            for (crd::usize k = 0; k < span; ++k) { mine[k] += theirs[k]; }
            consumed.insert(src, true);
        }

        const StringView nm = ctx.op_name(op->kind());
        if (nm == StringView("audio.source"))
        {
            StringView sname;
            if (!read_sym(ctx, op, "name", sname)) { return 0; }
            const GraphSourceBinding* const b = binding_for(bindings, sname);
            if (b == nullptr || b->samples.size() == 0 || b->channels == 0) { return 0; }
            crd::i64 start_frame = 0;
            crd::i64 loopv       = 0;
            if (!read_i64(ctx, op, "start_frame", start_frame) || !read_i64(ctx, op, "loop", loopv)) { return 0; }
            apply_source(mine, 0, frames, *b, start_frame, loopv != 0);
        }
        else if (nm == StringView("audio.gain") || nm == StringView("audio.send"))
        {
            crd::f32 gain_db = 0.0F;
            if (!read_f32(ctx, op, "gain_db", gain_db)) { return 0; }
            apply_gain(mine, frames, gain_db);
        }
        else if (nm == StringView("audio.biquad"))
        {
            crd::i64 filter = 0;
            crd::f32 cutoff = 0.0F;
            crd::f32 q      = 0.0F;
            if (!read_i64(ctx, op, "filter", filter) || !read_f32(ctx, op, "cutoff", cutoff) ||
                !read_f32(ctx, op, "q", q))
            {
                return 0;
            }
            crd::f64 z1[2] = {};
            crd::f64 z2[2] = {};
            apply_biquad(mine, frames, static_cast<crd::u8>(filter), cutoff, q, z1, z2);
        }
        else if (nm == StringView("audio.delay"))
        {
            crd::i64 delay_frames = 0;
            if (!read_i64(ctx, op, "delay_frames", delay_frames)) { return 0; }
            apply_delay(mine, frames, delay_frames);
        }
        else if (nm == StringView("audio.compressor"))
        {
            crd::f32 threshold_db = 0.0F;
            crd::f32 ratio        = 0.0F;
            crd::f32 attack_ms    = 0.0F;
            crd::f32 release_ms   = 0.0F;
            if (!read_f32(ctx, op, "threshold_db", threshold_db) || !read_f32(ctx, op, "ratio", ratio) ||
                !read_f32(ctx, op, "attack_ms", attack_ms) || !read_f32(ctx, op, "release_ms", release_ms))
            {
                return 0;
            }
            crd::f64 env = 0.0; // one linked envelope, local per op (single-pass; passed IN like biquad z1/z2 so a future
                                // chunked caller could thread it across blocks -- the 31a-1b-i "state passed in" argument)
            apply_compressor(mine, frames, threshold_db, ratio, attack_ms, release_ms, sample_rate, env);
        }
        else if (nm == StringView("audio.mix")) { /* the input operands are already summed above -- nothing more */ }
        else
        {
            CRD_ASSERT_MSG(false, "is_audio_op admitted an op the PASS-2 dispatch does not handle"); // set drift guard
            return 0;
        }
    }

    // the OUTPUT sink = the unique audio op whose result is unconsumed (a convention; audio.output the eventual contract).
    const Operation* sink = nullptr;
    for (Operation* op = body->first_op(); op != nullptr; op = op->next_in_block())
    {
        if (consumed.find(op) == nullptr)
        {
            if (sink != nullptr) { return 0; } // >1 unconsumed result -- an ambiguous output
            sink = op;
        }
    }
    if (sink == nullptr) { return 0; }

    // ⛔ 31z ruling (5): the output tensor's FRAME extent, WHEN PINNED (a Static dim0), IS the block-size contract — reject a
    // `frames` that disagrees (the declared-words-validated family; a Dynamic dim0 is the runtime block size, so any `frames`).
    {
        const crd::ceir::Type ot = ctx.type_of(sink->result(0U)->type());
        if (ot.members.size() >= 2U) // members[0] = element, members[1] = shape
        {
            const crd::ceir::Type os = ctx.type_of(ot.members[1U]);
            if (os.members.size() >= 1U)
            {
                const crd::ceir::Type d0 = ctx.type_of(os.members[0U]); // dim0 = frames
                if (static_cast<crd::ceir::DimKind>(d0.cols) == crd::ceir::DimKind::Static &&
                    static_cast<crd::i64>(d0.count) != frames)
                {
                    return 0;
                }
            }
        }
    }

    out.resize(span, 0.0F);
    const crd::f32* const fb = bus.data() + static_cast<crd::usize>(*slot.find(sink)) * span;
    for (crd::usize k = 0; k < span; ++k) { out[k] = fb[k]; }
    return frames;
}

} // namespace crd::audio
