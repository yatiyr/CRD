// CEIR-8f (ADR-0116, U-§57) — the capability contract. Programs REQUEST capabilities; hosts GRANT them. The matrix: an
// op's declared required set round-trips (op_capabilities); the module-wide program set is the sorted-unique union with
// an UNREGISTERED op contributing external.process (EMPTY!=UNKNOWN); capabilities_satisfied is the host-grant check;
// the required set JOINS the interface hash (different caps -> different hash; a REORDER is invariant; adding a cap
// changes it) while stable_hash (content) is UNAFFECTED; the FNV id round-trips to its name. Host-only. ASCII names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>        // serialize / deserialize / stable_hash
#include <crd/ceir/func.hpp>
#include <crd/ceir/program_asset.hpp> // interface_hash

#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::memcmp

using namespace crd::ceir; // NOLINT(google-build-using-namespace)
namespace fn = crd::ceir::func;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::u8;
using crd::usize;
using ByteArray = Array<u8>;

namespace
{
[[nodiscard]] ConstSpan<u8> span(const ByteArray& b) noexcept { return ConstSpan<u8>(b.data(), b.size()); }
[[nodiscard]] bool blob_eq(const ByteArray& a, const ByteArray& b) noexcept
{
    return a.size() == b.size() && (a.size() == 0U || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
// A module whose body holds the given ops in order (0-result bare ops).
Module* mod_with_ops(Context& ctx, ConstSpan<OpId> kinds)
{
    Module* const m   = ctx.create_module();
    Block* const  top = ctx.create_block(0U);
    m->body()->append(top);
    for (usize i = 0; i < kinds.size(); ++i) { top->append(ctx.create_operation(kinds[i], {}, 0U)); }
    return m;
}
[[nodiscard]] bool set_has(const Array<CapabilityId>& s, CapabilityId c) noexcept
{
    for (usize i = 0; i < s.size(); ++i)
    {
        if (s[i] == c) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("ceir 8f: an op's required capabilities round-trip; an UNREGISTERED op contributes external.process", "[ceir][capability]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d      = ctx.register_dialect("test");
    const StringView             gpu[1] = {StringView{"gpu.compute"}};
    const OpId                   k      = d->register_op("gpu_op", {.capabilities = ConstSpan<StringView>(gpu, 1U)});

    const ConstSpan<CapabilityId> caps = ctx.op_capabilities(k);
    REQUIRE(caps.size() == 1U);
    CHECK(caps[0] == ctx.intern_capability("gpu.compute")); // the FNV id matches the interned name
    CHECK(ctx.capability_name(caps[0]) == StringView{"gpu.compute"});
    CHECK(ctx.op_capabilities(ctx.intern_op("plugin", "widget")).size() == 0U); // unregistered: empty HERE...

    // ...but the PROGRAM set adds external.process for the unregistered op (EMPTY!=UNKNOWN), sorted-unique.
    const OpId    kinds[2] = {k, ctx.intern_op("plugin", "widget")};
    Module* const m        = mod_with_ops(ctx, ConstSpan<OpId>(kinds, 2U));
    Array<CapabilityId> prog(&root);
    ctx.program_capabilities(*m, prog);
    CHECK(prog.size() == 2U);
    CHECK(set_has(prog, ctx.intern_capability("gpu.compute")));
    CHECK(set_has(prog, ctx.intern_capability("external.process")));
}

TEST_CASE("ceir 8f: the program capability set is the module-wide sorted-UNIQUE union", "[ceir][capability]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d     = ctx.register_dialect("test");
    const StringView             a[1]  = {StringView{"scene.read"}};
    const StringView             ab[2] = {StringView{"scene.read"}, StringView{"gpu.compute"}};
    const OpId                   ka    = d->register_op("a", {.capabilities = ConstSpan<StringView>(a, 1U)});
    const OpId                   kab   = d->register_op("ab", {.capabilities = ConstSpan<StringView>(ab, 2U)});

    // two ops both requiring scene.read + one also gpu.compute -> the union DEDUPES scene.read.
    const OpId    kinds[2] = {ka, kab};
    Array<CapabilityId> prog(&root);
    ctx.program_capabilities(*mod_with_ops(ctx, ConstSpan<OpId>(kinds, 2U)), prog);
    CHECK(prog.size() == 2U); // {scene.read, gpu.compute} — scene.read appears once
    // sorted: strictly increasing ids
    for (usize i = 1; i < prog.size(); ++i) { CHECK(prog[i - 1U].value < prog[i].value); }
}

TEST_CASE("ceir 8f: capabilities_satisfied is the host-grant check (required subset of granted)", "[ceir][capability]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    const CapabilityId req[2]   = {ctx.intern_capability("scene.read"), ctx.intern_capability("gpu.compute")};
    const CapabilityId grant_a[2] = {ctx.intern_capability("scene.read"), ctx.intern_capability("gpu.compute")};
    const CapabilityId grant_b[1] = {ctx.intern_capability("scene.read")}; // missing gpu.compute
    CHECK(Context::capabilities_satisfied(ConstSpan<CapabilityId>(req, 2U), ConstSpan<CapabilityId>(grant_a, 2U)));
    CHECK_FALSE(Context::capabilities_satisfied(ConstSpan<CapabilityId>(req, 2U), ConstSpan<CapabilityId>(grant_b, 1U)));
    CHECK(Context::capabilities_satisfied(ConstSpan<CapabilityId>(req, 0U), ConstSpan<CapabilityId>(grant_b, 1U))); // none required
}

TEST_CASE("ceir 8f: the required capability set joins the interface hash (reorder-invariant, membership-sensitive)", "[ceir][capability]")
{
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d    = ctx.register_dialect("test");
    const StringView             a[1] = {StringView{"scene.read"}};
    const StringView             b[1] = {StringView{"gpu.compute"}};
    const OpId                   ka   = d->register_op("a", {.capabilities = ConstSpan<StringView>(a, 1U)});
    const OpId                   kb   = d->register_op("b", {.capabilities = ConstSpan<StringView>(b, 1U)});

    const OpId    ab[2] = {ka, kb};
    const OpId    ba[2] = {kb, ka};
    const OpId    aonly[1] = {ka};
    Module* const m_ab = mod_with_ops(ctx, ConstSpan<OpId>(ab, 2U));
    Module* const m_ba = mod_with_ops(ctx, ConstSpan<OpId>(ba, 2U));
    Module* const m_a  = mod_with_ops(ctx, ConstSpan<OpId>(aonly, 1U));

    CHECK(interface_hash(ctx, *m_ab, &root) == interface_hash(ctx, *m_ba, &root)); // REORDER invariant (sorted-unique)
    CHECK(interface_hash(ctx, *m_ab, &root) != interface_hash(ctx, *m_a, &root));  // dropping a cap CHANGES the hash
    // ...but the CONTENT hash is unaffected by the presence of the cap (caps aren't serialized in the module blob).
    // (m_ab and m_a differ in op count too, so instead compare a cap-only difference below.)
}

TEST_CASE("ceir 8f: an UNREGISTERED op's external.process capability reaches the interface hash (EMPTY!=UNKNOWN)", "[ceir][capability]")
{
    // ⛔ the security-relevant leg: an unknown op-kind must not read as requiring-nothing. Two modules with a single op
    // of the SAME arity — one a registered effect-free/cap-free op, one an UNREGISTERED op — have DIFFERENT interface
    // hashes, because the unregistered op folds external.process into the program capability set.
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d       = ctx.register_dialect("test");
    const OpId                   known   = d->register_op("known", {}); // registered, requires no capabilities
    const OpId                   unknown = ctx.intern_op("plugin", "widget"); // unregistered
    const OpId                   a1[1]   = {known};
    const OpId                   a2[1]   = {unknown};
    CHECK(interface_hash(ctx, *mod_with_ops(ctx, ConstSpan<OpId>(a1, 1U)), &root) !=
          interface_hash(ctx, *mod_with_ops(ctx, ConstSpan<OpId>(a2, 1U)), &root));
}

TEST_CASE("ceir 8f: a capability required by an op INSIDE a func body reaches the program set", "[ceir][capability]")
{
    // ⛔ pins the module-wide-walk-covers-func-bodies claim (funcs ARE ops in the body; the recursion visits their
    // regions) — the whole "no transitivity machinery" simplification rests on it.
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d      = ctx.register_dialect("test");
    const StringView             gpu[1] = {StringView{"gpu.compute"}};
    const OpId                   k      = d->register_op("gpu_op", {.capabilities = ConstSpan<StringView>(gpu, 1U)});

    Module* const    m   = ctx.create_module();
    Block* const     top = ctx.create_block(0U);
    m->body()->append(top);
    Operation* const f = fn::create_func(ctx, *m, "f", Visibility::Public, 0U);
    top->append(f);
    fn::func_body_block(f)->append(ctx.create_operation(k, {}, 0U)); // the cap-op is INSIDE @f's body region

    Array<CapabilityId> prog(&root);
    ctx.program_capabilities(*m, prog);
    CHECK(set_has(prog, ctx.intern_capability("gpu.compute"))); // reached from inside the nested func body
}

TEST_CASE("ceir 8f: capabilities are registration metadata - a cap-bearing module round-trips byte-exact + same content hash", "[ceir][capability]")
{
    // ⛔ pins "capabilities are NOT in the module blob": a cap-bearing module serialize->deserialize->serialize is
    // BYTE-EXACT and its stable_hash is unchanged (caps live in the interface hash + registration, never the content).
    crd::memory::MallocAllocator root;
    Context                      ctx(&root);
    Dialect* const               d      = ctx.register_dialect("test");
    const StringView             cap[1] = {StringView{"file.write"}};
    const OpId                   k      = d->register_op("w", {.capabilities = ConstSpan<StringView>(cap, 1U)});
    const OpId                   a[1]   = {k};
    Module* const                m      = mod_with_ops(ctx, ConstSpan<OpId>(a, 1U));

    const ByteArray blob = serialize(ctx, *m, &root);
    Context         ctx2(&root);
    Dialect* const  d2 = ctx2.register_dialect("test");
    (void)d2->register_op("w", {.capabilities = ConstSpan<StringView>(cap, 1U)}); // re-register the kind (caps re-derive)
    const ParseResult pr = deserialize(ctx2, span(blob));
    REQUIRE(pr.ok);
    CHECK(blob_eq(blob, serialize(ctx2, *pr.module, &root)));                       // caps not in the blob -> byte-exact
    CHECK(stable_hash(ctx, *m, &root) == stable_hash(ctx2, *pr.module, &root));     // content hash unchanged
}
