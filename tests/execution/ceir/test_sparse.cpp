// CEIR-23d (§53) — the ceir.sparse dialect: sparse.spmv (CSR sparse-matrix × dense-vector, y = A·x). The CSR triple
// (row_ptr, col_idx, values) + the dense x are explicit rank-1 tensor OPERANDS (no new TypeKind). find_sparse_misuse enforces
// the SHAPE-AWARE + ELEMENT-ROLE contract the generated verify_spmv cannot: all five rank-1, the index arrays Int-kinded, the
// value side (values/x/y) Float + EQUAL, and the CSR shape relations (nnz, M+1). Well-formed CSR verifies clean; every contract
// misuse rejects with the exact kind. ⛔ row_ptr monotonicity / col_idx<N / nnz==row_ptr[M] are DATA properties, out of scope.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/sparse.hpp>
#include <crd/ceir/type.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using SK = crd::ceir::sparse::SparseMisuseKind;

namespace
{
struct Kit
{
    OpId decl;
    explicit Kit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)sparse::register_dialect(ctx);
    }
};
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 b)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(b)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
TypeId tt(Context& ctx, TypeId elem, TypeId shape) { return ctx.type_tensor(elem, shape); }

Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* decl(Context& ctx, const Kit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
} // namespace

TEST_CASE("ceir 23d: a well-formed CSR sparse.spmv verifies clean", "[ceir][sparse]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Kit                     k(ctx);
    const TypeId                  f32 = ctx.type_f32();
    const TypeId                  i32 = ctx.type_int(32U, true);
    Module* const                 m   = ctx.create_module();
    Block* const                  b   = mkmain(ctx, *m);

    // M=4 rows, nnz=9, N=6: row_ptr i32[5], col_idx i32[9], values f32[9], x f32[6], y f32[4].
    Value* const     rp = decl(ctx, k, b, tt(ctx, i32, sh1(ctx, 5U)));
    Value* const     ci = decl(ctx, k, b, tt(ctx, i32, sh1(ctx, 9U)));
    Value* const     vl = decl(ctx, k, b, tt(ctx, f32, sh1(ctx, 9U)));
    Value* const     xv = decl(ctx, k, b, tt(ctx, f32, sh1(ctx, 6U)));
    Operation* const op = sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f32, sh1(ctx, 4U)));
    b->append(op);

    CHECK(sparse::find_sparse_misuse(ctx, *m).kind == SK::None);
}

TEST_CASE("ceir 23d: find_sparse_misuse rejects every contract misuse with the exact kind", "[ceir][sparse]")
{
    const auto one = [](auto build) {
        memory::GrowableTlsfAllocator root;
        Context                       ctx(&root);
        const Kit                     k(ctx);
        Module* const                 m = ctx.create_module();
        Block* const                  b = mkmain(ctx, *m);
        build(ctx, k, b);
        return sparse::find_sparse_misuse(ctx, *m).kind;
    };

    // OperandNotTensor: row_ptr is a bare scalar i32, not a Tensor.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, i); // WRONG: a scalar, not a tensor
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 9U)));
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::OperandNotTensor);

    // ResultNotTensor: the result type is a bare scalar f32.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 5U)));
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 9U)));
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, f)); // WRONG: scalar result
    }) == SK::ResultNotTensor);

    // RankInvalid: row_ptr is rank-2.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, i, sh2(ctx, 2U, 3U))); // WRONG: rank-2
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 9U)));
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::RankInvalid);

    // IndexElementNotInt: row_ptr is a FLOAT tensor (not an integer index array).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 5U))); // WRONG: float row_ptr
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 9U)));
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::IndexElementNotInt);

    // ValueElementMismatch: values is an INTEGER tensor (not the float value side).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 5U)));
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U))); // WRONG: int values
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::ValueElementMismatch);

    // NnzMismatch: col_idx length 9 but values length 8.
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 5U)));
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 8U))); // WRONG: 8 != col_idx's 9
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::NnzMismatch);

    // RowPtrLengthMismatch: row_ptr length 4 but y length 4 (should be M+1 == 5).
    CHECK(one([](Context& ctx, const Kit& k, Block* b) {
        const TypeId f  = ctx.type_f32();
        const TypeId i  = ctx.type_int(32U, true);
        Value*       rp = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 4U))); // WRONG: 4 != y.dim0(4) + 1
        Value*       ci = decl(ctx, k, b, tt(ctx, i, sh1(ctx, 9U)));
        Value*       vl = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 9U)));
        Value*       xv = decl(ctx, k, b, tt(ctx, f, sh1(ctx, 6U)));
        b->append(sparse::build_spmv(ctx, rp, ci, vl, xv, tt(ctx, f, sh1(ctx, 4U))));
    }) == SK::RowPtrLengthMismatch);
}
