# crd-hesap-dsp — DSP cluster (core + adaptive)

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Reference pointer; no live status here.** Track slices/findings in [ROADMAP](../ROADMAP.md).

[Module manifest](../../engine/numerics/hesap-dsp/CMakeLists.txt); [Current module/source map](../systems/README.md).
Full technical requirements and dated evidence: [preserved reference](../archive/systems/hesap-dsp.md).
Read the master row and current public source before relying on a historical example.

FFT-bearing parallel spectral jobs explicitly request the existing 2 MiB fiber tier. GCC Debug reserves about
420 KiB for `FftPlan<double>::execute` before nested calls, exceeding the 64 KiB default. The transform and
thread-count bit-identity contracts are unchanged; [reproduction and repair](../sessions/2026-09-12-repository-cleanup.md).
