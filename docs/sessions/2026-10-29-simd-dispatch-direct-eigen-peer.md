# 2026-10-29 — SIMD-DISPATCH: the sparse direct vs-reference benches on the Eigen peer

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §8](../decisions/0134-runtime-cpu-dispatch.md);
follows [the sparse part](2026-10-29-simd-dispatch-sparse-eigen-peer.md).

## CI first

Push run 40784988994 (92db578d) still runs its Windows jobs. Its three GCC LTO failures are fixed in 0df22cdc (see
the sparse note), and every Ninja build step now keeps going past a failure, so the next run reports all of them.
No completed run carries hosted evidence for DIAG.11c or DIAG.12.

## What landed

- `eigen_peer_direct.cpp` adds the sparse direct solvers to the peer. `SparseSolve` names the six configurations
  the boards measure: SimplicialLLT with natural and AMD ordering, SimplicialLDLT, SparseLU with natural and COLAMD
  ordering, and SparseQR. An `SpSolver` handle can analyze, factorize, solve one or many right-hand sides, and
  report its factor's nonzeros and L's pattern. `csc_f64` also builds from a compressed CSR matrix, so the LU and
  QR benches hand Eigen exactly the matrix Cerid factors.
- The peer's object types moved to `eigen_peer_objects.hpp`, so every unit of the library can reach them. The
  Hessenberg, Schur and general-eigen declarations moved from the sparse section of `eigen_peer.hpp` to the dense one.
- Converted: ordering, cholesky, lu, qr, ldlt and lu_supernodal. Each prints the peer's ISA line, and each now
  builds with the tree's warnings and passes tidy for the first time.
- Fixed in passing:
  - lu and qr timed an Eigen solve after a failed factorization, and Eigen asserts on that. The solve now runs only
    after a successful factorization.
  - lu_supernodal's comment said natural ordering; it uses COLAMD.
  - Nested conditionals, a two-variable declaration and a capitalised loop variable in the benches.
- **One reader of environment variables:** `crd/core/environment.hpp`. `read_environment` copies a value into
  caller storage, with no heap and no lock, and reports Unset (missing or empty), Set or TooLong. `EnvironmentValue<N>`
  and `environment_set` build on it. cpu_features' `CRD_CPU_LEVEL` reader, its test's `ScopedEnv` and the benches'
  `getenv` calls use it; MSVC deprecates `std::getenv`, and its warning had been hidden by the benches' old `/WX-`.

## Checks

On win-debug (`CRD_BUILD_HESAP_VS_REFERENCE=ON`):

- The peer and the six benches build at /W4 /WX.
- tidy-files.py is clean on every changed file.
- The `[cpu]` group passes, including the new environment case.
- The ctest guards pass.
- Smoke runs, each against Eigen 3.4.0 with AVX2 and FMA:
  - ordering: nnz(L) and the L pattern MATCH on bcsstk13, 24 and 25, which checks `factor_nnz` and `factor_pattern`;
  - qr (well1033), lu and cholesky (bcsstk13), and lu_supernodal (west2021): both residuals are small.
- No boards, following the fast-row order.

## Left

The iterative Eigen benches (cg, gmres, idrs, gcrot, nested, block_cg, block_gmres, ilu, spai), then the pocketfft and
Boost peers, the no-peer-header check and PRINCIPLES §3.

The other environment readers move to `crd/core/environment.hpp` in the next batch: jobs' `policy_getenv`,
hesap-direct's `mf_getenv`, perf's `env_present`, platform's `context.cpp` and `filesystem.cpp`, the Vulkan GLSL and
HLSL compilers, and `bench_nn` and `bench_sparse_tensor`.
