# crd-hesap-autodiff — the differentiation layer (Phase 3.1.6 v15 forward + v16 reverse)

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Reference pointer; no live status here.** Track slices/findings in [ROADMAP](../ROADMAP.md).

[Module manifest](../../engine/numerics/hesap-autodiff/CMakeLists.txt); [Current module/source map](../systems/README.md).
Full technical requirements and dated evidence: [preserved reference](../archive/systems/hesap-autodiff.md).
Read the master row and current public source before relying on a historical example.

**CPU levels** (SIMD-DISPATCH, [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md) §5): the forward jets and their
gradient and Jacobian drivers (`jet_simd.hpp`) live in the level namespace and run at the level of the unit that
instantiates them; the canned `hesap.ad.gradient.f64` functions run in the `autodiff` kernel family
(`autodiff_kernels.hpp`), bit-exact on every level.
