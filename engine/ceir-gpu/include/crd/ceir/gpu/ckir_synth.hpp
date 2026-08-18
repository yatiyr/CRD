#pragma once

// crd-ceir-gpu — CEIR-22b: the CEIR→CKIR NATIVE-PROVIDER SYNTHESIS (§52/§59). A ceir.linalg / ceir.tensor op is DECLARE-only
// (typed NoSemantics, NO kernel_ref — §70 keeps the tensor graph WHOLE for a native backend); THIS is that native backend for
// the CKIR (crd-kir) substrate: it SYNTHESIZES a CKIR KGraph (or kernel plan) from the high-level op, which the device gates
// then run + validate. ⛔ SYNTHESIS ONLY — ceir-gpu names NO backend (crd-kir is header-only + GPU-free); the on-device run +
// bit-exact-vs-oracle comparison lives in tests/ceir-gpu-{vulkan,dx12} (the test_ceir_add_* precedent). The engine-side runner
// is CEIR-22c's executor (ledgered).
//
// ⛔ TWO CKIR TIERS (the survey): GRAPH tier (`g.input`/`g.contract`/`g.reduce` → `KirBackend::run` vs `eval_cpu`) owns gemm +
// reduce; KERNEL tier (`buffer_decl`+`stmt_*` → `dispatch_kernel_1wg` vs `eval_cpu_kernel`) owns fft. ⛔ BIT-EXACT ENVELOPE: the
// graph tier has NO general 2D-transpose / scalar-scale / tensor-add node, so gemm maps ONLY the plain contract (α=1, β=0, no
// transpose) and reduce ONLY {sum,prod,max,min}; ANYTHING outside → a TYPED SynthReject (never a silent wrong-result subset —
// a plain-contract synthesis of an α=2 gemm is a false-green). F32-only (the CKIR kernels are F32; the 22a element-agnostic
// declare's F32 restriction lands HERE, with a test). Dialect-qualified op-names + valid-checked attr reads (the scars).

#include <crd/ceir/context.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/kir/ckir.hpp>     // kir::KGraph / make_shape / DType / contract — the synthesized graph-tier CKIR
#include <crd/kir/ckir_fft.hpp> // kir::Fft1dPlan / build_fft1d_batched — the synthesized kernel-tier FFT

namespace crd::ceir::gpu
{
// Why a synthesis was REJECTED — the TYPED reject (the provider NEVER silently computes a subset of the op's semantics).
// ⛔ append at END (a value enum); each value is device-gate-tested (the F32 contract's siblings).
enum class SynthReject : crd::u8
{
    None = 0,
    OpNotSupported,          // the op name is not one this provider synthesizes
    ElementNotF32,           // the CKIR kernels are F32-only (the 22a element-agnostic declare's F32 contract lands here)
    OperandNotTensor,        // an operand/result is not a 3d Tensor (or its shape/element is malformed)
    ShapeNotStatic,          // a dim is Symbolic/Dynamic — a device kernel needs a concrete extent (name-forward)
    RankUnsupported,         // gemm operands are not rank-2 (batched gemm → name-forward); reduce rank > CKIR's kMaxRank (8)
    GemmEpilogueUnsupported, // gemm alpha!=1 / beta!=0 / trans_a / trans_b — the graph tier lacks scale/add/2D-transpose (name-forward)
    ReduceFnUnsupported,     // reduce `fn` is `mean` (needs a post-scale the graph tier lacks) or an unknown token — {sum,prod,max,min} map
    ReduceAxisInvalid,       // reduce `axis` < 0 or >= rank (the provider builds mask = 1 << axis)
    FftRankUnsupported,      // fft input is not rank-1 (higher-D / non-innermost-axis c2c → name-forward)
    FftLengthNotPow2,        // fft length n is not a power of two >= 2 (Bluestein → name-forward)
    FftDirectionUnknown,     // fft `direction` not in {forward, inverse}
};
[[nodiscard]] containers::StringView synth_reject_name(SynthReject r) noexcept;

// A GRAPH-tier synthesis (gemm / reduce): on success the op was authored into `g` and `output` is the CKIR result node; on a
// reject `output` is -1 and `reject` says why. The caller (a device gate / 22c executor) feeds `g.input` data by iidx order.
struct GraphSynth
{
    SynthReject reject = SynthReject::None;
    int         output = -1;
};

// Synthesize a `ceir.linalg.gemm` op into `g` as a graph-tier CKIR Contract. ⛔ BIT-EXACT ENVELOPE ONLY: alpha==1.0 &&
// beta==0.0 && !trans_a && !trans_b (else GemmEpilogueUnsupported); both operands rank-2 static-dim F32 tensors with a matching
// contraction dim. On success builds `input(A)` [M,K] as iidx 0, `input(B)` [K,N] as iidx 1, `contract(A,B)` (DetTier::Exact =
// bit-exact vs eval_cpu) → the [M,N] output node. The C operand is IGNORED (beta==0). ⛔ reads attrs via the valid-checked
// reader (the absent-reads-as-zero scar); op-name compared DIALECT-QUALIFIED ("linalg.gemm" — the work.consume scar).
[[nodiscard]] GraphSynth synth_gemm(const Context& ctx, const Operation& op, kir::KGraph& g);

// Synthesize a `ceir.tensor.reduce` op into `g` as a graph-tier CKIR reduce over `mask = 1 << axis`. ⛔ BIT-EXACT ENVELOPE:
// `fn` in {sum,prod,max,min} → KOp::Reduce{Sum,Prod,Max,Min} (DetTier::Exact); `mean` → ReduceFnUnsupported (the graph tier has
// no post-scale). F32, all-static dims, rank ≤ CKIR kMaxRank(8), axis in [0,rank). On success builds `input` iidx 0 +
// `reduce` → the output node (CKIR keepdims — the reduced axis becomes size-1; the ceir op drops it, but the REDUCED VALUES
// match, which the device gate compares). ⛔ op-name DIALECT-QUALIFIED ("tensor.reduce"); attrs via the valid-checked reader.
[[nodiscard]] GraphSynth synth_reduce(const Context& ctx, const Operation& op, kir::KGraph& g);

// A KERNEL-tier synthesis (fft): on success the FFT kernel was authored into `g` and `plan` is the CKIR `Fft1dPlan` (its
// KEntry runs via `dispatch_kernel_1wg` — NOT KirBackend::run). The 6-BUFFER CONTRACT the caller provisions (set 0): in_re=0,
// in_im=1, tw_re=2, tw_im=3, out_re=4, out_im=5 (the ceir fft's SPLIT re/im). ⛔ TWIDDLES (buffers 2/3) are the CALLER's:
// `n/2` entries, tw[k] = (cos(2πk/n), -sin(2πk/n)) for k in [0, n/2). local_size = n/2. `n` is the transform length.
struct FftSynth
{
    SynthReject    reject = SynthReject::None;
    kir::Fft1dPlan plan;
    int            n       = 0;
    bool           inverse = false;
};

// Synthesize a `ceir.tensor.fft` op into `g` as a kernel-tier CKIR FFT (build_fft1d_batched). ⛔ ENVELOPE: re_in a rank-1 [n]
// static-dim F32 tensor, n a power of two >= 2 (else FftLengthNotPow2 — Bluestein name-forward), `direction` in
// {forward,inverse}. im_in / re_out / im_out are the 22a-verified split-complex partners (find_tensor_misuse owns their
// element/shape equality). op-name DIALECT-QUALIFIED; attrs via the valid-checked reader.
[[nodiscard]] FftSynth synth_fft(const Context& ctx, const Operation& op, kir::KGraph& g);
} // namespace crd::ceir::gpu
