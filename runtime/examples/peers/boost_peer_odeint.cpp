// The Boost.Odeint peer of the ODE board (boost_peer.hpp).

#include "boost_peer.hpp"

#include <array>
#include <boost/numeric/odeint/integrate/integrate_adaptive.hpp>
#include <boost/numeric/odeint/stepper/generation.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta_dopri5.hpp>

namespace odeint = ::boost::numeric::odeint;

namespace crd_peer::boost_odeint
{
Integration dopri5(const VanDerPol& problem, const Tolerances& tolerances)
{
    using State = std::array<double, 2>;
    std::int64_t evaluations = 0;
    const double mu = problem.mu;
    auto system = [&evaluations, mu](const State& y, State& dydt, double)
    {
        dydt[0] = y[1];
        dydt[1] = mu * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
        ++evaluations;
    };
    State y{problem.y0[0], problem.y0[1]};
    auto stepper =
        odeint::make_controlled(tolerances.absolute, tolerances.relative, odeint::runge_kutta_dopri5<State>());
    const std::size_t steps = odeint::integrate_adaptive(stepper, system, y, 0.0, problem.t1, tolerances.first_step);
    return Integration{{y[0], y[1]}, evaluations, static_cast<std::int64_t>(steps)};
}
} // namespace crd_peer::boost_odeint
