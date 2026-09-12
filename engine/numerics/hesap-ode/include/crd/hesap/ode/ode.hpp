#pragma once

// crd-hesap-ode umbrella — Phase 3.1.6 v9 (ODE/DAE cluster). ADR-0091. TWO API LAYERS by design (memory
// `project_ode_in_games_layering`): the raw-span allocation-free stepper KERNELS (steppers.hpp — what
// eylem/animation/DAW inline into hot loops) and the general DRIVER substrate (OdeFunction + options/result
// + controllers + dense output + integrate) for scripts, engineering, cinematic, and the CLI. The cluster
// grows here: v9-b embedded explicit RK → v9-d BDF → v9-e Radau → v9-f Rosenbrock/SDIRK → v9-g symplectic
// → v9-h mass/DAE → v9-i IMEX → v9-j sparse/Krylov Newton → v9-k sensitivities.

#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/controller.hpp>
#include <crd/hesap/ode/dae.hpp>
#include <crd/hesap/ode/dae_structural.hpp>
#include <crd/hesap/ode/dense_output.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/imex.hpp>
#include <crd/hesap/ode/ode_krylov_solver.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/hesap/ode/ode_sparse_solver.hpp>
#include <crd/hesap/ode/radau.hpp>
#include <crd/hesap/ode/rosenbrock.hpp>
#include <crd/hesap/ode/sdirk.hpp>
#include <crd/hesap/ode/sensitivity.hpp>
#include <crd/hesap/ode/events.hpp>
#include <crd/hesap/ode/integrate.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/hesap/ode/steppers.hpp>
#include <crd/hesap/ode/symplectic.hpp>
