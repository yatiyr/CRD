// CKIR contract fatals (crd-kir-fatal-* CTests, tests/gpu/kir). The CPU reference interpreter and the reverse-mode AD
// take their working memory from a caller's scratch allocator; one that refuses a block (a sub-budget allocator returns
// nullptr) leaves them out of memory, fatal in every build, never a null buffer written (DIAG.3a; they wrote through
// the refused pointer before). One mode per run (argv[1]); see expected_fatal.hpp for the exit protocol:
//   eval-scratch-refused            eval_cpu: the per-node materialization buffer is refused
//   eval-attention-scratch-refused  eval_cpu of an Attention node: its row of scores is refused
//   reverse-ad-scratch-refused      reverse_ad: the cotangent table is refused
// The graph lives in its own allocator; the scratch allocator refuses exactly the request the mode is about
// (refusing_allocator.hpp), so it is the run's first refused one.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_grad.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace
{
namespace kir = crd::kir;
using crd_diag_fatal::RefusingAllocator;

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (is(mode, "eval-scratch-refused"))
    {
        crd_diag_fatal::arm(mode, kir::eval_detail::kEvalScratchOutOfMemory);
        kir::KGraph g(crd::memory::default_allocator());
        const int x = g.input(kir::make_shape({4, 4}), kir::DType::F64);
        const int y = g.unary(kir::KOp::Tanh, x);
        // Two nodes of 16 values each: the materialization buffer is 32 f64, a request no other scratch block of the
        // run makes (the offset table holds 8 i64).
        RefusingAllocator scratch("kir-fatal-refusing");
        scratch.refuse_shape(32U * sizeof(crd::f64), alignof(crd::f64));
        crd::f64 xv[16] = {};
        const crd::f64* const inputs[] = {xv};
        crd::f64 out[16] = {};
        kir::eval_cpu(g, inputs, &scratch, y, out);
        return true;
    }
    if (is(mode, "eval-attention-scratch-refused"))
    {
        crd_diag_fatal::arm(mode, kir::eval_detail::kEvalScratchOutOfMemory);
        kir::KGraph g(crd::memory::default_allocator());
        const int q = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int k = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int v = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int o = g.attention(q, k, v, 0.5);
        // A sequence of 3: the attention node's row of scores is 3 f64, a request no other scratch block makes.
        RefusingAllocator scratch("kir-fatal-refusing");
        scratch.refuse_shape(3U * sizeof(crd::f64), alignof(crd::f64));
        crd::f64 qv[6] = {0.5, 1.0, -0.5, 0.25, 0.0, 1.0};
        crd::f64 kv[6] = {1.0, 0.0, 0.5, -1.0, 0.25, 0.5};
        crd::f64 vv[6] = {2.0, 1.0, 0.0, 3.0, 1.0, 1.0};
        const crd::f64* const inputs[] = {qv, kv, vv};
        crd::f64 out[6] = {};
        kir::eval_cpu(g, inputs, &scratch, o, out);
        return true;
    }
    if (is(mode, "reverse-ad-scratch-refused"))
    {
        crd_diag_fatal::arm(mode, kir::grad_detail::kReverseAdScratchOutOfMemory);
        kir::KGraph g(crd::memory::default_allocator());
        const int x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int t = g.unary(kir::KOp::Tanh, x);
        const int loss = g.reduce(kir::KOp::ReduceSum, t, 0x3U);
        const int seed = g.constant(1.0, g.node(loss).shape, kir::DType::F64);
        RefusingAllocator scratch("kir-fatal-refusing");
        scratch.refuse_all(); // reverse_ad's first scratch block is the cotangent table
        int grad_in[1] = {-1};
        kir::reverse_ad(g, loss, seed, &scratch, grad_in, 1);
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
