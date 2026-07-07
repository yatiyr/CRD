#pragma once

// taylor_tape.hpp — Phase 3.1.6 v15-g: the O(K²)-per-step TAYLOR-TAPE ODE integrator (TIDES-class). The generic
// order-by-order integrator (`taylor_ode.hpp`) re-evaluates the whole RHS once per order → O(K³)/step. Here we RECORD
// the RHS's operation graph ONCE (operator overloading onto a tape), then propagate every node's Taylor coefficients
// ORDER BY ORDER: at order m each op costs one length-m convolution, so a full order-K local series is O(K²)/step —
// a ~K× speedup that lets the Taylor integrator beat adaptive RK across the whole tolerance range, not only on the
// high-precision frontier. Determinism preserved (crd::math recurrences). ADR-0097.

#include <crd/hesap/autodiff/taylor_ode.hpp> // taylor_step_size

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

enum class TapeOp : unsigned char
{
    leaf_y,  // the state y (coeffs fed in progressively)
    leaf_t,  // the independent variable t = {t,1,0,…}
    konst,   // a constant {s,0,…}
    slave,   // an auxiliary node computed by its master (skip in the sweep)
    neg, add, sub, mul, div,
    sadd,    // x + s
    smul,    // s·x
    rsub,    // s − x
    exp_, log_, sqrt_, sin_, cos_, tanh_, pow_
};

// Fixed-capacity tape. Nodes are stored in creation (topological) order, so a node's inputs always precede it.
template <int K, int MaxN = 64>
struct TaylorTape
{
    int          n = 0;
    TapeOp       op[MaxN];
    int          ain[MaxN];
    int          bin[MaxN];
    int          aux[MaxN]; // slave (coupled functions)
    crd::f64     s[MaxN];   // scalar param / constant value
    crd::f64     c[MaxN][K + 1];
    int          yn = -1;
    int          tn = -1;
    int          outn = -1;

    int emit(TapeOp o, int a, int b, crd::f64 sc, int ax) noexcept
    {
        const int i = n;
        op[i]       = o;
        ain[i]      = a;
        bin[i]      = b;
        aux[i]      = ax;
        s[i]        = sc;
        ++n;
        return i;
    }
};

// Recording handle. Overloaded ops append to the tape and return a fresh node.
template <int K, int MaxN>
struct TT
{
    TaylorTape<K, MaxN>* t;
    int                  i;
};

template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator-(TT<K, MaxN> x) noexcept { return {x.t, x.t->emit(TapeOp::neg, x.i, -1, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator+(TT<K, MaxN> x, TT<K, MaxN> y) noexcept { return {x.t, x.t->emit(TapeOp::add, x.i, y.i, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator-(TT<K, MaxN> x, TT<K, MaxN> y) noexcept { return {x.t, x.t->emit(TapeOp::sub, x.i, y.i, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator*(TT<K, MaxN> x, TT<K, MaxN> y) noexcept { return {x.t, x.t->emit(TapeOp::mul, x.i, y.i, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator/(TT<K, MaxN> x, TT<K, MaxN> y) noexcept { return {x.t, x.t->emit(TapeOp::div, x.i, y.i, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator+(TT<K, MaxN> x, crd::f64 s) noexcept { return {x.t, x.t->emit(TapeOp::sadd, x.i, -1, s, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator+(crd::f64 s, TT<K, MaxN> x) noexcept { return x + s; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator-(TT<K, MaxN> x, crd::f64 s) noexcept { return {x.t, x.t->emit(TapeOp::sadd, x.i, -1, -s, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator-(crd::f64 s, TT<K, MaxN> x) noexcept { return {x.t, x.t->emit(TapeOp::rsub, x.i, -1, s, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator*(TT<K, MaxN> x, crd::f64 s) noexcept { return {x.t, x.t->emit(TapeOp::smul, x.i, -1, s, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> operator*(crd::f64 s, TT<K, MaxN> x) noexcept { return x * s; }

template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> exp(TT<K, MaxN> x) noexcept { return {x.t, x.t->emit(TapeOp::exp_, x.i, -1, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> log(TT<K, MaxN> x) noexcept { return {x.t, x.t->emit(TapeOp::log_, x.i, -1, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> sqrt(TT<K, MaxN> x) noexcept { return {x.t, x.t->emit(TapeOp::sqrt_, x.i, -1, 0.0, -1)}; }
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> pow(TT<K, MaxN> x, crd::f64 p) noexcept { return {x.t, x.t->emit(TapeOp::pow_, x.i, -1, p, -1)}; }
// coupled: primary node + a slave holding the companion series (cos for sin, sin for cos, 1−t² for tanh).
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> sin(TT<K, MaxN> x) noexcept
{
    const int p  = x.t->emit(TapeOp::sin_, x.i, -1, 0.0, -1);
    const int sl = x.t->emit(TapeOp::slave, -1, -1, 0.0, -1);
    x.t->aux[p]  = sl;
    return {x.t, p};
}
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> cos(TT<K, MaxN> x) noexcept
{
    const int p  = x.t->emit(TapeOp::cos_, x.i, -1, 0.0, -1);
    const int sl = x.t->emit(TapeOp::slave, -1, -1, 0.0, -1);
    x.t->aux[p]  = sl;
    return {x.t, p};
}
template <int K, int MaxN>
[[nodiscard]] inline TT<K, MaxN> tanh(TT<K, MaxN> x) noexcept
{
    const int p  = x.t->emit(TapeOp::tanh_, x.i, -1, 0.0, -1);
    const int sl = x.t->emit(TapeOp::slave, -1, -1, 0.0, -1);
    x.t->aux[p]  = sl;
    return {x.t, p};
}

// Compute every node's order-m coefficient (leaves are set externally / by their master). O(m) per op-node.
template <int K, int MaxN>
inline void tape_eval_order(TaylorTape<K, MaxN>& T, int m) noexcept
{
    for (int i = 0; i < T.n; ++i)
    {
        crd::f64* ci = T.c[i];
        switch (T.op[i])
        {
        case TapeOp::leaf_y:
        case TapeOp::leaf_t:
        case TapeOp::konst:
        case TapeOp::slave: break; // set elsewhere
        case TapeOp::neg: ci[m] = -T.c[T.ain[i]][m]; break;
        case TapeOp::add: ci[m] = T.c[T.ain[i]][m] + T.c[T.bin[i]][m]; break;
        case TapeOp::sub: ci[m] = T.c[T.ain[i]][m] - T.c[T.bin[i]][m]; break;
        case TapeOp::sadd: ci[m] = T.c[T.ain[i]][m] + (m == 0 ? T.s[i] : 0.0); break;
        case TapeOp::smul: ci[m] = T.s[i] * T.c[T.ain[i]][m]; break;
        case TapeOp::rsub: ci[m] = (m == 0 ? T.s[i] : 0.0) - T.c[T.ain[i]][m]; break;
        case TapeOp::mul:
        {
            const crd::f64* a = T.c[T.ain[i]];
            const crd::f64* b = T.c[T.bin[i]];
            crd::f64        acc = 0.0;
            for (int j = 0; j <= m; ++j) { acc += a[j] * b[m - j]; }
            ci[m] = acc;
            break;
        }
        case TapeOp::div:
        {
            const crd::f64* a = T.c[T.ain[i]];
            const crd::f64* b = T.c[T.bin[i]];
            crd::f64        acc = a[m];
            for (int j = 0; j < m; ++j) { acc -= ci[j] * b[m - j]; }
            ci[m] = acc / b[0];
            break;
        }
        case TapeOp::exp_:
        {
            const crd::f64* a = T.c[T.ain[i]];
            if (m == 0) { ci[0] = crd::math::exp(a[0]); }
            else
            {
                crd::f64 acc = 0.0;
                for (int j = 0; j < m; ++j) { acc += static_cast<crd::f64>(m - j) * a[m - j] * ci[j]; }
                ci[m] = acc / static_cast<crd::f64>(m);
            }
            break;
        }
        case TapeOp::log_:
        {
            const crd::f64* a = T.c[T.ain[i]];
            if (m == 0) { ci[0] = crd::math::log(a[0]); }
            else
            {
                crd::f64 acc = 0.0;
                for (int j = 1; j < m; ++j) { acc += static_cast<crd::f64>(j) * a[m - j] * ci[j]; }
                ci[m] = (a[m] - acc / static_cast<crd::f64>(m)) / a[0];
            }
            break;
        }
        case TapeOp::sqrt_:
        {
            const crd::f64* a = T.c[T.ain[i]];
            if (m == 0) { ci[0] = crd::math::sqrt(a[0]); }
            else
            {
                crd::f64 acc = a[m];
                for (int j = 1; j < m; ++j) { acc -= ci[j] * ci[m - j]; }
                ci[m] = acc / (2.0 * ci[0]);
            }
            break;
        }
        case TapeOp::sin_: // ci = sin, co = aux = cos
        {
            const crd::f64* a  = T.c[T.ain[i]];
            crd::f64*       co = T.c[T.aux[i]];
            if (m == 0) { crd::math::sincos(a[0], ci[0], co[0]); }
            else
            {
                crd::f64 sacc = 0.0;
                crd::f64 cacc = 0.0;
                for (int j = 0; j < m; ++j)
                {
                    const crd::f64 w = static_cast<crd::f64>(m - j) * a[m - j];
                    sacc += w * co[j];
                    cacc += w * ci[j];
                }
                ci[m] = sacc / static_cast<crd::f64>(m);
                co[m] = -cacc / static_cast<crd::f64>(m);
            }
            break;
        }
        case TapeOp::cos_: // ci = cos, si = aux = sin
        {
            const crd::f64* a  = T.c[T.ain[i]];
            crd::f64*       si = T.c[T.aux[i]];
            if (m == 0) { crd::math::sincos(a[0], si[0], ci[0]); }
            else
            {
                crd::f64 sacc = 0.0;
                crd::f64 cacc = 0.0;
                for (int j = 0; j < m; ++j)
                {
                    const crd::f64 w = static_cast<crd::f64>(m - j) * a[m - j];
                    sacc += w * ci[j]; // sin from cos
                    cacc += w * si[j]; // cos from sin
                }
                si[m] = sacc / static_cast<crd::f64>(m);
                ci[m] = -cacc / static_cast<crd::f64>(m);
            }
            break;
        }
        case TapeOp::tanh_: // ci = tanh, w = aux = 1 − tanh²
        {
            const crd::f64* a = T.c[T.ain[i]];
            crd::f64*       w = T.c[T.aux[i]];
            if (m == 0)
            {
                ci[0] = crd::math::tanh(a[0]);
                w[0]  = 1.0 - ci[0] * ci[0];
            }
            else
            {
                crd::f64 acc = 0.0;
                for (int j = 0; j < m; ++j) { acc += static_cast<crd::f64>(m - j) * a[m - j] * w[j]; }
                ci[m]     = acc / static_cast<crd::f64>(m);
                crd::f64 q = 0.0;
                for (int j = 0; j <= m; ++j) { q += ci[j] * ci[m - j]; }
                w[m] = -q;
            }
            break;
        }
        case TapeOp::pow_:
        {
            const crd::f64* a = T.c[T.ain[i]];
            const crd::f64  p = T.s[i];
            if (m == 0) { ci[0] = crd::math::pow(a[0], p); }
            else
            {
                crd::f64 acc = 0.0;
                for (int j = 0; j < m; ++j)
                {
                    acc += (p * static_cast<crd::f64>(m - j) - static_cast<crd::f64>(j)) * a[m - j] * ci[j];
                }
                ci[m] = acc / (static_cast<crd::f64>(m) * a[0]);
            }
            break;
        }
        }
    }
}

// Integrate y'=rhs(y,t) with the taped O(K²)/step method. Records the RHS once, then steps. Returns y(t_end).
template <int K, int MaxN = 64, class F>
[[nodiscard]] inline crd::f64 taylor_solve_taped(const F& rhs, crd::f64 t0, crd::f64 y0, crd::f64 t_end, crd::f64 tol,
                                                 int* nsteps = nullptr) noexcept
{
    TaylorTape<K, MaxN> tape;
    tape.yn = tape.emit(TapeOp::leaf_y, -1, -1, 0.0, -1);
    tape.tn = tape.emit(TapeOp::leaf_t, -1, -1, 0.0, -1);
    const TT<K, MaxN> yv{&tape, tape.yn};
    const TT<K, MaxN> tv{&tape, tape.tn};
    const TT<K, MaxN> out = rhs(yv, tv);
    tape.outn             = out.i;

    crd::f64 t      = t0;
    crd::f64 y      = y0;
    crd::f64 h_prev = 0.0;
    crd::f64 yc[K + 1];
    int      steps = 0;
    while (t < t_end)
    {
        // leaf coefficients for this step: t = {t,1,0,…}; y filled in progressively below.
        tape.c[tape.tn][0] = t;
        if constexpr (K >= 1) { tape.c[tape.tn][1] = 1.0; }
        for (int i = 2; i <= K; ++i) { tape.c[tape.tn][i] = 0.0; }
        tape.c[tape.yn][0] = y;
        yc[0]              = y;
        for (int m = 0; m < K; ++m)
        {
            tape_eval_order<K, MaxN>(tape, m);
            const crd::f64 rhs_m = tape.c[tape.outn][m];
            yc[m + 1]            = rhs_m / static_cast<crd::f64>(m + 1);
            tape.c[tape.yn][m + 1] = yc[m + 1];
        }
        crd::f64 h = taylor_step_size<K>(yc, tol, h_prev);
        if (t + h > t_end) { h = t_end - t; }
        crd::f64 yn = yc[K];
        for (int k = K - 1; k >= 0; --k) { yn = yn * h + yc[k]; }
        t += h;
        y      = yn;
        h_prev = h;
        ++steps;
    }
    if (nsteps != nullptr) { *nsteps = steps; }
    return y;
}

template <class F>
[[nodiscard]] inline crd::f64 taylor_solve_taped_auto(const F& rhs, crd::f64 t0, crd::f64 y0, crd::f64 t_end,
                                                      crd::f64 tol, int* nsteps = nullptr) noexcept
{
    if (tol >= 1e-4) { return taylor_solve_taped<6>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-7) { return taylor_solve_taped<10>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-10) { return taylor_solve_taped<14>(rhs, t0, y0, t_end, tol, nsteps); }
    if (tol >= 1e-13) { return taylor_solve_taped<18>(rhs, t0, y0, t_end, tol, nsteps); }
    return taylor_solve_taped<24>(rhs, t0, y0, t_end, tol, nsteps);
}

} // namespace crd::hesap::autodiff::forward
