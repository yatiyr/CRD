// CEIR-7a - the CEIR asset COOK bridge: cook a verified crd::ceir::Module into a self-describing CRDR blob (CookedHeader
// + 'CEIR' program chunk + 'CDEP' dependency chunk), then round-trip it back. The HEADLINE is CROSS-CONTEXT PURITY: cook
// in Context A, read into a FRESH Context B, recompute BOTH hashes on B's module, and require they equal the header
// fields (the declared-header-words-validated scar + the 3a dirty-Context precedent). Plus: the sec 107 property end to
// end (an impl-only edit keeps the interface hash, changes the content hash), dependency survival, the STRICT
// unregistered-op rejection (EMPTY != UNKNOWN), a verify-reject, and the read error paths. ASCII test names.

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/test_ops.hpp>
#include <crd/ceir/print.hpp> // print (the source-text cook path)
#include <crd/ceir/program_asset.hpp>
#include <crd/resources/crdr.hpp> // the test FORGES containers (missing-CDEP / wrong-type)
#include <crd/resources/resource_id.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::ceir;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir::cook; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::StringView;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;

namespace
{
struct Reg
{
    OpId cst, muli, dummy;
    explicit Reg(Context& c)
        : cst(c.intern_op("arith", "const")), muli(c.intern_op("arith", "muli")), dummy(c.intern_op("test", "dummy"))
    {
        (void)arith::register_arith_ops(c);
        (void)core::register_core_ops(c);
        (void)test::register_test_ops(c);
        (void)func::register_dialect(c);
    }
};
Block* module_block(Context& c, Module& m)
{
    Block* b = m.body()->first_block();
    if (b == nullptr)
    {
        b = c.create_block(0U);
        m.body()->append(b);
    }
    return b;
}
Operation* konst(Context& c, const Reg& r, Block* b, i64 v)
{
    Operation* const op = c.create_operation(r.cst, {}, 1U, c.type_i32());
    c.set_attr(op, "value", c.attr_int(v));
    b->append(op);
    return op;
}
// @f(x: i32) -> i32 { return x*x }
Operation* mkfunc_sq(Context& c, const Reg& r, Module& m, StringView name)
{
    Operation* const f = func::create_func(c, m, name, Visibility::Public, 1U, c.type_i32());
    module_block(c, m)->append(f);
    Block* const b = func::func_body_block(f);
    Value*       ops[2] = {b->arg(0U), b->arg(0U)};
    Operation* const mul = c.create_operation(r.muli, ConstSpan<Value*>(ops, 2U), 1U, c.type_i32());
    b->append(mul);
    Value* rv[1] = {mul->result(0U)};
    b->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));
    return f;
}
// @f() -> i32 { return K }  (K distinguishes an implementation-only edit).
Module* build_const_fn(Context& c, const Reg& r, i64 k)
{
    Module* const m = c.create_module();
    (void)module_block(c, *m);
    Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
    module_block(c, *m)->append(f);
    Block* const b = func::func_body_block(f);
    Value* rv[1] = {konst(c, r, b, k)->result(0U)};
    b->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));
    return m;
}
// CEIR-13c: a single-entry mock kernel resolver (name -> interface hash). fn-ptr + user, the KernelResolveFn contract.
struct KTable
{
    StringView name;
    u64        hash;
};
bool resolve_one(StringView n, void* user, u64& out)
{
    const KTable* const t = static_cast<const KTable*>(user);
    if (n == t->name)
    {
        out = t->hash;
        return true;
    }
    return false;
}
struct CReg
{
    OpId cst, decl, disp;
    explicit CReg(Context& c)
        : cst(c.intern_op("arith", "const")), decl(c.intern_op("resource", "declare")),
          disp(c.intern_op("compute", "dispatch"))
    {
        (void)arith::register_arith_ops(c);
        (void)func::register_dialect(c);
        (void)resource::register_resource_ops(c);
        (void)compute::register_compute_ops(c);
    }
};
// append %buf=declare; %g=1; compute.dispatch(%g,%g,%g, %buf){kernel=@kernel_name, access="r"[, kernel_interface=pin]}.
Operation* add_dispatch(Context& c, const CReg& r, Block* bm, const char* kernel_name, i64 pin, bool pinned)
{
    Operation* const buf = c.create_operation(r.decl, {}, 1U, c.type_buffer(BufferMode::Plain, c.type_f32()));
    bm->append(buf);
    Operation* const g = c.create_operation(r.cst, {}, 1U, c.type_index());
    c.set_attr(g, "value", c.attr_int(1));
    bm->append(g);
    Value*           dops[4] = {g->result(0U), g->result(0U), g->result(0U), buf->result(0U)};
    Operation* const d       = c.create_operation(r.disp, ConstSpan<Value*>(dops, 4U), 0U);
    c.set_attr(d, "kernel", c.attr_symbol(StringView(kernel_name)));
    c.set_attr(d, "access", c.attr_string("r"));
    if (pinned) { c.set_attr(d, "kernel_interface", c.attr_int(pin)); }
    bm->append(d);
    return d;
}
Block* main_block(Context& c, Module& m)
{
    Operation* const f = func::create_func(c, m, "main", Visibility::Public, 0U);
    module_block(c, m)->append(f);
    return func::func_body_block(f);
}
Module* build_dispatch(Context& c, const CReg& r, const char* kernel_name, i64 pin, bool pinned)
{
    Module* const m  = c.create_module();
    Block* const  bm = main_block(c, *m);
    (void)add_dispatch(c, r, bm, kernel_name, pin, pinned);
    bm->append(func::create_return(c, {}));
    return m;
}
} // namespace

TEST_CASE("ceir 13c: the cook resolves ckir_refs and enforces the interface-hash contract", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    // a stand-in KERNEL program cooked first; its §107 interface hash is the "actual" the resolver returns.
    u64 kernel_hash = 0;
    {
        Context    kc(&root);
        const Reg  kr(kc);
        Module*    km = kc.create_module();
        (void)mkfunc_sq(kc, kr, *km, StringView("kmain"));
        const CookResult kres = cook_program(kc, *km, 0x9001U, &root, &root);
        REQUIRE(kres.ok());
        kernel_hash = kres.interface_hash;
    }
    KTable      table{StringView("mykernel"), kernel_hash}; // non-const so &table -> void* needs no const_cast
    void* const user = &table;

    // (1) the pin MATCHES the resolved kernel -> cook OK.
    {
        Context      c(&root);
        const CReg   r(c);
        Module* const    m   = build_dispatch(c, r, "mykernel", static_cast<i64>(kernel_hash), true);
        const CookResult res = cook_program(c, *m, 1U, &root, &root, &resolve_one, user);
        CHECK(res.ok());
    }
    // (2) the pin MISMATCHES -> KernelInterfaceMismatch, op = the dispatch.
    {
        Context          c(&root);
        const CReg       r(c);
        Module* const    m   = build_dispatch(c, r, "mykernel", static_cast<i64>(kernel_hash ^ 1U), true);
        const CookResult res = cook_program(c, *m, 1U, &root, &root, &resolve_one, user);
        CHECK(res.error == CookError::KernelInterfaceMismatch);
        REQUIRE(res.op != nullptr);
        CHECK(c.op_name(res.op->kind()) == StringView("compute.dispatch")); // ⭐ points at the DISPATCH
    }
    // (3) an unknown @kernel name -> KernelUnresolved (existence is ALWAYS checked, even unpinned).
    {
        Context          c(&root);
        const CReg       r(c);
        Module* const    m   = build_dispatch(c, r, "ghost", 0, false);
        const CookResult res = cook_program(c, *m, 1U, &root, &root, &resolve_one, user);
        CHECK(res.error == CookError::KernelUnresolved);
    }
    // (4) UNPINNED but resolvable -> OK (no hash to check, but the name must resolve).
    {
        Context          c(&root);
        const CReg       r(c);
        Module* const    m   = build_dispatch(c, r, "mykernel", 0, false);
        const CookResult res = cook_program(c, *m, 1U, &root, &root, &resolve_one, user);
        CHECK(res.ok());
    }
    // (5) NO resolver -> cook OK (deferred) + the CDEP chunk PERSISTS the refs; read_program returns them (schema v5). Two
    // dispatches (one PINNED, one UNPINNED) exercise the pinned=0 wire branch + the count>1 CDEP path + the sort.
    {
        Context       c(&root);
        const CReg    r(c);
        Module* const m  = c.create_module();
        Block* const  bm = main_block(c, *m);
        (void)add_dispatch(c, r, bm, "ghost", static_cast<i64>(kernel_hash), true); // pinned (unresolvable, but deferred)
        (void)add_dispatch(c, r, bm, "zebra", 0, false);                            // UNPINNED
        bm->append(func::create_return(c, {}));
        const CookResult res = cook_program(c, *m, 1U, &root, &root); // no resolver -> resolution deferred
        REQUIRE(res.ok());
        Context          c2(&root);
        const CReg       r2(c2);
        (void)r2;
        const ReadResult rr = read_program(c2, ConstSpan<u8>(res.blob.data(), res.blob.size()), &root);
        REQUIRE(rr.ok());
        REQUIRE(rr.deps.ckir_refs.size() == 2U); // ⭐ both refs survived the CDEP round-trip, sorted by name
        CHECK(rr.deps.ckir_refs[0].name == StringView("ghost"));
        CHECK(rr.deps.ckir_refs[0].pinned);
        CHECK(rr.deps.ckir_refs[0].interface_hash == kernel_hash);
        CHECK(rr.deps.ckir_refs[1].name == StringView("zebra"));
        CHECK_FALSE(rr.deps.ckir_refs[1].pinned); // ⭐ the unpinned (pinned=0) wire branch
    }
}

TEST_CASE("ceir cook 7a: a module cooks and round-trips CROSS-CONTEXT with matching hashes", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    // COOK in Context A.
    Context   a(&root);
    const Reg ra(a);
    Module*   ma = a.create_module();
    (void)module_block(a, *ma);
    (void)mkfunc_sq(a, ra, *ma, "square");
    const CookResult cooked = cook_program(a, *ma, /*asset_id*/ 0x1234U, &root, &root);
    REQUIRE(cooked.ok());
    REQUIRE(cooked.blob.size() > 0U);

    // READ into a FRESH Context B (independent intern history).
    Context   b(&root);
    const Reg rb(b); // the caller re-registers the dialects before recomputing hashes
    const ReadResult read = read_program(b, ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), &root);
    REQUIRE(read.ok());
    REQUIRE(read.module != nullptr);
    // the header hashes survive the blob verbatim...
    CHECK(read.content_hash == cooked.content_hash);
    CHECK(read.interface_hash == cooked.interface_hash);
    // ...AND recomputing them on B's deserialized module reproduces them (cross-Context PURITY -- no intern-state leak).
    CHECK(stable_hash(b, *read.module, &root) == cooked.content_hash);
    CHECK(interface_hash(b, *read.module, &root) == cooked.interface_hash);
}

TEST_CASE("ceir cook 7a: an implementation-only edit keeps the interface hash but changes the content hash", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    const CookResult c5 = cook_program(a, *build_const_fn(a, ra, 5), 1U, &root, &root);

    Context   b(&root);
    const Reg rb(b);
    const CookResult c6 = cook_program(b, *build_const_fn(b, rb, 6), 1U, &root, &root); // body constant 6, not 5

    REQUIRE(c5.ok());
    REQUIRE(c6.ok());
    CHECK(c5.interface_hash == c6.interface_hash); // sec 107: callers are not invalidated by an impl edit
    CHECK(c5.content_hash != c6.content_hash);     // ...but the cook-cache key differs (a recook)
}

TEST_CASE("ceir cook 7a: the dependency record survives the round-trip", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    Module*   m = a.create_module();
    (void)module_block(a, *m);
    // @main() { call ext(); test.dummy; return }  (ext is unresolved -> an import dependency; dummy is an intrinsic)
    Operation* const fm = func::create_func(a, *m, "main", Visibility::Public, 0U, a.type_i32());
    module_block(a, *m)->append(fm);
    Block* const mb = func::func_body_block(fm);
    mb->append(func::create_call(a, "ext", {}, 0U, {}));
    mb->append(a.create_operation(ra.dummy, {}, 0U, {}, 0U));
    mb->append(func::create_return(a, {}));

    const CookResult cooked = cook_program(a, *m, 7U, &root, &root);
    REQUIRE(cooked.ok());
    Context   b(&root);
    const Reg rb(b);
    const ReadResult read = read_program(b, ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), &root);
    REQUIRE(read.ok());
    REQUIRE(read.deps.called_funcs.size() == 1U);
    CHECK(read.deps.called_funcs[0] == StringView("ext"));
    REQUIRE(read.deps.intrinsics.size() == 1U);
    CHECK(read.deps.intrinsics[0] == StringView("test.dummy"));
    REQUIRE(read.deps.providers.size() == 1U);
    CHECK(read.deps.providers[0] == StringView("host"));
}

TEST_CASE("ceir cook 7a: cooking REJECTS an op of an unregistered dialect (EMPTY != UNKNOWN)", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    Block* const top = module_block(c, *m);
    top->append(c.create_operation(c.intern_op("ghost", "op"), {}, 0U, {}, 0U)); // never registered
    const CookResult cooked = cook_program(c, *m, 1U, &root, &root);
    CHECK_FALSE(cooked.ok());
    CHECK(cooked.error == CookError::UnregisteredOp);
    CHECK(cooked.op != nullptr);
}

TEST_CASE("ceir cook 7a: cooking REJECTS a structurally-invalid module (source must be VERIFIED)", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    (void)module_block(c, *m);
    Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
    module_block(c, *m)->append(f);
    Block* const b = func::func_body_block(f);
    // %late defined AFTER its use -> a def-before-use structural defect (not a state feedback).
    Operation* const late = c.create_operation(r.cst, {}, 1U, c.type_i32());
    c.set_attr(late, "value", c.attr_int(3));
    Value*           bad_ops[2] = {late->result(0U), late->result(0U)};
    Operation* const bad        = c.create_operation(r.muli, ConstSpan<Value*>(bad_ops, 2U), 1U, c.type_i32());
    b->append(bad);  // the USE comes first...
    b->append(late); // ...the DEF second -> UseBeforeDef
    Value* rv[1] = {bad->result(0U)};
    b->append(func::create_return(c, ConstSpan<Value*>(rv, 1U)));

    const CookResult cooked = cook_program(c, *m, 1U, &root, &root);
    CHECK_FALSE(cooked.ok());
    CHECK(cooked.error == CookError::StructureError);
    CHECK(cooked.op != nullptr);
}

TEST_CASE("ceir cook 7a: reading garbage bytes reports BadContainer, not a crash", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context c(&root);
    const u8 junk[8] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    const ReadResult read = read_program(c, ConstSpan<u8>(junk, 8U), &root);
    CHECK_FALSE(read.ok());
    CHECK(read.error == ReadError::BadContainer);
}

TEST_CASE("ceir cook 7a: cooking from source TEXT matches cooking the builder module (no privileged path)", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    Module*   m = a.create_module();
    (void)module_block(a, *m);
    (void)mkfunc_sq(a, ra, *m, "square");
    const crd::containers::String text = print(a, *m, &root); // the source form (no SourceLocs set -> text/binary agree)

    const CookResult from_module = cook_program(a, *m, 9U, &root, &root);
    const CookResult from_text   = cook_program_text(a, StringView(text.data(), text.size()), 9U, &root, &root);
    REQUIRE(from_module.ok());
    REQUIRE(from_text.ok());
    CHECK(from_text.content_hash == from_module.content_hash);
    CHECK(from_text.interface_hash == from_module.interface_hash);
}

TEST_CASE("ceir cook 7a: cooking REJECTS a None-declared function that recurses (declared-words-validated)", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   c(&root);
    const Reg r(c);
    Module*   m = c.create_module();
    (void)module_block(c, *m);
    Operation* const f = func::create_func(c, *m, "f", Visibility::Public, 0U, c.type_i32());
    module_block(c, *m)->append(f);
    func::set_recursion_policy(c, f, func::RecursionPolicy::None); // DECLARES non-recursive...
    Block* const b = func::func_body_block(f);
    b->append(func::create_call(c, "f", {}, 0U, {})); // ...but calls itself -> DeclaredNoneRecurses
    b->append(func::create_return(c, {}));

    const CookResult cooked = cook_program(c, *m, 1U, &root, &root);
    CHECK_FALSE(cooked.ok());
    CHECK(cooked.error == CookError::RecursionViolation);
    CHECK(cooked.op != nullptr);
}

TEST_CASE("ceir cook 7a: a blob whose dependency chunk is MISSING reports BadDeps (never silent no-deps)", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context   a(&root);
    const Reg ra(a);
    Module*   m = a.create_module();
    (void)module_block(a, *m);
    (void)mkfunc_sq(a, ra, *m, "square");
    const CookResult cooked = cook_program(a, *m, 5U, &root, &root);
    REQUIRE(cooked.ok());

    // RE-PACKAGE the cooked container WITHOUT the 'CDEP' chunk (reusing the valid 'META' + 'CEIR' payloads).
    crd::resources::CrdrFile file(&root);
    REQUIRE(crd::resources::crdr_read(ConstSpan<u8>(cooked.blob.data(), cooked.blob.size()), file, &root)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* const meta = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_META);
    const crd::resources::CrdrChunk* const prog = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_CEIR);
    REQUIRE(meta != nullptr);
    REQUIRE(prog != nullptr);
    crd::resources::CrdrWriter w(&root, crd::resources::ResourceId{0U, 5U}, crd::resources::kFourCC_CEIR);
    w.add_chunk(crd::resources::kFourCC_META, meta->payload);
    w.add_chunk(crd::resources::kFourCC_CEIR, prog->payload); // NO 'CDEP'
    const crd::containers::Array<u8> forged = w.finish();

    Context          b(&root);
    const Reg        rb(b);
    const ReadResult read = read_program(b, ConstSpan<u8>(forged.data(), forged.size()), &root);
    CHECK_FALSE(read.ok());
    CHECK(read.error == ReadError::BadDeps);
}

TEST_CASE("ceir cook 7a: reading a container of the WRONG type reports WrongType", "[ceir][cook]")
{
    crd::memory::GrowableTlsfAllocator root;
    Context  c(&root);
    const u8 dummy[4] = {1U, 2U, 3U, 4U};
    crd::resources::CrdrWriter w(&root, crd::resources::ResourceId{0U, 1U}, crd::resources::kFourCC_PACK); // NOT a CEIR program
    w.add_chunk(crd::resources::kFourCC_BLOB, ConstSpan<u8>(dummy, 4U));
    const crd::containers::Array<u8> blob = w.finish();

    const ReadResult read = read_program(c, ConstSpan<u8>(blob.data(), blob.size()), &root);
    CHECK_FALSE(read.ok());
    CHECK(read.error == ReadError::WrongType);
}
