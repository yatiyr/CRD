#pragma once

// ode_types.hpp — Phase 3.1.6 v9-a: shared ODE status/options/result/work types. ADR-0091.
// T is real (f32/f64). The work counters are the WORK-PRECISION CURRENCY: every driver maintains them
// deterministically (bit-identical runs produce identical counters), and the v9-z scoreboard plots
// error-vs-nfev against CVODE/scipy/odeint — so their semantics are pinned here once.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <limits>

namespace crd::hesap::ode
{

enum class OdeStatus : crd::u8
{
    Success = 0,       // reached t_end
    MaxSteps = 1,      // hit OdeOptions::max_steps before reaching t_end
    StepTooSmall = 2,  // the controller drove h below the representable/configured floor (stiffness signal)
    NotFinite = 3,     // the state became NaN/Inf
    InvalidInput = 4,  // non-finite t0/t1/h, or zero steps over a nonzero span
    EventTerminal = 5, // a terminal event stopped the integration (v9-c; OdeResult::event_index names it)
};

// Deterministic work counters (CVODE/scipy conventions):
//   nsteps  = step ATTEMPTS (naccept + nreject)
//   nfev    = RHS evaluations            njev = Jacobian evaluations (stiff, v9-d+)
//   nlu     = matrix factorizations      nsol = triangular/back solves (stiff, v9-d+)
struct OdeWork
{
    crd::u64 nsteps = 0;
    crd::u64 naccept = 0;
    crd::u64 nreject = 0;
    crd::u64 nfev = 0;
    crd::u64 njev = 0;
    crd::u64 nlu = 0;
    crd::u64 nsol = 0;
};

template <typename T> struct OdeOptions
{
    // Local error tolerances (consumed by the adaptive drivers from v9-b on; the WRMS norm in
    // controller.hpp defines their exact meaning). scipy `solve_ivp` defaults. `atol_vec` non-empty ⇒
    // per-component atol (caller-owned storage, size n) overriding the scalar.
    T rtol = static_cast<T>(1e-3);
    T atol = static_cast<T>(1e-6);
    crd::containers::ConstSpan<T> atol_vec = {};
    // Initial step (0 ⇒ automatic, Hairer's algorithm — v9-b) and step-size ceiling.
    T h0 = static_cast<T>(0);
    T hmax = std::numeric_limits<T>::infinity();
    // Step-attempt budget. 0 ⇒ unbounded (engineering default); game/realtime callers set a per-tick cap.
    crd::u64 max_steps = 0;
};

// The final state y(t) lives in the caller's in-out span (drivers never own the state); trajectory
// recording is the v9-c `OdeSolution`'s job. The result is deliberately a flat POD.
template <typename T> struct OdeResult
{
    OdeStatus status = OdeStatus::InvalidInput;
    bool success = false;    // status == Success
    T t = static_cast<T>(0); // the time actually reached (== t_end on success; the event time on EventTerminal)
    OdeWork work;
    crd::i32 event_index = -1; // which event terminated the run (EventTerminal only; -1 otherwise). v9-c.
};

} // namespace crd::hesap::ode
