# Phase 3.1.14 — `crd-ml-inference`: neural network inference + differentiable bridge

**Status:** 📋 planned (ADR-0077 §3.1.14)
**ADR:** `docs/decisions/0077-multi-domain-expansion-vision.md`
**Slot:** after `crd-hesap-autodiff` lands (Phase 3.1.6).

## Why this exists

Modern games and applications integrate ML in many ways:
- **Denoising** for ray-traced effects (OIDN / NRD / SVGF).
- **Super-resolution** (DLSS-class, FSR-class).
- **In-game AI / NPC behavior** (small recurrent / transformer models).
- **Content generation** (text-to-mesh, text-to-texture, diffusion).
- **Animation** (motion matching ML, learned mocap retargeting).
- **Audio** (noise reduction, source separation, neural codecs).

`crd-hesap-autodiff` is the math-side autodiff layer. `crd-ml-inference` is the **application-side runtime**: ONNX model loading, GPU compute kernels for inference, integration with `crd-rhi`.

## Scope

### ONNX runtime integration

- **Model loading** — parse ONNX model file, validate operator coverage.
- **Operator implementation** — convolution, matrix-multiply, attention, normalization, activation functions; the standard transformer / CNN set.
- **Model optimization** — operator fusion, constant folding, dead code elimination.
- **Quantization** — INT8 / FP16 inference for runtime performance.

### GPU inference

- **Compute-shader kernels** via `crd-rhi` for common operators.
- **Memory layout** — tensor strides, NCHW vs NHWC, batched inference.
- **Async inference** — overlap with rendering (`crd-rhi` async compute queue).

### Differentiable programming bridge

- Wrap `crd-hesap-autodiff` (forward + reverse mode AD on BLAS/LAPACK) as the training-time math.
- Extend to GPU compute via `crd-rhi`.
- Use case: eylem v9 differentiable physics consumes this directly.

### Use cases (consumers)

- **Phase 5 RT denoising** — replace OIDN/SVGF with custom ML denoiser, integrated into the RT path.
- **Phase 6 upscaling** — DLSS-class neural super-resolution.
- **Phase 3.2 animation** — motion matching with ML latent-space retrieval.
- **Phase 3.4 audio** — noise reduction, source separation, neural codec.
- **Phase 8 robotics** — learned controllers (e.g. RL-trained policies for locomotion).
- **eylem v9 differentiable** — backprop through physics for system identification, optimization.

### Out of scope (this phase)

- **Training infrastructure** — full RL/SL training loops are a separate concern. If needed, a `crd-ml-train` future substrate. For now, models are trained externally (PyTorch / TensorFlow) and imported via ONNX.
- **Custom inference kernel authoring** — defer; rely on ONNX op coverage + a few hand-written kernels for the engine-specific use cases.
- **Distributed inference** — defer (HPC integration via Phase 6).

## Dependencies

- `crd-hesap-tensor` — N-dim tensor + broadcasting + einsum.
- `crd-hesap-autodiff` — forward + reverse mode AD.
- `crd-rhi` — GPU compute kernels.
- `crd-jobs` — async inference scheduling.
- `crd-resources` — model loading via resource pipeline (`.onnx` extension).

## Sub-modules (planned)

- `crd-ml-inference-onnx` — ONNX parser + runtime.
- `crd-ml-inference-cpu` — CPU kernels (fallback for non-GPU paths).
- `crd-ml-inference-gpu` — GPU kernels via `crd-rhi` compute.
- `crd-ml-inference-fusion` — operator fusion optimization pass.

## Reference reading

- ONNX specification (https://github.com/onnx/onnx).
- ONNX Runtime architecture (Microsoft).
- ggml / llama.cpp architecture (CPU-only inference reference, mature).
- NVIDIA TensorRT design (high-perf GPU inference).
- Chen et al. "TVM: An Automated End-to-End Optimizing Compiler for Deep Learning" (2018).
- Apple Metal Performance Shaders for ML.
- Khronos NNEF (alternative neural network exchange format).

## Open questions

- **Operator coverage** — full ONNX has ~150 operators; engine needs maybe 30 (Conv/Gemm/LayerNorm/Softmax/Attention/etc.). Defining the supported subset is half the design.
- **Quantization fidelity** — INT8 quantization can degrade quality; calibration data sets are needed. Defer per-model calibration to the application layer.
- **Custom op extension API** — ONNX supports custom ops; whether to expose this to Cerid users (for engine-specific kernels) is an API surface decision.
- **Training in-engine** — explicitly out of scope for v0, but the differentiable bridge could be extended to support gradient-tape training in a future phase.

## Revisit triggers

This stub becomes a full phase plan when:
- The research dossier (`docs/research/cerid-ml-inference.md`) ships.
- `crd-hesap-autodiff` lands.
- A specific consumer (RT denoising, super-resolution, animation ML) needs ML inference as a substrate.
