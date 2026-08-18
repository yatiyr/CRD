// CEIR-22a — the ceir.tensor.fft op (sec-59, the GEMM->FFT->reduction charter's FFT leg): a complex-to-complex FFT along
// `axis`, complex = SPLIT re/im (two real tensors in -> two out, MIRRORING the CKIR c2c split-buffer ABI; NO complex element
// TypeKind). Device-free (crd-ceir). Proves: (1) a well-formed fft verifies clean; (2) each contract is REJECTED with the exact
// TensorMisuseKind — the four operands/results Tensor-kinded, element consistency (split re/im share re_in's element), re_in.shape
// == im_in.shape, a c2c FFT PRESERVES shape (re_out/im_out == re_in.shape), `axis` in [0,rank), `direction` in {forward,inverse};
// (3) an under-arity fft FOLDS to the generated verify_fft (the 21x min-arity guard). ⛔ DECLARE-only: NO kernel_ref (sec-70).

#include <crd/ceir/tensor.hpp>

#include <crd/ceir/binary.hpp> // serialize / deserialize (the binary round-trip — the first multi-result crossing)
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/linalg.hpp> // the round-trip module carries gemm too (Float/Bool attr printing rides along)
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
struct TensorKit
{
    OpId decl;
    explicit TensorKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)tensor::register_dialect(ctx);
    }
};
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
Value* mkval(Context& ctx, const TensorKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
TypeId sh1(Context& ctx, u32 a) { const TypeId d[1] = {ctx.type_dim_static(a)}; return ctx.type_shape(ConstSpan<TypeId>(d, 1U)); }
TypeId sh2(Context& ctx, u32 a, u32 c)
{
    const TypeId d[2] = {ctx.type_dim_static(a), ctx.type_dim_static(c)};
    return ctx.type_shape(ConstSpan<TypeId>(d, 2U));
}
Value* tf(Context& ctx, const TensorKit& k, Block* b, TypeId shape) { return mkval(ctx, k, b, ctx.type_tensor(ctx.type_f32(), shape)); }
} // namespace

TEST_CASE("ceir 22a: a well-formed tensor.fft verifies (c2c, split re/im, direction+axis)", "[ceir][tensor][fft]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const TensorKit               k(ctx);
    Module* const                 m = ctx.create_module();
    Block* const                  b = mkmain(ctx, *m);

    const TypeId s8  = sh1(ctx, 8U);
    const TypeId ef  = ctx.type_f32();
    Value* const re  = tf(ctx, k, b, s8);
    Value* const im  = tf(ctx, k, b, s8);
    // fft(re, im){forward, axis=0} -> (re_out<f32,8>, im_out<f32,8>): a c2c FFT preserves shape + element.
    b->append(tensor::build_fft(ctx, re, im, ctx.attr_string(StringView("forward")), ctx.attr_int(0), ctx.type_tensor(ef, s8)));
    // an inverse fft along axis 1 of a rank-2 signal, too.
    const TypeId s34 = sh2(ctx, 3U, 4U);
    Value* const re2 = tf(ctx, k, b, s34);
    Value* const im2 = tf(ctx, k, b, s34);
    b->append(tensor::build_fft(ctx, re2, im2, ctx.attr_string(StringView("inverse")), ctx.attr_int(1), ctx.type_tensor(ef, s34)));

    CHECK(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    CHECK(ctx.op_info(ctx.intern_op("tensor", "fft")) != nullptr);
}

TEST_CASE("ceir 22a: tensor.fft REJECTS every contract misuse with the exact kind", "[ceir][tensor][fft]")
{
    memory::GrowableTlsfAllocator root;
    using MK = tensor::TensorMisuseKind;
    const auto find = [](Context& c, Module& m) { return tensor::find_tensor_misuse(c, m).kind; };

    // OperandNotTensor: re_in a non-tensor.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* bad=mkval(ctx,k,b,ctx.type_i32()); Value* im=tf(ctx,k,b,sh1(ctx,8U));
      b->append(tensor::build_fft(ctx,bad,im,ctx.attr_string(StringView("forward")),ctx.attr_int(0),ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U))));
      CHECK(find(ctx,*m)==MK::OperandNotTensor); }
    // ResultNotTensor: result_type non-tensor (both re_out/im_out i32).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,8U));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("forward")),ctx.attr_int(0),ctx.type_i32()));
      CHECK(find(ctx,*m)==MK::ResultNotTensor); }
    // TensorElementMismatch: im_in i32-element while re_in f32.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=mkval(ctx,k,b,ctx.type_tensor(ctx.type_i32(),sh1(ctx,8U)));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("forward")),ctx.attr_int(0),ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U))));
      CHECK(find(ctx,*m)==MK::TensorElementMismatch); }
    // FftInputShapeMismatch: re_in <8> but im_in <16>.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,16U));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("forward")),ctx.attr_int(0),ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U))));
      CHECK(find(ctx,*m)==MK::FftInputShapeMismatch); }
    // FftResultShapeMismatch: a c2c FFT preserves shape, but the result is declared <16> (input is <8>).
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,8U));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("forward")),ctx.attr_int(0),ctx.type_tensor(ctx.type_f32(),sh1(ctx,16U))));
      CHECK(find(ctx,*m)==MK::FftResultShapeMismatch); }
    // AxisInvalid: axis 5 on a rank-1 signal.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,8U));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("forward")),ctx.attr_int(5),ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U))));
      CHECK(find(ctx,*m)==MK::AxisInvalid); }
    // FftDirectionInvalid: direction outside {forward,inverse}.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,8U));
      b->append(tensor::build_fft(ctx,re,im,ctx.attr_string(StringView("sideways")),ctx.attr_int(0),ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U))));
      CHECK(find(ctx,*m)==MK::FftDirectionInvalid); }
    // STANDALONE-ROBUST: an under-arity fft (1 result, built via create_operation) FOLDS to the generated verify_fft (the
    // min-arity guard skips it in the walk), NEVER trips an in-walk result(1) assert.
    { Context ctx(&root); const TensorKit k(ctx); Module* m=ctx.create_module(); Block* b=mkmain(ctx,*m);
      Value* re=tf(ctx,k,b,sh1(ctx,8U)); Value* im=tf(ctx,k,b,sh1(ctx,8U)); Value* ops[2]={re,im};
      Operation* bad=ctx.create_operation(ctx.intern_op("tensor","fft"),ConstSpan<Value*>(ops,2U),1U,ctx.type_tensor(ctx.type_f32(),sh1(ctx,8U)));
      ctx.set_attr(bad,StringView("direction"),ctx.attr_string(StringView("forward"))); ctx.set_attr(bad,StringView("axis"),ctx.attr_int(0));
      b->append(bad);
      CHECK(find(ctx,*m)==MK::None); } // folds (no assert); the generated verify_fft owns the 2-result arity
}

TEST_CASE("ceir 22a: fft + gemm round-trip byte-clean through text AND binary (the first MULTI-RESULT serializer crossing)",
          "[ceir][tensor][fft][linalg]")
{
    // ⛔ fft is the FIRST fixed 2-result op in these dialects. This PROVES the printer/parser + binary serializer handle a
    // 2-result op (`%a, %b = tensor.fft(...) : !tensor<...>` — the printer emits the SINGLE create_operation result type,
    // the parser reads N result SSA ids + that one type), which UNBLOCKS the 22c proof asset (a TEXT file loaded via parse).
    // gemm rides along (its Float alpha/beta + Bool trans_a/trans_b attrs cover attr printing). ⛔ a distinct re_out/im_out
    // TYPE is UNCONSTRUCTIBLE here (one printed/interned result type broadcasts to both results — the single-result-type model),
    // so the find_tensor_misuse im_out-specific lines are DEFENSIVE (redundant under this model, live for a future per-result
    // type path / a corrupted blob) — no independent malformed test exists for them by construction.
    memory::GrowableTlsfAllocator root;
    const auto reg = [](Context& c) {
        (void)func::register_dialect(c);
        (void)resource::register_resource_ops(c);
        (void)tensor::register_dialect(c);
        (void)linalg::register_dialect(c);
    };
    Context ctx(&root);
    reg(ctx);
    const OpId     decl = ctx.intern_op("resource", "declare");
    Module* const  m    = ctx.create_module();
    Block*         top  = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    Operation* const f = func::create_func(ctx, *m, "main", Visibility::Public, 0U);
    top->append(f);
    Block* const b  = func::func_body_block(f);
    const TypeId ef = ctx.type_f32();
    const auto   mk = [&](TypeId t) { Operation* const d = ctx.create_operation(decl, {}, 1U, t); b->append(d); return d->result(0U); };

    // fft(re<f32,8>, im<f32,8>){forward, axis=0} -> (re_out, im_out) both <f32,8> — the multi-result op.
    const TypeId s8 = sh1(ctx, 8U);
    Value* const re = mk(ctx.type_tensor(ef, s8));
    Value* const im = mk(ctx.type_tensor(ef, s8));
    b->append(tensor::build_fft(ctx, re, im, ctx.attr_string(StringView("forward")), ctx.attr_int(0), ctx.type_tensor(ef, s8)));
    // gemm(A[4,3], B[3,5], C[4,5]){alpha=1,beta=0,no-trans} -> D[4,5].
    Value* const ga = mk(ctx.type_tensor(ef, sh2(ctx, 4U, 3U)));
    Value* const gb = mk(ctx.type_tensor(ef, sh2(ctx, 3U, 5U)));
    Value* const gc = mk(ctx.type_tensor(ef, sh2(ctx, 4U, 5U)));
    b->append(linalg::build_gemm(ctx, ga, gb, gc, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                 ctx.attr_bool(false), ctx.type_tensor(ef, sh2(ctx, 4U, 5U))));

    REQUIRE(tensor::find_tensor_misuse(ctx, *m).kind == tensor::TensorMisuseKind::None);
    REQUIRE(linalg::find_linalg_misuse(ctx, *m).kind == linalg::LinalgMisuseKind::None);

    // TEXT round-trip: print -> parse -> re-walk both None + print==reprint byte-exact.
    const String t1 = print(ctx, *m, &root);
    Context      ctx2(&root);
    reg(ctx2);
    const ParseResult pr = parse(ctx2, StringView(t1.c_str(), t1.size()));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);
    CHECK(tensor::find_tensor_misuse(ctx2, *pr.module).kind == tensor::TensorMisuseKind::None);
    CHECK(linalg::find_linalg_misuse(ctx2, *pr.module).kind == linalg::LinalgMisuseKind::None);
    const String t2 = print(ctx2, *pr.module, &root);
    CHECK(StringView(t1.c_str(), t1.size()) == StringView(t2.c_str(), t2.size()));

    // BINARY round-trip: serialize -> deserialize -> re-walk both None.
    const containers::Array<u8> blob = serialize(ctx, *m, &root);
    REQUIRE(blob.size() > 0U);
    Context ctx3(&root);
    reg(ctx3);
    const ParseResult dr = deserialize(ctx3, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(dr.ok);
    REQUIRE(dr.module != nullptr);
    CHECK(tensor::find_tensor_misuse(ctx3, *dr.module).kind == tensor::TensorMisuseKind::None);
    CHECK(linalg::find_linalg_misuse(ctx3, *dr.module).kind == linalg::LinalgMisuseKind::None);
}
