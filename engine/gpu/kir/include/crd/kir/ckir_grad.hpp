#pragma once

// ckir_grad.hpp — Phase 3.1.6 v17-a: SYMBOLIC reverse-mode AD on CKIR-Graph — the tensor-level generalization of the
// v16-h scalar `graph_ad` reverse-AD. Given an output node + an output cotangent (seed), it emits the input gradients
// as NEW graph nodes (a VJP per primitive) — so once a kernel is expressed in CKIR it is differentiable on every
// backend FOR FREE (the JAX/XLA payoff, but deterministic + certified). The gradient graph is itself CKIR, so it
// evaluates on the CPU reference (FD-gated) and lowers to the GPU exactly like the forward. ADR-0098 / ADR-0097 §v16-h.
//
// HAZARD (learned): the builder grows m_nodes, so a held `const KNode&` dangles after the next emit. Every rule COPIES
// its node first (KNode is a small POD).

#include <crd/kir/ckir.hpp>

namespace crd::kir
{

namespace grad_detail
{
// permutation that swaps the last two axes (the matmul transpose).
// NOLINTNEXTLINE(readability-non-const-parameter) -- perm is written (perm[k]=...); the check misfires on header-only fns
inline void swap_last2(const Shape& sh, crd::u8* perm) noexcept
{
    for (int k = 0; k < sh.rank; ++k) { perm[k] = static_cast<crd::u8>(k); }
    if (sh.rank >= 2) { perm[sh.rank - 1] = static_cast<crd::u8>(sh.rank - 2); perm[sh.rank - 2] = static_cast<crd::u8>(sh.rank - 1); }
}
} // namespace grad_detail

// accumulate a cotangent contribution into cot[node] (sum over consumers = a fresh Add node).
inline void accumulate(KGraph& g, int* cot, int node, int contrib)
{
    if (contrib < 0) { return; }
    cot[node] = (cot[node] < 0) ? contrib : g.binary(KOp::Add, cot[node], contrib);
}

// reverse-AD: seed = a node holding dL/d(output) (same shape as output; for a scalar loss, constant 1). Fills
// grad_of_input[k] (k = Input node's index) with the node computing dL/dInput_k. `scratch` holds the cotangent map.
// NOLINTNEXTLINE(readability-non-const-parameter) -- scratch (allocate/deallocate) + grad_of_input (written) are non-const; the check misfires
inline void reverse_ad(KGraph& g, int output, int seed, crd::memory::IAllocator* scratch, int* grad_of_input, int n_inputs)
{
    const int n   = g.size();
    auto*     cot = static_cast<int*>(scratch->allocate(sizeof(int) * static_cast<crd::usize>(n), alignof(int)));
    for (int i = 0; i < n; ++i) { cot[i] = -1; }
    cot[output] = seed;

    for (int i = n - 1; i >= 0; --i)
    {
        if (cot[i] < 0) { continue; }
        const KNode    node = g.node(i); // COPY — g grows below
        const int      gi   = cot[i];
        const Shape    ash  = node.a >= 0 ? g.node(node.a).shape : Shape{};
        const DType    adt  = node.a >= 0 ? g.node(node.a).dtype() : node.dtype();
        const int      one  = g.constant(1.0, node.shape, node.dtype()); // reused as needed (CSE folds duplicates later)

        switch (node.op)
        {
        case KOp::Add: accumulate(g, cot, node.a, gi); accumulate(g, cot, node.b, gi); break;
        case KOp::Sub: accumulate(g, cot, node.a, gi); accumulate(g, cot, node.b, g.unary(KOp::Neg, gi)); break;
        case KOp::Mul:
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, node.b));
            accumulate(g, cot, node.b, g.binary(KOp::Mul, gi, node.a));
            break;
        case KOp::Div: // C=A/B: dA=gi/B, dB=-gi*C/B
            accumulate(g, cot, node.a, g.binary(KOp::Div, gi, node.b));
            accumulate(g, cot, node.b, g.unary(KOp::Neg, g.binary(KOp::Div, g.binary(KOp::Mul, gi, i), node.b)));
            break;
        case KOp::Neg: accumulate(g, cot, node.a, g.unary(KOp::Neg, gi)); break;
        case KOp::Recip: // C=1/A: dA=-gi*C*C
            accumulate(g, cot, node.a, g.unary(KOp::Neg, g.binary(KOp::Mul, gi, g.binary(KOp::Mul, i, i))));
            break;
        case KOp::Exp: accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, i)); break;           // dA=gi*exp(A)=gi*C
        case KOp::Log: accumulate(g, cot, node.a, g.binary(KOp::Div, gi, node.a)); break;       // dA=gi/A
        case KOp::Sin: accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, g.unary(KOp::Cos, node.a))); break;
        case KOp::Cos: accumulate(g, cot, node.a, g.unary(KOp::Neg, g.binary(KOp::Mul, gi, g.unary(KOp::Sin, node.a)))); break;
        case KOp::Sqrt: // C=sqrt(A): dA=0.5*gi/C
        {
            const int half = g.constant(0.5, node.shape, node.dtype());
            accumulate(g, cot, node.a, g.binary(KOp::Div, g.binary(KOp::Mul, gi, half), i));
            break;
        }
        case KOp::Tanh: // dA=gi*(1-C^2)
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, g.binary(KOp::Sub, one, g.binary(KOp::Mul, i, i))));
            break;
        case KOp::Abs: // dA=gi*sign(A)
        {
            const int z    = g.constant(0.0, ash, adt);
            const int negone = g.constant(-1.0, ash, adt);
            const int pos  = g.constant(1.0, ash, adt);
            const int lt   = g.binary(KOp::CmpLt, node.a, z);
            const int sign = g.select(lt, negone, pos);
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, sign));
            break;
        }
        case KOp::Max: // dA=gi*(A>=B), dB=gi*(A<B)
        {
            const int lt  = g.binary(KOp::CmpLt, node.a, node.b);
            const int ge  = g.binary(KOp::Sub, one, lt);
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, ge));
            accumulate(g, cot, node.b, g.binary(KOp::Mul, gi, lt));
            break;
        }
        case KOp::Min: // dA=gi*(A<=B), dB=gi*(B<A)
        {
            const int ltba = g.binary(KOp::CmpLt, node.b, node.a);
            const int le   = g.binary(KOp::Sub, one, ltba);
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, le));
            accumulate(g, cot, node.b, g.binary(KOp::Mul, gi, ltba));
            break;
        }
        case KOp::Select: // C=cond?a:b ; dA=gi*cond, dB=gi*(1-cond); cond is a predicate (no grad)
            accumulate(g, cot, node.a, g.binary(KOp::Mul, gi, node.c));
            accumulate(g, cot, node.b, g.binary(KOp::Mul, gi, g.binary(KOp::Sub, one, node.c)));
            break;
        case KOp::Cast: accumulate(g, cot, node.a, g.cast(gi, adt)); break;
        case KOp::Reshape: accumulate(g, cot, node.a, g.reshape(gi, ash)); break;
        case KOp::Permute: // dA = permute(gi, inverse perm)
        {
            crd::u8 inv[kMaxRank];
            for (int k = 0; k < node.shape.rank; ++k) { inv[node.perm[k]] = static_cast<crd::u8>(k); }
            accumulate(g, cot, node.a, g.permute(gi, inv));
            break;
        }
        case KOp::Broadcast: // dA = sum gi over the axes that were size-1 in A but expanded in the output
        {
            crd::u32 bmask = 0;
            for (int k = 0; k < node.shape.rank; ++k) { if (ash.dims[k] == 1 && node.shape.dims[k] > 1) { bmask |= (1U << k); } }
            accumulate(g, cot, node.a, g.reduce(KOp::ReduceSum, gi, bmask));
            break;
        }
        case KOp::ReduceSum: accumulate(g, cot, node.a, g.broadcast(gi, ash)); break;
        case KOp::ReduceMax: // dA = broadcast(gi) * (A == broadcast(out))
        {
            const int outb = g.broadcast(i, ash);
            const int lt   = g.binary(KOp::CmpLt, node.a, outb);
            const int onea = g.constant(1.0, ash, adt);
            const int mask = g.binary(KOp::Sub, onea, lt);
            accumulate(g, cot, node.a, g.binary(KOp::Mul, g.broadcast(gi, ash), mask));
            break;
        }
        case KOp::Contract: // C=A@B: dA=gi@B^T, dB=A^T@gi
        {
            const Shape bsh = g.node(node.b).shape;
            crd::u8     pa[kMaxRank];
            crd::u8     pb[kMaxRank];
            grad_detail::swap_last2(ash, pa);
            grad_detail::swap_last2(bsh, pb);
            accumulate(g, cot, node.a, g.contract(gi, g.permute(node.b, pb)));
            accumulate(g, cot, node.b, g.contract(g.permute(node.a, pa), gi));
            break;
        }
        default: break; // leaves (Input/Const/Iota) + CmpLt (predicate): no operand cotangent
        }
    }

    for (int k = 0; k < n_inputs; ++k) { grad_of_input[k] = -1; }
    for (int i = 0; i < n; ++i) { const KNode& nd = g.node(i); if (nd.op == KOp::Input) { grad_of_input[nd.iidx] = cot[i]; } }
    scratch->deallocate(cot);
}

} // namespace crd::kir
