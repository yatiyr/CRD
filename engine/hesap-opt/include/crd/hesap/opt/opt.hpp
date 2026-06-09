#pragma once

// crd-hesap-opt umbrella — Phase 3.1.6 v7. OPTIMISATION (the full domain), matrix-free over Objective<T>.
// The differentiator: the optimization TRAJECTORY is bit-identical across {1..16} workers (a serial optimizer
// over a bit-exact objective eval) — no mainstream optimizer carries it. ADR-0090.
//
// v7-a substrate (this set). Methods land per slice: b derivatives · c line searches · d L-BFGS · e nonlinear-LS
// · f first-order · g Newton · h trust-region · i stochastic · j-o constrained (KKT/QP/LP/conic/NLP/modeling)
// · p-r derivative-free/global/MIP (slip) · z CLOSE (CLI + gold-standard bench + system doc + ADR).

#include <crd/hesap/opt/conjugate_gradient.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/dual.hpp>
#include <crd/hesap/opt/finite_difference.hpp>
#include <crd/hesap/opt/forward_ad.hpp>
#include <crd/hesap/opt/gradient_check.hpp>
#include <crd/hesap/opt/gradient_descent.hpp>
#include <crd/hesap/opt/lbfgs.hpp>
#include <crd/hesap/opt/lbfgsb.hpp>
#include <crd/hesap/opt/levenberg_marquardt.hpp>
#include <crd/hesap/opt/line_search.hpp>
#include <crd/hesap/opt/more_thuente_line_search.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/opt/quadratic_objective.hpp>
#include <crd/hesap/opt/quasi_newton.hpp>
#include <crd/hesap/opt/residual_function.hpp>
#include <crd/hesap/opt/wolfe_line_search.hpp>
