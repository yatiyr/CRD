#pragma once

// crd-hesap-opt umbrella — Phase 3.1.6 v7. OPTIMISATION (the full domain), matrix-free over Objective<T>.
// The differentiator: the optimization TRAJECTORY is bit-identical across {1..16} workers (a serial optimizer
// over a bit-exact objective eval) — no mainstream optimizer carries it. ADR-0090.
//
// v7-a substrate (this set). Methods land per slice: b derivatives · c line searches · d L-BFGS · e nonlinear-LS
// · f first-order · g Newton · h trust-region · i stochastic · j-o constrained (KKT/QP/LP/conic/NLP/modeling)
// · p-r derivative-free/global/MIP (slip) · z CLOSE (CLI + gold-standard bench + system doc + ADR).

#include <crd/hesap/opt/bobyqa.hpp>
#include <crd/hesap/opt/cmaes.hpp>
#include <crd/hesap/opt/cobyla.hpp>
#include <crd/hesap/opt/conic.hpp>
#include <crd/hesap/opt/conjugate_gradient.hpp>
#include <crd/hesap/opt/constraints.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/dual.hpp>
#include <crd/hesap/opt/finite_difference.hpp>
#include <crd/hesap/opt/forward_ad.hpp>
#include <crd/hesap/opt/global_search.hpp>
#include <crd/hesap/opt/gradient_check.hpp>
#include <crd/hesap/opt/gradient_descent.hpp>
#include <crd/hesap/opt/kkt.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/lbfgsb.hpp>
#include <crd/hesap/opt/levenberg_marquardt.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/lp.hpp>
#include <crd/hesap/opt/merit.hpp>
#include <crd/hesap/opt/minibatch.hpp>
#include <crd/hesap/opt/mip.hpp>
#include <crd/hesap/opt/model.hpp>
#include <crd/hesap/opt/momentum.hpp>
#include <crd/hesap/opt/more_thuente_line_search.hpp>
#include <crd/hesap/opt/nelder_mead.hpp>
#include <crd/hesap/opt/newton.hpp>
#include <crd/hesap/opt/newton_cg.hpp>
#include <crd/hesap/opt/newuoa.hpp>
#include <crd/hesap/opt/nlp_auglag.hpp>
#include <crd/hesap/opt/nlp_interior_point.hpp>
#include <crd/hesap/opt/nlp_sqp.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/pattern_search.hpp>
#include <crd/hesap/opt/powell.hpp>
#include <crd/hesap/opt/qp.hpp>
#include <crd/hesap/opt/qp_active_set.hpp>
#include <crd/hesap/opt/quadratic_objective.hpp>
#include <crd/hesap/opt/quasi_newton.hpp>
#include <crd/hesap/opt/residual_function.hpp>
#include <crd/hesap/opt/sqp_equality.hpp>
#include <crd/hesap/opt/stochastic.hpp>
#include <crd/hesap/opt/trust_region.hpp>
#include <crd/hesap/opt/wolfe_line_search.hpp>
