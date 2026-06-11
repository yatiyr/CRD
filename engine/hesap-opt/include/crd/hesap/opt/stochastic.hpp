#pragma once

// stochastic.hpp — Phase 3.1.6 v7-i: the STOCHASTIC / ML optimizer family — per-step UPDATE RULES (steppers,
// the PyTorch `torch.optim` shape: the caller owns the training loop and supplies the (mini)batch gradient;
// the stepper owns its moment state and applies one in-place update). Implemented to the REFERENCE update
// formulas with PyTorch's documented default semantics, so step-level closed forms are testable exactly:
//   SGD (+momentum/Nesterov/dampening/L2) · Adam · AdamW (decoupled decay) · Nadam (Dozat, with the PyTorch
//   μ-product schedule) · RAdam (Liu et al. rectification, ρ>5 gate) · RMSprop (+momentum) · Adagrad ·
//   Adadelta · Lion (Chen et al. 2023, sign update) · LAMB (You et al., trust ratio; SINGLE parameter group —
//   layer-wise grouping is the caller's slicing).
// Plus LR schedules (step/exponential/cosine-annealing/linear-warmup — pure functions of t) and gradient
// clipping (by global norm / by value, torch semantics).
//
// DETERMINISM MOAT (the differentiator no mainstream ML optimizer carries): every stepper is a SERIAL
// fixed-order scalar recurrence ⇒ given bit-exact gradients (e.g. over the parallel-but-bit-exact spmv) the
// TRAINING TRAJECTORY is bit-identical across {1..16} workers; reproducible minibatch SAMPLING is the
// Philox-backed `minibatch.hpp` (counter-based ⇒ same (seed, epoch) ⇒ same batches, by construction).
//
// These steppers deliberately do NOT return OptResult — training loops are caller-owned (the torch shape);
// the minimize_* drivers remain the right tool for deterministic full-batch optimization. Full trajectory
// parity vs actual PyTorch runs is the v7-z gold-standard scoreboard (gated script); the in-tree gates are
// the exact closed-form first/second steps of each rule. ADR-0090.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::opt
{

// ---------------------------------------------------------------- gradient clipping (torch semantics)

// Scale `g` in place so ‖g‖₂ ≤ max_norm; returns the PRE-clip norm (torch clip_grad_norm_).
template <typename T> inline T clip_grad_norm(crd::containers::Span<T> g, T max_norm) noexcept
{
    T nrm_sq = static_cast<T>(0);
    for (crd::usize i = 0; i < g.size(); ++i)
    {
        nrm_sq += g[i] * g[i];
    }
    const T nrm = std::sqrt(nrm_sq);
    if (nrm > max_norm && nrm > static_cast<T>(0))
    {
        const T scale = max_norm / nrm;
        for (crd::usize i = 0; i < g.size(); ++i)
        {
            g[i] *= scale;
        }
    }
    return nrm;
}

// Clamp each component of `g` into [−clip, clip] (torch clip_grad_value_).
template <typename T> inline void clip_grad_value(crd::containers::Span<T> g, T clip) noexcept
{
    for (crd::usize i = 0; i < g.size(); ++i)
    {
        g[i] = g[i] > clip ? clip : (g[i] < -clip ? -clip : g[i]);
    }
}

// ---------------------------------------------------------------- LR schedules (pure functions of the step)

template <typename T> [[nodiscard]] inline T lr_step_decay(T base, crd::usize t, crd::usize step_size, T gamma) noexcept
{
    T lr = base;
    for (crd::usize k = step_size; k <= t; k += step_size)
    {
        lr *= gamma; // integer-stepped ⇒ exact + deterministic (no pow rounding surprises)
    }
    return lr;
}

template <typename T> [[nodiscard]] inline T lr_exponential(T base, crd::usize t, T gamma) noexcept
{
    T lr = base;
    for (crd::usize k = 0; k < t; ++k)
    {
        lr *= gamma;
    }
    return lr;
}

// Cosine annealing (Loshchilov-Hutter SGDR, single cycle): η_t = η_min + ½(base−η_min)(1+cos(πt/T)).
template <typename T>
[[nodiscard]] inline T lr_cosine_annealing(T base, crd::usize t, crd::usize t_max,
                                           T eta_min = static_cast<T>(0)) noexcept
{
    constexpr T pi = static_cast<T>(3.14159265358979323846);
    const T frac = t_max > 0 ? static_cast<T>(t) / static_cast<T>(t_max) : static_cast<T>(1);
    return eta_min + static_cast<T>(0.5) * (base - eta_min) * (static_cast<T>(1) + std::cos(pi * frac));
}

// Linear warmup over the first `warmup` steps, then the base rate.
template <typename T> [[nodiscard]] inline T lr_linear_warmup(T base, crd::usize t, crd::usize warmup) noexcept
{
    if (warmup == 0 || t >= warmup)
    {
        return base;
    }
    return base * static_cast<T>(t + 1) / static_cast<T>(warmup);
}

// ---------------------------------------------------------------- SGD (+momentum / Nesterov)

template <typename T> struct SgdConfig
{
    T lr = static_cast<T>(1e-2);
    T momentum = static_cast<T>(0);     // μ; 0 ⇒ plain SGD
    T dampening = static_cast<T>(0);    // τ: v ← μv + (1−τ)g
    T weight_decay = static_cast<T>(0); // L2: g ← g + wd·x (COUPLED — torch SGD semantics)
    bool nesterov = false;              // step uses g + μ·v (requires momentum > 0, dampening = 0)
};

template <typename T> class SgdOptimizer
{
public:
    SgdOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const SgdConfig<T>& cfg) : m_cfg(cfg), m_v(alloc)
    {
        CRD_ASSERT_MSG(!cfg.nesterov || (cfg.momentum > static_cast<T>(0) && cfg.dampening == static_cast<T>(0)),
                       "SGD: nesterov requires momentum > 0 and zero dampening (torch contract)");
        m_v.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_v[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        for (crd::usize i = 0; i < n; ++i)
        {
            T g = grad[i] + m_cfg.weight_decay * x[i];
            if (m_cfg.momentum > static_cast<T>(0))
            {
                // torch: the FIRST momentum buffer is g itself (not (1−τ)g).
                m_v[i] = m_t == 1 ? g : m_cfg.momentum * m_v[i] + (static_cast<T>(1) - m_cfg.dampening) * g;
                g = m_cfg.nesterov ? g + m_cfg.momentum * m_v[i] : m_v[i];
            }
            x[i] -= m_cfg.lr * g;
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    SgdConfig<T> m_cfg;
    crd::containers::Array<T> m_v;
    crd::usize m_t = 0;
};

// ---------------------------------------------------------------- Adam / AdamW

template <typename T> struct AdamConfig
{
    T lr = static_cast<T>(1e-3);
    T beta1 = static_cast<T>(0.9);
    T beta2 = static_cast<T>(0.999);
    T eps = static_cast<T>(1e-8);
    T weight_decay = static_cast<T>(0); // Adam: COUPLED L2 (g += wd·x); AdamW: DECOUPLED (x ← x − lr·wd·x)
    bool decoupled = false;             // false = Adam, true = AdamW
};

template <typename T> class AdamOptimizer
{
public:
    AdamOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const AdamConfig<T>& cfg)
        : m_cfg(cfg), m_m(alloc), m_v(alloc)
    {
        m_m.resize(n);
        m_v.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_m[i] = static_cast<T>(0);
            m_v[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        m_b1t *= m_cfg.beta1; // β1^t, β2^t as exact running products (no pow)
        m_b2t *= m_cfg.beta2;
        const T bc1 = static_cast<T>(1) - m_b1t;
        const T bc2 = static_cast<T>(1) - m_b2t;
        for (crd::usize i = 0; i < n; ++i)
        {
            T g = grad[i];
            if (m_cfg.decoupled)
            {
                x[i] -= m_cfg.lr * m_cfg.weight_decay * x[i]; // AdamW: decoupled decay (Loshchilov-Hutter)
            }
            else
            {
                g += m_cfg.weight_decay * x[i]; // Adam: coupled L2
            }
            m_m[i] = m_cfg.beta1 * m_m[i] + (static_cast<T>(1) - m_cfg.beta1) * g;
            m_v[i] = m_cfg.beta2 * m_v[i] + (static_cast<T>(1) - m_cfg.beta2) * g * g;
            const T mhat = m_m[i] / bc1;
            const T vhat = m_v[i] / bc2;
            x[i] -= m_cfg.lr * mhat / (std::sqrt(vhat) + m_cfg.eps);
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    AdamConfig<T> m_cfg;
    crd::containers::Array<T> m_m;
    crd::containers::Array<T> m_v;
    crd::usize m_t = 0;
    T m_b1t = static_cast<T>(1);
    T m_b2t = static_cast<T>(1);
};

// ---------------------------------------------------------------- Nadam (Dozat; the PyTorch μ-product schedule)

template <typename T> struct NadamConfig
{
    T lr = static_cast<T>(2e-3);
    T beta1 = static_cast<T>(0.9);
    T beta2 = static_cast<T>(0.999);
    T eps = static_cast<T>(1e-8);
    T weight_decay = static_cast<T>(0);      // coupled L2
    T momentum_decay = static_cast<T>(4e-3); // ψ in μ_t = β1·(1 − ½·0.96^{tψ})
};

template <typename T> class NadamOptimizer
{
public:
    NadamOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const NadamConfig<T>& cfg)
        : m_cfg(cfg), m_m(alloc), m_v(alloc)
    {
        m_m.resize(n);
        m_v.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_m[i] = static_cast<T>(0);
            m_v[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        const T t = static_cast<T>(m_t);
        const T mu_t = m_cfg.beta1 * (static_cast<T>(1) -
                                      static_cast<T>(0.5) * std::pow(static_cast<T>(0.96), t * m_cfg.momentum_decay));
        const T mu_next =
            m_cfg.beta1 *
            (static_cast<T>(1) -
             static_cast<T>(0.5) * std::pow(static_cast<T>(0.96), (t + static_cast<T>(1)) * m_cfg.momentum_decay));
        const T mu_prod_t = m_mu_prod * mu_t; // ∏_{i≤t} μ_i
        const T mu_prod_next = mu_prod_t * mu_next;
        m_b2t *= m_cfg.beta2;
        const T bc2 = static_cast<T>(1) - m_b2t;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i] + m_cfg.weight_decay * x[i];
            m_m[i] = m_cfg.beta1 * m_m[i] + (static_cast<T>(1) - m_cfg.beta1) * g;
            m_v[i] = m_cfg.beta2 * m_v[i] + (static_cast<T>(1) - m_cfg.beta2) * g * g;
            const T mhat = mu_next * m_m[i] / (static_cast<T>(1) - mu_prod_next) +
                           (static_cast<T>(1) - mu_t) * g / (static_cast<T>(1) - mu_prod_t);
            const T vhat = m_v[i] / bc2;
            x[i] -= m_cfg.lr * mhat / (std::sqrt(vhat) + m_cfg.eps);
        }
        m_mu_prod = mu_prod_t;
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    NadamConfig<T> m_cfg;
    crd::containers::Array<T> m_m;
    crd::containers::Array<T> m_v;
    crd::usize m_t = 0;
    T m_mu_prod = static_cast<T>(1);
    T m_b2t = static_cast<T>(1);
};

// ---------------------------------------------------------------- RAdam (Liu et al. 2020 rectification)

template <typename T> struct RadamConfig
{
    T lr = static_cast<T>(1e-3);
    T beta1 = static_cast<T>(0.9);
    T beta2 = static_cast<T>(0.999);
    T eps = static_cast<T>(1e-8);
    T weight_decay = static_cast<T>(0); // coupled L2
};

template <typename T> class RadamOptimizer
{
public:
    RadamOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const RadamConfig<T>& cfg)
        : m_cfg(cfg), m_m(alloc), m_v(alloc)
    {
        m_m.resize(n);
        m_v.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_m[i] = static_cast<T>(0);
            m_v[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        m_b1t *= m_cfg.beta1;
        m_b2t *= m_cfg.beta2;
        const T bc1 = static_cast<T>(1) - m_b1t;
        const T bc2 = static_cast<T>(1) - m_b2t;
        const T t = static_cast<T>(m_t);
        const T rho_inf = static_cast<T>(2) / (static_cast<T>(1) - m_cfg.beta2) - static_cast<T>(1);
        const T rho_t = rho_inf - static_cast<T>(2) * t * m_b2t / bc2;
        const bool rectified = rho_t > static_cast<T>(5);
        T r_t = static_cast<T>(1);
        if (rectified)
        {
            r_t = std::sqrt(((rho_t - static_cast<T>(4)) * (rho_t - static_cast<T>(2)) * rho_inf) /
                            ((rho_inf - static_cast<T>(4)) * (rho_inf - static_cast<T>(2)) * rho_t));
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i] + m_cfg.weight_decay * x[i];
            m_m[i] = m_cfg.beta1 * m_m[i] + (static_cast<T>(1) - m_cfg.beta1) * g;
            m_v[i] = m_cfg.beta2 * m_v[i] + (static_cast<T>(1) - m_cfg.beta2) * g * g;
            const T mhat = m_m[i] / bc1;
            if (rectified)
            {
                const T vhat = std::sqrt(m_v[i] / bc2);
                x[i] -= m_cfg.lr * r_t * mhat / (vhat + m_cfg.eps);
            }
            else
            {
                x[i] -= m_cfg.lr * mhat; // the small-t un-rectified (no adaptive denominator) branch
            }
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    RadamConfig<T> m_cfg;
    crd::containers::Array<T> m_m;
    crd::containers::Array<T> m_v;
    crd::usize m_t = 0;
    T m_b1t = static_cast<T>(1);
    T m_b2t = static_cast<T>(1);
};

// ---------------------------------------------------------------- RMSprop (+momentum)

template <typename T> struct RmspropConfig
{
    T lr = static_cast<T>(1e-2);
    T alpha = static_cast<T>(0.99); // smoothing
    T eps = static_cast<T>(1e-8);
    T momentum = static_cast<T>(0);
    T weight_decay = static_cast<T>(0); // coupled L2
};

template <typename T> class RmspropOptimizer
{
public:
    RmspropOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const RmspropConfig<T>& cfg)
        : m_cfg(cfg), m_sq(alloc), m_buf(alloc)
    {
        m_sq.resize(n);
        m_buf.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_sq[i] = static_cast<T>(0);
            m_buf[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i] + m_cfg.weight_decay * x[i];
            m_sq[i] = m_cfg.alpha * m_sq[i] + (static_cast<T>(1) - m_cfg.alpha) * g * g;
            const T denom = std::sqrt(m_sq[i]) + m_cfg.eps;
            if (m_cfg.momentum > static_cast<T>(0))
            {
                m_buf[i] = m_cfg.momentum * m_buf[i] + g / denom;
                x[i] -= m_cfg.lr * m_buf[i];
            }
            else
            {
                x[i] -= m_cfg.lr * g / denom;
            }
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    RmspropConfig<T> m_cfg;
    crd::containers::Array<T> m_sq;
    crd::containers::Array<T> m_buf;
    crd::usize m_t = 0;
};

// ---------------------------------------------------------------- Adagrad

template <typename T> struct AdagradConfig
{
    T lr = static_cast<T>(1e-2);
    T eps = static_cast<T>(1e-10);
    T weight_decay = static_cast<T>(0); // coupled L2
};

template <typename T> class AdagradOptimizer
{
public:
    AdagradOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const AdagradConfig<T>& cfg)
        : m_cfg(cfg), m_sum(alloc)
    {
        m_sum.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_sum[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i] + m_cfg.weight_decay * x[i];
            m_sum[i] += g * g;
            x[i] -= m_cfg.lr * g / (std::sqrt(m_sum[i]) + m_cfg.eps);
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    AdagradConfig<T> m_cfg;
    crd::containers::Array<T> m_sum;
    crd::usize m_t = 0;
};

// ---------------------------------------------------------------- Adadelta (Zeiler 2012)

template <typename T> struct AdadeltaConfig
{
    T lr = static_cast<T>(1); // torch default: 1.0 (the method self-scales)
    T rho = static_cast<T>(0.9);
    T eps = static_cast<T>(1e-6);
    T weight_decay = static_cast<T>(0); // coupled L2
};

template <typename T> class AdadeltaOptimizer
{
public:
    AdadeltaOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const AdadeltaConfig<T>& cfg)
        : m_cfg(cfg), m_sq(alloc), m_acc(alloc)
    {
        m_sq.resize(n);
        m_acc.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_sq[i] = static_cast<T>(0);
            m_acc[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i] + m_cfg.weight_decay * x[i];
            m_sq[i] = m_cfg.rho * m_sq[i] + (static_cast<T>(1) - m_cfg.rho) * g * g;
            const T delta = std::sqrt(m_acc[i] + m_cfg.eps) / std::sqrt(m_sq[i] + m_cfg.eps) * g;
            m_acc[i] = m_cfg.rho * m_acc[i] + (static_cast<T>(1) - m_cfg.rho) * delta * delta;
            x[i] -= m_cfg.lr * delta;
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    AdadeltaConfig<T> m_cfg;
    crd::containers::Array<T> m_sq;
    crd::containers::Array<T> m_acc;
    crd::usize m_t = 0;
};

// ---------------------------------------------------------------- Lion (Chen et al. 2023)

template <typename T> struct LionConfig
{
    T lr = static_cast<T>(1e-4);
    T beta1 = static_cast<T>(0.9);
    T beta2 = static_cast<T>(0.99);
    T weight_decay = static_cast<T>(0); // DECOUPLED (the paper's formulation): x −= lr·(sign(u) + wd·x)
};

template <typename T> class LionOptimizer
{
public:
    LionOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const LionConfig<T>& cfg) : m_cfg(cfg), m_m(alloc)
    {
        m_m.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_m[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i];
            const T u = m_cfg.beta1 * m_m[i] + (static_cast<T>(1) - m_cfg.beta1) * g;
            const T s = u > static_cast<T>(0) ? static_cast<T>(1)
                                              : (u < static_cast<T>(0) ? static_cast<T>(-1) : static_cast<T>(0));
            x[i] -= m_cfg.lr * (s + m_cfg.weight_decay * x[i]);
            m_m[i] = m_cfg.beta2 * m_m[i] + (static_cast<T>(1) - m_cfg.beta2) * g;
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    LionConfig<T> m_cfg;
    crd::containers::Array<T> m_m;
    crd::usize m_t = 0;
};

// ---------------------------------------------------------------- LAMB (You et al. 2020; single param group)

template <typename T> struct LambConfig
{
    T lr = static_cast<T>(1e-3);
    T beta1 = static_cast<T>(0.9);
    T beta2 = static_cast<T>(0.999);
    T eps = static_cast<T>(1e-6);
    T weight_decay = static_cast<T>(0); // enters the trust-ratio update r += wd·x (the paper)
};

template <typename T> class LambOptimizer
{
public:
    LambOptimizer(crd::memory::IAllocator* alloc, crd::usize n, const LambConfig<T>& cfg)
        : m_cfg(cfg), m_m(alloc), m_v(alloc), m_r(alloc)
    {
        m_m.resize(n);
        m_v.resize(n);
        m_r.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_m[i] = static_cast<T>(0);
            m_v[i] = static_cast<T>(0);
        }
    }

    void step(crd::containers::Span<T> x, crd::containers::ConstSpan<T> grad) noexcept
    {
        const crd::usize n = x.size();
        ++m_t;
        m_b1t *= m_cfg.beta1;
        m_b2t *= m_cfg.beta2;
        const T bc1 = static_cast<T>(1) - m_b1t;
        const T bc2 = static_cast<T>(1) - m_b2t;
        T x_norm_sq = static_cast<T>(0);
        T r_norm_sq = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T g = grad[i];
            m_m[i] = m_cfg.beta1 * m_m[i] + (static_cast<T>(1) - m_cfg.beta1) * g;
            m_v[i] = m_cfg.beta2 * m_v[i] + (static_cast<T>(1) - m_cfg.beta2) * g * g;
            const T mhat = m_m[i] / bc1;
            const T vhat = m_v[i] / bc2;
            m_r[i] = mhat / (std::sqrt(vhat) + m_cfg.eps) + m_cfg.weight_decay * x[i];
            x_norm_sq += x[i] * x[i];
            r_norm_sq += m_r[i] * m_r[i];
        }
        const T x_norm = std::sqrt(x_norm_sq);
        const T r_norm = std::sqrt(r_norm_sq);
        const T trust =
            (x_norm > static_cast<T>(0) && r_norm > static_cast<T>(0)) ? x_norm / r_norm : static_cast<T>(1);
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] -= m_cfg.lr * trust * m_r[i];
        }
    }

    void set_lr(T lr) noexcept { m_cfg.lr = lr; }
    [[nodiscard]] crd::usize steps() const noexcept { return m_t; }

private:
    LambConfig<T> m_cfg;
    crd::containers::Array<T> m_m;
    crd::containers::Array<T> m_v;
    crd::containers::Array<T> m_r; // the trust-ratio update direction (kept to avoid a second pass allocation)
    crd::usize m_t = 0;
    T m_b1t = static_cast<T>(1);
    T m_b2t = static_cast<T>(1);
};

} // namespace crd::hesap::opt
