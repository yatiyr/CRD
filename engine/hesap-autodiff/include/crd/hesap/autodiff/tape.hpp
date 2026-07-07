#pragma once

// tape.hpp — Phase 3.1.6 v16-a: the DETERMINISTIC reverse-mode tape — the foundation of the differentiation moat.
// A reverse-mode `Var` records its computation onto a `Tape` (a Wengert list) during the forward pass; `backward()`
// then replays the tape in REVERSE index order, propagating each node's adjoint to its operands
// (`operand.adjoint += local_partial · node.adjoint`). One backward pass yields the WHOLE gradient ∇f — the reverse
// advantage: O(1) passes for n partials, where forward mode needs n and finite differences need n+1 evaluations.
//
// ★ THE MOAT — DETERMINISTIC GRADIENTS. The accumulation is a fixed reverse-index-order sum with NO float atomics:
// operands always precede their result (indices are assigned in evaluation order), so a node's contributions are
// applied in a single, worker-count-independent order. PyTorch/JAX scatter adjoints through non-associative atomic
// adds ⇒ run-to-run drift (O(1e-4)); Cerid's tape is bit-identical run-to-run and (with the deterministic
// data-parallel reduction, `batch_gradient`) across `{1..16}` workers and batch layouts. Arena-owned (IAllocator),
// no per-op malloc (Stan Math / Adept allocate the tape). A VJP is the TRANSPOSE of the v15 JVP — the local partials
// reuse the audited `forward::detail` slopes (one rule library, two modes). ADR-0097.

#include <crd/hesap/autodiff/detail/jvp_rules.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::autodiff::reverse
{

namespace fwd_detail = crd::hesap::autodiff::forward::detail;

// The tape: SoA Wengert list. node i has value m_value[i] + adjoint m_adjoint[i]; its operands (input node index +
// local partial ∂node_i/∂operand) live in [m_op_beg[i], m_op_end[i]) of the flat operand arrays. Leaves have none.
class Tape
{
public:
    explicit Tape(crd::memory::IAllocator* alloc) noexcept
        : m_value(alloc), m_adjoint(alloc), m_op_beg(alloc), m_op_end(alloc), m_in_idx(alloc), m_in_par(alloc)
    {
    }

    [[nodiscard]] crd::u32 leaf(crd::f64 v) { return push(v); }
    [[nodiscard]] crd::u32 unary(crd::f64 v, crd::u32 a, crd::f64 pa)
    {
        const crd::u32 beg = static_cast<crd::u32>(m_in_idx.size());
        m_in_idx.push_back(a);
        m_in_par.push_back(pa);
        return push(v, beg);
    }
    [[nodiscard]] crd::u32 binary(crd::f64 v, crd::u32 a, crd::f64 pa, crd::u32 b, crd::f64 pb)
    {
        const crd::u32 beg = static_cast<crd::u32>(m_in_idx.size());
        m_in_idx.push_back(a);
        m_in_par.push_back(pa);
        m_in_idx.push_back(b);
        m_in_par.push_back(pb);
        return push(v, beg);
    }

    [[nodiscard]] crd::f64  value(crd::u32 id) const noexcept { return m_value[id]; }
    [[nodiscard]] crd::f64  grad(crd::u32 id) const noexcept { return m_adjoint[id]; }
    void                    seed(crd::u32 id, crd::f64 a) noexcept { m_adjoint[id] = a; }
    [[nodiscard]] crd::usize num_nodes() const noexcept { return m_value.size(); }

    // Propagate adjoints in reverse index order — deterministic, no atomics. Seed the output adjoint(s) first.
    void backward() noexcept
    {
        const crd::usize n = m_value.size();
        for (crd::usize ii = n; ii-- > 0;)
        {
            const crd::f64 a = m_adjoint[ii];
            if (a == 0.0) { continue; }
            const crd::u32 beg = m_op_beg[ii];
            const crd::u32 end = m_op_end[ii];
            for (crd::u32 k = beg; k < end; ++k) { m_adjoint[m_in_idx[k]] += m_in_par[k] * a; }
        }
    }

    void zero_adjoints() noexcept
    {
        for (crd::usize i = 0; i < m_adjoint.size(); ++i) { m_adjoint[i] = 0.0; }
    }
    void reset() noexcept // reuse the tape (keeps capacity — arena-friendly)
    {
        m_value.resize(0);
        m_adjoint.resize(0);
        m_op_beg.resize(0);
        m_op_end.resize(0);
        m_in_idx.resize(0);
        m_in_par.resize(0);
    }

private:
    crd::u32 push(crd::f64 v, crd::u32 beg) // op node
    {
        const crd::u32 id = static_cast<crd::u32>(m_value.size());
        m_value.push_back(v);
        m_adjoint.push_back(0.0);
        m_op_beg.push_back(beg);
        m_op_end.push_back(static_cast<crd::u32>(m_in_idx.size()));
        return id;
    }
    crd::u32 push(crd::f64 v) // leaf (no operands)
    {
        const crd::u32 beg = static_cast<crd::u32>(m_in_idx.size());
        return push(v, beg);
    }

    crd::containers::Array<crd::f64> m_value;
    crd::containers::Array<crd::f64> m_adjoint;
    crd::containers::Array<crd::u32> m_op_beg;
    crd::containers::Array<crd::u32> m_op_end;
    crd::containers::Array<crd::u32> m_in_idx; // flat operand: input node index
    crd::containers::Array<crd::f64> m_in_par; // flat operand: local partial
};

// The reverse-mode variable — value carried on the tape, referenced by node index. Trivially copyable.
struct Var
{
    Tape*    tape = nullptr;
    crd::u32 node = 0;
    [[nodiscard]] crd::f64 val() const noexcept { return tape->value(node); }
};

[[nodiscard]] inline Var make_leaf(Tape& t, crd::f64 v) noexcept { return Var{&t, t.leaf(v)}; }

// ---- arithmetic (each records its local partials; the backward transposes them) -----------------------------
[[nodiscard]] inline Var operator+(Var a, Var b) noexcept { return Var{a.tape, a.tape->binary(a.val() + b.val(), a.node, 1.0, b.node, 1.0)}; }
[[nodiscard]] inline Var operator-(Var a, Var b) noexcept { return Var{a.tape, a.tape->binary(a.val() - b.val(), a.node, 1.0, b.node, -1.0)}; }
[[nodiscard]] inline Var operator*(Var a, Var b) noexcept { return Var{a.tape, a.tape->binary(a.val() * b.val(), a.node, b.val(), b.node, a.val())}; }
[[nodiscard]] inline Var operator/(Var a, Var b) noexcept
{
    const crd::f64 inv = 1.0 / b.val();
    const crd::f64 q   = a.val() * inv;
    return Var{a.tape, a.tape->binary(q, a.node, inv, b.node, -q * inv)}; // ∂/∂a = 1/b, ∂/∂b = −a/b²
}
[[nodiscard]] inline Var operator-(Var a) noexcept { return Var{a.tape, a.tape->unary(-a.val(), a.node, -1.0)}; }

// mixed scalar (the constant is baked into value + partial — no standalone constant node needed)
[[nodiscard]] inline Var operator+(Var a, crd::f64 s) noexcept { return Var{a.tape, a.tape->unary(a.val() + s, a.node, 1.0)}; }
[[nodiscard]] inline Var operator+(crd::f64 s, Var a) noexcept { return a + s; }
[[nodiscard]] inline Var operator-(Var a, crd::f64 s) noexcept { return Var{a.tape, a.tape->unary(a.val() - s, a.node, 1.0)}; }
[[nodiscard]] inline Var operator-(crd::f64 s, Var a) noexcept { return Var{a.tape, a.tape->unary(s - a.val(), a.node, -1.0)}; }
[[nodiscard]] inline Var operator*(Var a, crd::f64 s) noexcept { return Var{a.tape, a.tape->unary(a.val() * s, a.node, s)}; }
[[nodiscard]] inline Var operator*(crd::f64 s, Var a) noexcept { return a * s; }
[[nodiscard]] inline Var operator/(Var a, crd::f64 s) noexcept { const crd::f64 inv = 1.0 / s; return Var{a.tape, a.tape->unary(a.val() * inv, a.node, inv)}; }

// ---- transcendentals (value from crd::math; local partial from the audited forward slopes) ------------------
[[nodiscard]] inline Var exp(Var a) noexcept { const crd::f64 e = crd::math::exp(a.val()); return Var{a.tape, a.tape->unary(e, a.node, e)}; }
[[nodiscard]] inline Var log(Var a) noexcept { return Var{a.tape, a.tape->unary(crd::math::log(a.val()), a.node, 1.0 / a.val())}; }
[[nodiscard]] inline Var sqrt(Var a) noexcept { const crd::f64 s = crd::math::sqrt(a.val()); return Var{a.tape, a.tape->unary(s, a.node, fwd_detail::d_sqrt(s))}; }
[[nodiscard]] inline Var tanh(Var a) noexcept { const crd::f64 t = crd::math::tanh(a.val()); return Var{a.tape, a.tape->unary(t, a.node, 1.0 - t * t)}; }
[[nodiscard]] inline Var sin(Var a) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(a.val(), s, c);
    return Var{a.tape, a.tape->unary(s, a.node, c)};
}
[[nodiscard]] inline Var cos(Var a) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(a.val(), s, c);
    return Var{a.tape, a.tape->unary(c, a.node, -s)};
}
[[nodiscard]] inline Var pow(Var a, crd::f64 p) noexcept
{
    const auto r = fwd_detail::pow_const(a.val(), p);
    return Var{a.tape, a.tape->unary(r.value, a.node, r.dbase)};
}

} // namespace crd::hesap::autodiff::reverse
