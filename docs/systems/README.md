# Systems

One short overview per shipped engine module. Plain English. "What is it,
what does it do, how do I use it." Read this folder to remember what the
engine *is*; read `docs/sessions/` to remember how it got that way.

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-core`       | ✅        | [core.md](core.md) |
| `crd-log`        | ✅        | [log.md](log.md) — deep-dive: [`docs/log/LOG_FILE.md`](../log/LOG_FILE.md) |
| `crd-vm`         | ✅        | [vm.md](vm.md) |
| `crd-memory`     | ✅        | [memory.md](memory.md) — deep-dive: [`docs/memory/MEMORY_FILE.md`](../memory/MEMORY_FILE.md) |
| `crd-containers` | ✅        | [containers.md](containers.md) — deep-dive: [`docs/containers/CONTAINERS_FILE.md`](../containers/CONTAINERS_FILE.md) |
| `crd-math`       | ✅        | [math.md](math.md) |
| `crd-platform`   | ✅        | [platform.md](platform.md) |
| `crd-app`        | ✅        | [app.md](app.md) |
| `crd-config`     | ✅ (1.6a) | [config.md](config.md) — 1.6b reload hook planned |
| `crd-imgui`      | ✅        | [imgui.md](imgui.md) — debug-only overlay, docking branch |
| `crd-rhi`        | ✅        | [rhi.md](rhi.md) — Vulkan backend; descriptor system; index buffer |
| `crd-shader`     | ✅ (2.3g) | [shader.md](shader.md) — GLSL ingest, SPIR-V, reflection, hot-reload, pipeline handoff |
| `crd-renderer`   | 🚧 (v1i) | [renderer.md](renderer.md) — frame graph, IRenderPath, material system, ForwardRenderPath, swapchain blit; per-material PSO cache + depth prepass added in Phase 2.8 |
| `crd-jobs`       | ✅ (v1k) | [jobs.md](jobs.md) — fiber job system; run/wait/parallel_for; SBO lambdas; frame allocator |
| `crd-resources`  | ✅ (v1g) | [resources.md](resources.md) — handle table, sync/async/streamed loading, 2Q LRU eviction, hot-reload, asset cooker (CRDR), texture + mesh + material loaders |
| `crd-meshgen`    | ✅        | [meshgen.md](meshgen.md) — procedural geometry primitives (cube, sphere, cylinder, cone, plane, capsule, torus, icosphere) producing standard 48B vertex layout |
| `crd-sandbox`    | ✅        | [sandbox.md](sandbox.md) — interactive desktop app: orbit camera, ForwardRenderPath, unified Asset Browser (procedural shapes + cooked glTF imports), ImGui overlay |
| `crd-scene`      | 🚧 (v1c2)| [scene.md](scene.md) — Phase 3.0 foundation: `EntityId`/`SlotMap`/`World`, `ComponentRegistry` + trait grammar, archetype storage (`Archetype` + `ArchetypeGraph` + `ArchetypeChunkStorage`), typed `add_component<T>`/`get`/`remove`, `IStorageEventSink` (the L5 plug point). 8-layer slot architecture; v1d–v1n in flight |
| `crd-hesap-dense`| ✅ (v0d) | [hesap-dense.md](hesap-dense.md) — BLAS L1 + L2 + L3 over Matrix / Symmetric / Triangular / Banded; 10/10 GEMM WINS over Eigen-MT (i9-14900K, AVX2, both f32+f64); reference-class shootout vs Eigen 3.4 + OpenBLAS 0.3.27; FMA microkernel; allocator-propagating; continuous-benchmark policy |
| `crd-hesap-ode` | 🚧 (v9-a) | [hesap-ode.md](hesap-ode.md) — ODE/DAE cluster (ADR-0091): two API layers (raw-span alloc-free stepper kernels for eylem/animation/DAW hot loops + the OdeFunction driver substrate), deterministic controllers (scipy-exact + Hairer PI), dense-output contract, work-precision counters |
| `crd-hesap-dsp` | ✅ (v11 a–t) | [hesap-dsp.md](hesap-dsp.md) — DSP cluster (ADR-0093): the honest design/application gate-split, SOS-by-default + the two-layer streaming contract. Full analysis surface (design + filtering + multirate + Hilbert + spectral/multitaper + AR + subspace + transforms + waveforms + measurements + adaptive) on a **multi-threaded FFT** (bit-identical across {1..16}). Crushes scipy/MATLAB/liquid on owned kernels (Welch 15.3× MATLAB pwelch) |
| `crd-hesap-wavelet` | ✅ (v11w a–e) | [hesap-wavelet.md](hesap-wavelet.md) — wavelets (ADR-0093): 76 families (coeffs generated from pywt) · DWT/IDWT (9 modes, bit-match pywt) · SWT/iSWT · packets + best-basis · CWT (mexh/morl/cmor/gaus/cgau/shan/fbsp/paul, MT-batched) · 2-D DWT · VisuShrink/BayesShrink/SureShrink denoising · MODWT. Beats pywt's C core (wavedec 1.2×, swt 1.18×, cwt-cmor 2.94×, dwt2 1.49×) + the {1,4,16}-thread determinism moat |
| `crd-hesap-comms` | ✅ (v11c a–g) | [hesap-comms.md](hesap-comms.md) — comms/SDR (ADR-0093): Gray PSK/QAM/PAM/FSK modulation (BER-vs-theory) · RRC/Gaussian pulse shaping · Gardner/M&M timing + Costas/PLL/AFC carrier recovery · LMS-DD/CMA/DFE/MLSE equalizers · AWGN/Rayleigh/Rician channels + AGC + framing + Hamming FEC · FFT-based OFDM. **Crushes liquid-dsp on all benched ops** (modulate 4.2×, demod 3.6×, eqlms 2.5×, OFDM 3.0×) |

When a new module ships, add a row here and link to its overview. If a module
gets a long-form deep-dive document (like `LOG_FILE.md`), link to that too —
the overview here should stay short and stable.
