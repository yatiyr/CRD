# Phase 3.1.11 — `crd-estimation` + `crd-control`: aerospace + robotics substrates

**Status:** 📋 planned (ADR-0077 §3.1.11)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after Phase 3.1.6 (`crd-hesap`) close.

## Why two substrates, paired

**Estimation** reads sensor data and produces a posterior state estimate (where is the robot? what's its velocity? what's the state of the environment?). **Control** consumes state and produces actuator commands (motor torques, thruster firings, joint targets).

They're a natural pair (closed-loop control needs estimation; estimation can be conditioned on commanded control) but they're separate libraries with separate consumers:

- Estimation feeds `crd-eylem` v9 differentiable physics (system identification), SLAM applications, sensor fusion in autonomy stacks.
- Control feeds `crd-eylem`'s joints/articulations (commanded torques), robotics waypoint following, aerospace guidance.

Shipping them together because the consumer (autonomy / robotics / aerospace) almost always needs both, and they share linear-algebra paths.

## `crd-estimation` scope

### Kalman family

- **Linear Kalman filter** — the foundation; closed-form for linear-Gaussian systems.
- **Extended Kalman Filter (EKF)** — linearize about current estimate; the workhorse for mildly nonlinear systems.
- **Unscented Kalman Filter (UKF)** — sigma-point sampling; better for severely nonlinear systems.
- **Information filter** — dual form of Kalman; numerically better for high-dimensional sparse updates.
- **Square-root forms** — for numerical conditioning on long-running systems.

### Particle filter / Monte Carlo

- **Sequential importance resampling** (the basic particle filter).
- **Rao-Blackwellized particle filter** — combine particle filter (nonlinear states) with Kalman filter (linear conditionally-Gaussian states).
- **Auxiliary particle filter** — better importance sampling.

### Factor graphs + SLAM substrate

- **Factor graphs** (g²o / GTSAM pattern) — nodes are state variables, edges are measurement factors; nonlinear least-squares optimization (consumes `crd-hesap-opt`).
- **SLAM frontend** — feature detection + data association (vision / LiDAR).
- **SLAM backend** — pose-graph optimization, loop closure, marginalization.
- **Bundle adjustment** — for visual SLAM (consumes `crd-hesap-opt` Levenberg-Marquardt).

### Sensor noise models

- **IMU** — gyro bias drift, accelerometer noise, scale factor errors, allan variance.
- **GPS** — multi-path, ionospheric delay, satellite geometry (DOP).
- **LiDAR** — beam divergence, intensity, multi-return.
- **RGB-D** — depth noise vs distance, lens distortion, rolling shutter.
- **Magnetometer** — hard-iron + soft-iron calibration.

## `crd-control` scope

### Classical control

- **PID** library — gain scheduling, anti-windup, derivative filtering, output limits.
- **Lead-lag compensators**, state-space, transfer functions.

### Optimal control

- **LQR** (Linear Quadratic Regulator) — finite + infinite horizon; discrete + continuous time.
- **LQG** (LQR + Kalman filter) — separation principle estimation+control.
- **DARE / CARE** — discrete and continuous algebraic Riccati equation solvers (consumes `crd-hesap-eig`).

### Model Predictive Control (MPC)

- **Linear MPC** — receding-horizon QP (consumes `crd-hesap-opt` OSQP-class).
- **Nonlinear MPC (NMPC)** — sequential QP / interior point (consumes `crd-hesap-opt` IPOPT-class).
- **Real-time iteration** scheme (Diehl-class) — single Newton step per timestep, suitable for 100Hz+ control loops.

### Trajectory optimization

- **Direct collocation** — discretize the trajectory, solve as one big NLP.
- **Multiple shooting** — segments + continuity constraints.
- **Differential Dynamic Programming (DDP)** / **iLQR** — Bellman recursion with line-search Newton; the modern robotics-control workhorse.
- **CHOMP / TrajOpt** — robotics trajectory optimization with collision constraints (consumes `crd-geometry-spatial` for SDF).

### Robust control

- **H∞** (H-infinity) controller synthesis.
- **µ-synthesis** — structured singular value.
- **Sliding mode** control.

### Path planning

- **A*** / Dijkstra over discrete graphs.
- **RRT / RRT*** — sampling-based; the robotics-planning workhorse.
- **PRM** (Probabilistic Roadmap) — for multi-query settings.
- **CHOMP / TrajOpt** — trajectory-optimization-style planning (overlap with above).
- **Lattice-based motion primitives** — for vehicles with kinodynamic constraints.

## Dependencies

- `crd-hesap-dense` (Kalman filter linear algebra)
- `crd-hesap-sparse` (factor graphs, large-scale SLAM)
- `crd-hesap-iterative` (factor-graph optimization)
- `crd-hesap-direct` (sparse direct factorization for NMPC)
- `crd-hesap-opt` (QP / NLP for MPC and trajectory optimization)
- `crd-hesap-stats` (random sampling for particle filter)
- `crd-eylem` (plant model for MPC)
- `crd-geometry-spatial` (path planning workspace queries)
- `crd-geometry-bvh` (collision queries for trajectory optimization)

## Reference reading

### Estimation
- Anderson & Moore "Optimal Filtering" (1979) — classical Kalman reference.
- Thrun, Burgard & Fox "Probabilistic Robotics" (2005) — modern robotics state estimation.
- Bar-Shalom, Li & Kirubarajan "Estimation with Applications to Tracking and Navigation" (2001).
- Dellaert & Kaess "Factor Graphs for Robot Perception" (2017) — g²o / GTSAM theory.
- Cadena et al. "Past, Present, and Future of SLAM" (2016) — modern survey.

### Control
- Stengel "Optimal Control and Estimation" (1994) — comprehensive.
- Bertsekas "Dynamic Programming and Optimal Control" (2017) — DDP / DP foundation.
- Rawlings, Mayne & Diehl "Model Predictive Control: Theory, Computation, and Design" (2017) — MPC reference.
- Zhou, Doyle & Glover "Robust and Optimal Control" (1996) — H∞ reference.

### Path planning
- LaValle "Planning Algorithms" (2006) — comprehensive reference.
- Karaman & Frazzoli "Sampling-based Algorithms for Optimal Motion Planning" (2011) — RRT* paper.
- Ratliff et al. "CHOMP: Gradient Optimization Techniques for Efficient Motion Planning" (2009).

## Out of scope

- Reinforcement learning policy training (Phase 3.1.14 `crd-ml-inference` provides the inference path; training is a separate concern, possibly a `crd-rl` future substrate).
- Real-time ROS2 bridge (Phase 8 robotics integration).
- URDF/SDF/MJCF importers (Phase 3.1 eylem v6 reserved, ADR-0072).
- Reinforcement learning training loop infrastructure — defer.

## Open questions

- **Determinism for replay** — control loops are often replayed in regression tests. Bit-exact replay across SIMD widths is hard for floating-point control loops; per-config replay (same SIMD, same OS) should be achievable.
- **Real-time guarantees** — control loops at 1kHz+ need hard real-time. Cerid is soft-real-time; whether `crd-control` ships a real-time mode (no allocations, pre-allocated working memory, lock-free) is an architectural question for the research dossier.
- **GPU acceleration** — MPC is computationally heavy; GPU-accelerated QP solvers exist (OSQP-CUDA). Likely defer to v8+ after CPU substrate stabilizes.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-estimation-control.md`) ships.
- `crd-hesap` close.
- A specific consumer (robotics company, autonomy stack, aerospace project) makes this an active priority.
