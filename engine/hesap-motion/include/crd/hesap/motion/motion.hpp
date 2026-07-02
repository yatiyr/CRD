#pragma once

// crd-hesap-motion umbrella — Phase 3.1.6 v13-n/o/p/q (ADR-0095). Trajectory generation:
//   squad.hpp      — ★SQUAD C² attitude interpolation on the quaternion manifold (v13-n)
//   clothoid.hpp   — clothoid / Euler spiral, curvature linear in arc length, via Fresnel (v13-o)
//   nurbs.hpp      — NURBS rational B-splines (exact conics) (v13-o)
//   poly_traj.hpp  — minimum-jerk quintic + minimum-snap septic boundary-value trajectories (v13-p)
//   profile.hpp    — jerk-limited S-curve + trapezoidal profiles + multi-DoF time-synchronized OTG (v13-q)
//   otg.hpp        — ★★arbitrary-state single-DoF time-optimal jerk-limited OTG (faithful Ruckig port) (v13-q)
//   tcb.hpp        — Kochanek-Bartels (TCB) keyframe spline (v13-q)

#include <crd/hesap/motion/clothoid.hpp>
#include <crd/hesap/motion/nurbs.hpp>
#include <crd/hesap/motion/otg.hpp>
#include <crd/hesap/motion/otg_sync.hpp>
#include <crd/hesap/motion/poly_traj.hpp>
#include <crd/hesap/motion/profile.hpp>
#include <crd/hesap/motion/squad.hpp>
#include <crd/hesap/motion/tcb.hpp>
