// CEIR-4c — the eval-domain (sec 15) + realtime-class (sec 32) gate. A per-op-KIND EvalDomain rides OpInfo (declared via
// the 2a schema); a per-REGION (domain + realtime) tag rides a packed `region_exec` attr on the region-owning op. The
// domain-legality verifier composes the 4a EFFECTS with the enclosing region's tag: a filesystem / blocking-network
// effect in a real-time-audio region is rejected with a pointing diagnostic. Proves op_domain round-trip + empty!=unknown,
// the region_exec pack/round-trip + attr survival (module content), the legality matrix, and find_domain_violation
// (registered effect + unregistered-op conservatism + nested innermost-override + corrupt tag). Host-only, ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/test_ops.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;
using crd::containers::ConstSpan;

namespace
{
// a hand "io" dialect: `read` carries a FileIO effect, `send` a NetworkIO effect, `hold` is an effect-free region-holder.
struct IoOps
{
    OpId read, send, hold;
    explicit IoOps(Context& ctx)
    {
        Dialect* const d = ctx.register_dialect("io");
        read             = d->register_op("read", {.effects = ConstSpan<EffectRecord>(&kFile, 1U)});
        send             = d->register_op("send", {.effects = ConstSpan<EffectRecord>(&kNet, 1U)});
        hold             = d->register_op("hold", {}); // effect-free container
    }
    static constexpr EffectRecord kFile{EffectFamily::FileIO, EffectTarget::None, 0U, 0U};
    static constexpr EffectRecord kNet{EffectFamily::NetworkIO, EffectTarget::None, 0U, 0U};
};

// a region-holder op tagged `tag`, appended to `parent`, with one empty child region returned via `inner`.
Operation* tagged_region(Context& ctx, const IoOps& io, Block* parent, const RegionExec& tag, Block*& inner)
{
    Operation* const owner = ctx.create_operation(io.hold, {}, 0U, {}, 1U);
    parent->append(owner);
    ctx.set_region_exec(owner, tag);
    inner = ctx.create_block(0U);
    owner->region(0)->append(inner);
    return owner;
}
} // namespace

TEST_CASE("ceir domain: op_domain round-trips the 2a schema; EMPTY!=UNKNOWN", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    (void)test::register_test_ops(ctx);

    CHECK(ctx.op_domain(ctx.intern_op("arith", "addi")) == EvalDomain::EitherHostOrDevice);
    CHECK(ctx.op_domain(ctx.intern_op("test", "dummy")) == EvalDomain::HostFrameTime);
    CHECK(ctx.op_domain(ctx.intern_op("test", "kinds")) == EvalDomain::Unspecified); // undeclared

    const OpId unknown = ctx.intern_op("plugin", "widget");
    CHECK(ctx.op_domain(unknown) == EvalDomain::Unspecified);
    CHECK(ctx.op_info(unknown) == nullptr); // Unspecified here means UNKNOWN, not "declared none"
}

TEST_CASE("ceir domain: region_exec packs/round-trips and rejects out-of-range", "[ceir][domain]")
{
    const RegionExec r{EvalDomain::DeviceTime, RealtimeClass::AudioRealTime};
    RegionExec       back;
    REQUIRE(unpack_region_exec(pack_region_exec(r), back));
    CHECK(back == r);
    RegionExec def; // untagged packs to 0
    CHECK(pack_region_exec(def) == 0);

    RegionExec junk;
    CHECK_FALSE(unpack_region_exec(static_cast<crd::i64>(0xBULL), junk));          // domain nibble 11 > max 10
    CHECK_FALSE(unpack_region_exec(static_cast<crd::i64>(0x8ULL << 4U), junk));    // realtime nibble 8 > max 7
    CHECK_FALSE(unpack_region_exec(static_cast<crd::i64>(1ULL << 8U), junk));      // a bit above the defined 8
}

TEST_CASE("ceir domain: effect_legal_in_region enforces the sec 32 audio rule (matrix)", "[ceir][domain]")
{
    const RegionExec audio{EvalDomain::Unspecified, RealtimeClass::AudioRealTime};
    const RegionExec device{EvalDomain::DeviceTime, RealtimeClass::Unspecified};
    const RegionExec hostaudio{EvalDomain::HostAudioTime, RealtimeClass::Unspecified};
    const RegionExec frame{EvalDomain::HostFrameTime, RealtimeClass::FrameCritical};
    const RegionExec untagged;

    for (const RegionExec& forbid : {audio, device, hostaudio}) // FileIO + NetworkIO are illegal in every audio/device tag
    {
        CHECK_FALSE(effect_legal_in_region(EffectFamily::FileIO, forbid));
        CHECK_FALSE(effect_legal_in_region(EffectFamily::NetworkIO, forbid));
        CHECK(effect_legal_in_region(EffectFamily::MemoryWrite, forbid)); // a compute write is fine in an audio region
    }
    for (const RegionExec& ok : {frame, untagged}) // ...and legal in an unconstrained region
    {
        CHECK(effect_legal_in_region(EffectFamily::FileIO, ok));
        CHECK(effect_legal_in_region(EffectFamily::NetworkIO, ok));
        CHECK(effect_legal_in_region(EffectFamily::MemoryWrite, ok));
    }
}

TEST_CASE("ceir domain: the region_exec tag is module CONTENT; the verifier re-fires after text + binary", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const IoOps                  io(ctx);
    Module* const                m   = ctx.create_module();
    Block* const                 top = ctx.create_block(0U);
    m->body()->append(top);
    Block*                       inner = nullptr;
    (void)tagged_region(ctx, io, top, {EvalDomain::HostAudioTime, RealtimeClass::AudioRealTime}, inner);
    inner->append(ctx.create_operation(io.read, {}, 0U)); // a FileIO op inside the audio region — the walk must catch it

    REQUIRE(ctx.find_domain_violation(*m).effect == EffectFamily::FileIO); // caught before any round-trip
    const crd::containers::String         t1   = print(ctx, *m, &root);
    const crd::containers::Array<crd::u8> blob = serialize(ctx, *m, &root);

    // TEXT round-trip: reparse and RE-FIRE the verifier — the tag AND the io.read's registration must reconnect (the op's
    // OpId is the content-hash of "io.read", so `unknown_kind == false` proves the reparsed op rebound to io2's FileIO).
    Context           ctx2(&root);
    const IoOps       io2(ctx2);
    const ParseResult pr = parse(ctx2, t1);
    REQUIRE(pr.ok);
    const DomainViolation v2 = ctx2.find_domain_violation(*pr.module);
    REQUIRE(v2.op != nullptr);
    CHECK(v2.effect == EffectFamily::FileIO);
    CHECK_FALSE(v2.unknown_kind); // registered, not treated as an unknown opaque op

    // BINARY round-trip: same end-to-end re-fire
    Context           ctx3(&root);
    const IoOps       io3(ctx3);
    const ParseResult dr = deserialize(ctx3, ConstSpan<crd::u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    const DomainViolation v3 = ctx3.find_domain_violation(*dr.module);
    REQUIRE(v3.op != nullptr);
    CHECK(v3.effect == EffectFamily::FileIO);
    CHECK_FALSE(v3.unknown_kind);
}

TEST_CASE("ceir domain: find_domain_violation flags a forbidden effect in an audio region, spares a legal one", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    (void)arith::register_arith_ops(ctx);
    const IoOps                  io(ctx);

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Block*        inner = nullptr;
    Operation* const owner = tagged_region(ctx, io, top, {EvalDomain::Unspecified, RealtimeClass::AudioRealTime}, inner);
    Operation* const rd    = ctx.create_operation(io.read, {}, 0U); // FileIO inside the audio region
    inner->append(rd);

    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op == rd);
    CHECK(v.region_owner == owner);         // points at the op that SET the constraint
    CHECK(v.effect == EffectFamily::FileIO);
    CHECK_FALSE(v.unknown_kind);

    // a registered EFFECT-FREE op (arith.addi) in the same audio region is legal — empty≠unknown, the legal half.
    inner->append(ctx.create_operation(ctx.intern_op("arith", "addi"), {}, 1U, ctx.type_i32()));
    CHECK(ctx.find_domain_violation(*m).op == rd); // still only the io.read, not the addi
}

TEST_CASE("ceir domain: an UNREGISTERED op in an audio region is flagged (maximally effectful)", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const IoOps                  io(ctx);

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Block*        inner = nullptr;
    Operation* const owner = tagged_region(ctx, io, top, {EvalDomain::Unspecified, RealtimeClass::AudioRealTime}, inner);
    Operation* const opaque = ctx.create_operation(ctx.intern_op("plugin", "mystery"), {}, 0U); // unknown kind
    inner->append(opaque);

    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op == opaque);
    CHECK(v.region_owner == owner);
    CHECK(v.unknown_kind); // an unregistered op is maximally effectful ⇒ a potential FileIO in an audio region

    // the SAME unknown op in a non-audio region is fine (FileIO is legal there, so its unknown effects are too).
    Module* const m2   = ctx.create_module();
    Block* const  top2 = ctx.create_block(0U);
    m2->body()->append(top2);
    Block*        inner2 = nullptr;
    (void)tagged_region(ctx, io, top2, {EvalDomain::HostFrameTime, RealtimeClass::FrameCritical}, inner2);
    inner2->append(ctx.create_operation(ctx.intern_op("plugin", "mystery"), {}, 0U));
    CHECK(ctx.find_domain_violation(*m2).op == nullptr);
}

TEST_CASE("ceir domain: the tag is innermost-wins; a nested override can make a forbidden effect legal", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const IoOps                  io(ctx);

    // OUTER audio region ⊃ INNER HostFrameTime region ⊃ io.read : the inner tag REPLACES the outer for its subtree.
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Block*        outer_body = nullptr;
    (void)tagged_region(ctx, io, top, {EvalDomain::Unspecified, RealtimeClass::AudioRealTime}, outer_body);
    Block*        inner_body = nullptr;
    (void)tagged_region(ctx, io, outer_body, {EvalDomain::HostFrameTime, RealtimeClass::FrameCritical}, inner_body);
    inner_body->append(ctx.create_operation(io.read, {}, 0U)); // FileIO — legal under the innermost HostFrameTime tag

    CHECK(ctx.find_domain_violation(*m).op == nullptr); // innermost-wins: the override relaxes the constraint

    // but a FileIO directly in the OUTER audio region (not the inner override) is still caught.
    Operation* const rd = ctx.create_operation(io.read, {}, 0U);
    outer_body->append(rd);
    CHECK(ctx.find_domain_violation(*m).op == rd);
}

TEST_CASE("ceir domain: an untagged intermediate inherits the outer tag AND owner; a present-empty tag unconstrains", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const IoOps                  io(ctx);

    // OUTER audio owner ⊃ an UNTAGGED io.hold ⊃ io.read : the untagged intermediate inherits both the tag and its owner.
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Block*           outer_body  = nullptr;
    Operation* const outer_owner = tagged_region(ctx, io, top, {EvalDomain::Unspecified, RealtimeClass::AudioRealTime}, outer_body);
    Operation* const mid         = ctx.create_operation(io.hold, {}, 0U, {}, 1U); // NO set_region_exec — untagged
    outer_body->append(mid);
    Block* const mid_body = ctx.create_block(0U);
    mid->region(0)->append(mid_body);
    Operation* const rd = ctx.create_operation(io.read, {}, 0U);
    mid_body->append(rd);

    const DomainViolation v = ctx.find_domain_violation(*m);
    REQUIRE(v.op == rd);
    CHECK(v.region_owner == outer_owner); // ⛔ NOT `mid` — the violation names the op that SET the constraint, not the container

    // ⛔ present-empty ≠ absent: tagging `mid` with an all-Unspecified RegionExec is a PRESENT unconstrain OVERRIDE for its
    // subtree (innermost-wins), so the same io.read becomes legal — distinct from the untagged inherit above.
    ctx.set_region_exec(mid, RegionExec{});
    CHECK(ctx.find_domain_violation(*m).op == nullptr);
}

TEST_CASE("ceir domain: a corrupt region_exec attr is a violation pointing at the owner", "[ceir][domain]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context                      ctx(&root);
    const IoOps                  io(ctx);

    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const owner = ctx.create_operation(io.hold, {}, 0U, {}, 1U);
    top->append(owner);
    owner->region(0)->append(ctx.create_block(0U));
    ctx.set_attr(owner, "region_exec", ctx.attr_int(static_cast<crd::i64>(0xBULL))); // domain nibble 11 > max 10 = corrupt

    const DomainViolation v = ctx.find_domain_violation(*m);
    CHECK(v.op == owner);
    CHECK(v.region_owner == owner);
}
