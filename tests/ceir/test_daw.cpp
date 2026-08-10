// CEIR-9b (UNIVERSALITY VALIDATION, U2 DAW/timeline, U-§83/U-§38/U-§39). ⛔ A PROOF, not a feature: a DAW/audio graph
// runs on the CEIR foundation with ZERO new machinery — the 8f typed TIME domains ARE the sample clock, the 5d §20
// explicit-STATE/delay verifier IS the feedback discipline, the 8f safety axes + 4c region legality ARE realtime
// safety, the 8e op-interfaces ARE latency metadata. The mock `audio` dialect is INLINE-registered (zero central edits
// — the open-world proof). Five composed proofs: (1) sample-accurate time is a distinct TypeId from wall + a plugin
// beat clock; (2) feedback through the PLUGIN delay op (which merely carries OpTrait::StateEdge) is legal while a
// combinational loop is FeedbackWithoutState — the 5d verifier keys on the TRAIT, never a kind name, so a plugin's
// delay is exactly as legal as core.delay; (3) a disk load in a realtime audio region is flagged, and the OFFLINE
// render of the SAME module (an in-place region-tag flip) is legal — offline is not a second graph; (4) the two-RT
// oracles subset (realtime_safe ⟹ legal, never converse — an allocating op is the witness); (5) latency is a queryable
// interface summed along the chain, and a MISSING interface makes the sum UNKNOWN (EMPTY≠UNKNOWN, U-§39). Host-only.
// ASCII test names.

#include <crd/ceir/ceir.hpp>      // umbrella: context/ir/dialect/interface
#include <crd/ceir/effect.hpp>    // EffectRecord / EffectFamily
#include <crd/ceir/hazard.hpp>    // SafetyBits::realtime_safe
#include <crd/ceir/semantics.hpp> // RegionExec / effect_legal_in_region / DeterminismClass / EvalDomain / RealtimeClass
#include <crd/ceir/time.hpp>      // the 8f time-domain dialect

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::u32;

namespace
{
bool verify_one_member(const Context&, const Type& t) noexcept { return t.members.size() == 1U; }

// U-§39: latency/lookahead/tail carried as a TYPED op-interface (8e) — the compiler queries it with zero hard-coded op
// knowledge. The impls are `static constexpr` so they outlive the Context (the 8e function-table contract).
struct LatencyInterface
{
    u32                          latency;
    u32                          lookahead;
    u32                          tail;
    static constexpr InterfaceId kId = make_interface_id("crd.iface.audio_latency");
};
constexpr LatencyInterface kLatZero{0U, 0U, 0U};
constexpr LatencyInterface kLatDelay{64U, 0U, 64U};

struct AudioOps
{
    OpId source, gain, delay, mix, load_sample, scratch_alloc, graph, no_latency;
};
// The mock `audio` dialect — INLINE-registered (zero central-enum edits). Every op sets EXPLICIT determinism + domain
// axes (⛔ the 4z Unspecified-default trap applies to proof fixtures). `delay` carries OpTrait::StateEdge (a DOMAIN op
// inheriting the 5d feedback exemption via the trait); `load_sample` a FileIO effect; `scratch_alloc` an Allocate effect.
AudioOps register_audio(Context& ctx)
{
    Dialect* const     d        = ctx.register_dialect("audio");
    const EffectRecord memr[1]  = {EffectRecord{EffectFamily::MemoryRead}};
    const EffectRecord fileio[1] = {EffectRecord{EffectFamily::FileIO}};
    const EffectRecord alloc[1] = {EffectRecord{EffectFamily::Allocate}};
    AudioOps           a{};
    a.source = d->register_op("source", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::HostAudioTime});
    a.gain   = d->register_op("gain", OpSpec{.effects      = ConstSpan<EffectRecord>(memr, 1U),
                                             .determinism  = DeterminismClass::BitExact,
                                             .domain       = EvalDomain::HostAudioTime});
    a.delay  = d->register_op("delay", OpSpec{.traits      = flags_of(OpTrait::StateEdge),
                                              .determinism = DeterminismClass::BitExact,
                                              .domain      = EvalDomain::HostAudioTime});
    a.mix    = d->register_op("mix", OpSpec{.effects     = ConstSpan<EffectRecord>(memr, 1U),
                                            .determinism = DeterminismClass::BitExact,
                                            .domain      = EvalDomain::HostAudioTime});
    a.load_sample = d->register_op("load_sample", OpSpec{.effects     = ConstSpan<EffectRecord>(fileio, 1U),
                                                         .determinism = DeterminismClass::BitExact,
                                                         .domain      = EvalDomain::HostAudioTime});
    a.scratch_alloc = d->register_op("scratch_alloc", OpSpec{.effects     = ConstSpan<EffectRecord>(alloc, 1U),
                                                             .determinism = DeterminismClass::BitExact,
                                                             .domain      = EvalDomain::HostAudioTime});
    a.graph      = d->register_op("graph", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::HostAudioTime});
    a.no_latency = d->register_op("plugin_no_latency", OpSpec{.determinism = DeterminismClass::BitExact, .domain = EvalDomain::HostAudioTime});
    register_op_interface<LatencyInterface>(ctx, a.source, &kLatZero);
    register_op_interface<LatencyInterface>(ctx, a.gain, &kLatZero);
    register_op_interface<LatencyInterface>(ctx, a.delay, &kLatDelay);
    register_op_interface<LatencyInterface>(ctx, a.mix, &kLatZero);
    // a.no_latency deliberately has NO LatencyInterface — the EMPTY≠UNKNOWN witness.
    return a;
}
} // namespace

TEST_CASE("ceir 9b: sample-accurate audio time is a distinct type from wall and a plugin beat clock", "[ceir][daw]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    (void)time::register_dialect(ctx);
    const TypeId u        = ctx.type_i64();
    const TypeId t_audio  = time::time_type(ctx, time::domain(ctx, "audio_sample"), u);
    const TypeId t_wall   = time::time_type(ctx, time::domain(ctx, "wall"), u);
    CHECK(t_audio != t_wall);                                                       // sample time != wall time
    CHECK(t_audio == time::time_type(ctx, time::domain(ctx, "audio_sample"), u));   // dedup (same domain + underlying)

    // a PLUGIN tempo/beat clock — a registered type-class under a plugin dialect, ZERO central edits.
    const TypeClassId beat   = ctx.register_dialect("tempo")->register_type_class("beat", TypeClassSpec{&verify_one_member, 1U});
    const TypeId      t_beat = time::time_type(ctx, beat, u);
    CHECK(t_beat != t_audio);
    CHECK(t_beat != t_wall);
}

TEST_CASE("ceir 9b: feedback through a plugin delay is legal; a combinational loop is rejected", "[ceir][daw]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const AudioOps               a  = register_audio(ctx);
    (void)time::register_dialect(ctx);
    const TypeId audio = time::time_type(ctx, time::domain(ctx, "audio_sample"), ctx.type_i64());

    // LEGAL: source -> delay(init, next) -> gain -> (feedback into delay's LAST operand). `delay` is a PLUGIN op that
    // merely carries OpTrait::StateEdge, so the 5d verifier (which keys on the TRAIT, not a kind name) exempts its
    // last operand from def-before-use — a plugin delay is exactly as legal as core.delay.
    {
        Module* const    m   = ctx.create_module();
        Block* const     b   = ctx.create_block(0U);
        m->body()->append(b);
        Operation* const src = ctx.create_operation(a.source, {}, 1U, audio);
        b->append(src);
        Value* const     dly_ops[2] = {src->result(0), src->result(0)}; // (init, next placeholder)
        Operation* const dly        = ctx.create_operation(a.delay, ConstSpan<Value*>(dly_ops, 2U), 1U, audio);
        b->append(dly);
        Value* const     gn_ops[1] = {dly->result(0)};
        Operation* const gn        = ctx.create_operation(a.gain, ConstSpan<Value*>(gn_ops, 1U), 1U, audio);
        b->append(gn);
        dly->set_operand(1U, gn->result(0)); // the feedback edge: next = the gain output (defined LATER, same block)
        CHECK(ctx.find_structure_error(*m).kind == StructureErrorKind::None);
    }
    // ILLEGAL: the same loop routed through `gain` (NOT a StateEdge op) — a combinational feedback cycle.
    {
        Module* const    m    = ctx.create_module();
        Block* const     b    = ctx.create_block(0U);
        m->body()->append(b);
        Operation* const src  = ctx.create_operation(a.source, {}, 1U, audio);
        b->append(src);
        Value* const     g1_ops[1] = {src->result(0)};
        Operation* const gn1       = ctx.create_operation(a.gain, ConstSpan<Value*>(g1_ops, 1U), 1U, audio);
        b->append(gn1);
        Value* const     g2_ops[1] = {gn1->result(0)};
        Operation* const gn2       = ctx.create_operation(a.gain, ConstSpan<Value*>(g2_ops, 1U), 1U, audio);
        b->append(gn2);
        gn1->set_operand(0U, gn2->result(0)); // gn1 uses gn2 (defined LATER) — no StateEdge -> combinational
        const StructureError e = ctx.find_structure_error(*m);
        CHECK(e.kind == StructureErrorKind::FeedbackWithoutState);
        CHECK(e.op == gn1); // the pointing contract: the offender is the gain carrying the back-edge
    }
}

TEST_CASE("ceir 9b: a disk load in a realtime audio region is flagged; the offline render of the same graph is legal", "[ceir][daw]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const AudioOps               a     = register_audio(ctx);
    const TypeId                 audio = ctx.type_i64();

    // one audio.graph region holding a disk load (FileIO) — the classic "load a sample in the RT callback" sin.
    Module* const    m     = ctx.create_module();
    Block* const     top   = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const graph = ctx.create_operation(a.graph, {}, 0U, {}, 1U);
    top->append(graph);
    Block* const     inner = ctx.create_block(0U);
    graph->region(0)->append(inner);
    Operation* const load = ctx.create_operation(a.load_sample, {}, 1U, audio);
    inner->append(load);

    // REALTIME: the FileIO op is illegal in the audio-RT region.
    ctx.set_region_exec(graph, RegionExec{EvalDomain::HostAudioTime, RealtimeClass::AudioRealTime});
    const DomainViolation v = ctx.find_domain_violation(*m);
    CHECK(v.op == load);
    CHECK(v.effect == EffectFamily::FileIO);

    // OFFLINE = the SAME module object, an in-place region-tag flip (NOT a second graph — U-§38). The disk load is legal.
    ctx.set_region_exec(graph, RegionExec{EvalDomain::OfflineTime, RealtimeClass::Offline});
    CHECK(ctx.find_domain_violation(*m).op == nullptr);

    // symmetry: flip back to realtime -> flagged again (same ops, same structure, only the tag differs).
    ctx.set_region_exec(graph, RegionExec{EvalDomain::HostAudioTime, RealtimeClass::AudioRealTime});
    CHECK(ctx.find_domain_violation(*m).op == load);
}

TEST_CASE("ceir 9b: realtime-safe implies legal but not conversely (the two-oracles subset)", "[ceir][daw]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const AudioOps               a     = register_audio(ctx);
    const RegionExec             audio{EvalDomain::HostAudioTime, RealtimeClass::AudioRealTime};

    // Three data points pin the subset (two would leave it a coincidence):
    //   gain  (MemoryRead) -> realtime_safe TRUE , legal TRUE
    //   alloc (Allocate)   -> realtime_safe FALSE, legal TRUE   <- the witness the converse fails
    //   load  (FileIO)     -> realtime_safe FALSE, legal FALSE
    const bool gain_safe  = ctx.op_safety(a.gain).realtime_safe();
    const bool alloc_safe = ctx.op_safety(a.scratch_alloc).realtime_safe();
    const bool load_safe  = ctx.op_safety(a.load_sample).realtime_safe();
    const bool gain_legal  = effect_legal_in_region(EffectFamily::MemoryRead, audio);
    const bool alloc_legal = effect_legal_in_region(EffectFamily::Allocate, audio);
    const bool load_legal  = effect_legal_in_region(EffectFamily::FileIO, audio);

    CHECK(gain_safe);
    CHECK(gain_legal);
    CHECK_FALSE(alloc_safe);
    CHECK(alloc_legal); // the allocating op is LEGAL (a soft cost) yet NOT realtime-safe
    CHECK_FALSE(load_safe);
    CHECK_FALSE(load_legal);

    // realtime_safe ⟹ legal for every op; the converse fails at the allocating op.
    CHECK((!gain_safe || gain_legal));
    CHECK((!alloc_safe || alloc_legal));
    CHECK((!load_safe || load_legal));
    CHECK((alloc_legal && !alloc_safe)); // the explicit converse-witness
}

TEST_CASE("ceir 9b: latency is a queryable interface summed along the chain; a missing interface is unknown", "[ceir][daw]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const AudioOps               a = register_audio(ctx);

    // sum the plugin-declared latency along a chain via the 8e interface — ZERO hard-coded op knowledge. A MISSING
    // interface makes the sum UNKNOWN (EMPTY≠UNKNOWN), not silently 0 — the mark of a semantic interface, not a decoration.
    const auto chain_latency = [&](ConstSpan<OpId> kinds, bool& known) {
        u32 sum = 0U;
        known   = true;
        for (crd::usize i = 0; i < kinds.size(); ++i)
        {
            const LatencyInterface* const li = get_op_interface<LatencyInterface>(ctx, kinds[i]);
            if (li == nullptr) { known = false; } // an op with no declared latency -> the chain latency is UNKNOWN
            else { sum += li->latency; }
        }
        return sum;
    };

    // source(0) -> gain(0) -> delay(64) -> mix(0): total latency 64, fully known.
    const OpId full_chain[4] = {a.source, a.gain, a.delay, a.mix};
    bool       known1        = false;
    CHECK(chain_latency(ConstSpan<OpId>(full_chain, 4U), known1) == 64U);
    CHECK(known1);

    // a chain containing an op WITHOUT the latency interface -> UNKNOWN.
    const OpId partial[3] = {a.source, a.no_latency, a.delay};
    bool       known2     = false;
    (void)chain_latency(ConstSpan<OpId>(partial, 3U), known2);
    CHECK_FALSE(known2);
}
