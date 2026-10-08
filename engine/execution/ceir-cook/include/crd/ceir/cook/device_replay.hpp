#pragma once

// crd-ceir-cook -- device records: backend-specific numeric replay of compute dispatches within a declared envelope.
//
// A device program is a cooked CEIR module whose top-level block holds only index constants (`arith.const`), buffer
// declarations (`resource.declare` of a plain f32, i32 or u32 buffer) and direct compute dispatches
// (`compute.dispatch` over a constant grid, binding declared buffers, with a `kernel` symbol). A device run takes the
// initial contents of every declared buffer, runs the dispatches in block order on a device executor and reads back
// every buffer a dispatch writes (`w` or `rw` in its `access`).
//
// The device executor is the caller's (`DeviceExecutor`): crd-ceir-cook names no GPU API and links no backend. A GPU
// host lowers the block (crd-ceir-gpu `lower_region`), compiles each kernel's CKIR text for its backend and runs the
// list (`execute_lowered_host`); `reference_device_executor` is the CPU reference, which evaluates each dispatch with
// crd-kir's `eval_cpu_kernel`, the oracle every GPU backend is proven against. Executors report their adapter.
//
// A device record (a run record of executor `Device`, replay_record.hpp) holds the program as its cooked blob, the
// build, the adapter, the CKIR text of every dispatched kernel, the buffers' initial contents, the written buffers'
// contents after the run, the executor's outcome and the numeric envelope the run was declared with. The envelope is
// stated by whoever records, never inferred:
//   exact   every output bit pattern equal. Claimed only on the recording adapter and build: a replay anywhere else is
//           refused before anything runs. This is the guarantee a same-device rerun gives.
//   ulp:N   f32 outputs within N representable values of the recorded ones (+0 and -0 are 0 apart; a NaN matches only
//           a NaN), integer outputs equal. Replays on any adapter, so a GPU run can be checked against the CPU
//           reference or another backend. Bit identity across hardware is never claimed.
//
// Replay loads the record's own blob into a fresh Context (never the checkout's file), refuses an incompatible
// record before anything runs (another executor, another build unless allowed, an exact envelope on another adapter,
// a missing input, a blob or kernel text that is not the content it was recorded with) and otherwise runs the record's
// inputs on the given executor and compares: first the outcome (the executor's error and the dispatch it blamed),
// then every written buffer element by element in declaration order. The first element outside the envelope is the
// divergence, reported with both values, their distance, the bound, the buffer's declaration and the last dispatch
// that writes it, at their authored positions. A replay against another program or other kernel text (an edited
// checkout) is an explicit option, reported as such.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::cook
{
// The CPU reference executor's adapter: backend "cpu-reference", vendor and device 0, driver
// kReferenceDeviceVersion (bumped whenever the reference's numerics change), api 0.
inline constexpr crd::u64 kReferenceDeviceVersion = 1U;

// The distance of two elements that cannot be compared within any bound (one NaN and one number).
inline constexpr crd::u64 kDeviceIncomparable = ~crd::u64{0U};

// The number of representable f32 values between the bit patterns `a` and `b` (0 when equal, when both are zeros of
// either sign, and when both are NaN); kDeviceIncomparable when exactly one is NaN.
[[nodiscard]] crd::u64 f32_ulp_distance(crd::u32 a, crd::u32 b) noexcept;

// The distance of two `element` values under `envelope`: Exact compares bit patterns (0 or kDeviceIncomparable); Ulp
// measures f32 values with f32_ulp_distance and integers as the absolute difference of their values.
[[nodiscard]] crd::u64 device_distance(DeviceElement element, DeviceEnvelope envelope, crd::u32 a, crd::u32 b) noexcept;

// Whether `distance` is within `envelope` for `element`: Exact needs 0, Ulp needs at most `ulps` for f32 and 0 for
// integers.
[[nodiscard]] bool within_envelope(DeviceElement element, DeviceEnvelope envelope, crd::u64 distance) noexcept;

// One kernel the program dispatches: its symbol (without '@') and its CKIR text (crd-kir's `ckir_write` form).
struct DeviceKernelSource
{
    containers::StringView symbol;
    containers::StringView ckir;
};

// One declared buffer as an executor sees it: the declaration's result Value in the executor's Context, its element
// type, its contents (initial on entry; the executor writes a written buffer's contents after the run back into it)
// and whether a dispatch writes it.
struct DeviceBufferView
{
    const Value*               resource = nullptr;
    DeviceElement              element  = DeviceElement::F32;
    containers::Span<crd::u32> words;
    bool                       written = false;
};

// What an executor's run produced besides the buffers.
struct DeviceRunOutcome
{
    crd::u8  error    = 0U; // the executor's error code (0: none); a GPU executor reports gpu::ExecuteError
    crd::u64 fault_op = 0U; // the stable id of the dispatch the error is blamed on (0: none)
};

// An executor's adapter, as plain views (copied into a record).
struct DeviceAdapterInfo
{
    containers::StringView backend;
    containers::StringView name;
    crd::u32               vendor = 0U;
    crd::u32               device = 0U;
    crd::u64               driver = 0U;
    crd::u32               api    = 0U;
};

// Run the device program `module` (in `ctx`, stable ids assigned, its top-level block checked as above) with
// `kernels` (one per dispatched symbol) on `buffers` (one per declaration, in declaration order). Returns false when
// the executor could not run the program at all (a kernel it cannot build, no device): nothing is recorded or compared
// then, and `reason` says why. True otherwise, whatever the run's own outcome (`out`).
using DeviceRunFn = bool (*)(Context& ctx, const Module& module, containers::ConstSpan<DeviceKernelSource> kernels,
                             containers::Span<DeviceBufferView> buffers, DeviceRunOutcome& out,
                             containers::String& reason, void* user);

struct DeviceExecutor
{
    DeviceRunFn       run  = nullptr;
    void*             user = nullptr;
    DeviceAdapterInfo adapter;
};

// The CPU reference executor: each dispatch, in block order, evaluated by crd-kir's eval_cpu_kernel over its bound
// buffers (f32 elements widened to f64 and rounded back, integers exact) on its grid's X workgroups. It runs scalar
// compute kernels with a one-dimensional workgroup and grid that read no builtin but LocalInvocationIndex and
// WorkgroupIndex; it refuses (returns false) any other kernel rather than skip a statement or read a builtin as 0. A
// kernel must keep its buffer reads and writes in range (the evaluator asserts that). `scratch` backs the evaluation
// and must outlive every run.
[[nodiscard]] DeviceExecutor reference_device_executor(memory::IAllocator* scratch) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class DeviceReplayStatus : crd::u8
{
    Ok = 0,
    BadRequest,      // buffers or kernels that do not match the program, or past a record's bounds
    NotLoaded,       // the program blob did not read
    Unsupported,     // the program is not a device program (see the header comment); `reason` names the op
    WrongExecutor,   // not a device record
    OtherBuild,      // made by another build (an ulp envelope replays it anyway when allowed)
    OtherAdapter,    // an exact envelope on another adapter (`reason` names the differing fields)
    MissingInputs,   // the record lacks an input its program needs
    ContentMismatch, // the record's blob or a kernel's text is not the content it was recorded with
    DeviceFailed,    // the executor could not run the program (`reason` says why)
};

// "ok", "bad-request", "not-loaded", "unsupported", "wrong-executor", "other-build", "other-adapter",
// "missing-inputs", "content-mismatch", "device-failed".
[[nodiscard]] containers::StringView device_replay_status_name(DeviceReplayStatus s) noexcept;

// What a host must supply to record the cooked program `blob`: the kernel symbols it dispatches (without '@', each
// once, in first-dispatch order) and the number of buffers it declares. Ok, NotLoaded (the blob did not read),
// Unsupported (not a device program; `reason` names the op) or BadRequest (past a record's bounds). `registrar`
// installs the program's dialects into the fresh Context it is read in.
struct DeviceProgramShape
{
    explicit DeviceProgramShape(memory::IAllocator* a) : kernels(a) {}

    containers::Array<containers::String> kernels;
    crd::u32                              buffers = 0U;
};
[[nodiscard]] DeviceReplayStatus describe_device_program(containers::ConstSpan<crd::u8> blob, Registrar registrar,
                                                         void* user, DeviceProgramShape& out,
                                                         containers::String& reason);

struct DeviceRecordRequest
{
    containers::ConstSpan<crd::u8>                         blob; // the cooked program
    containers::StringView                                 path; // the authored path it was cooked under
    containers::ConstSpan<DeviceKernelSource>              kernels;
    containers::ConstSpan<containers::ConstSpan<crd::u32>> buffers; // initial contents, one per declaration
    DeviceEnvelope                                         envelope;
};

// Run `request` once on `executor` and make its device record in `out` (left as it was unless Ok). `registrar`
// installs the program's dialects into the fresh Context it is loaded in. `reason` (cleared) says why a request was
// refused or the executor failed. The record is made whatever the run's outcome; `fault` (when not null) gets the
// blamed dispatch's authored position.
[[nodiscard]] DeviceReplayStatus record_device_run(const DeviceRecordRequest& request, const DeviceExecutor& executor,
                                                   Registrar registrar, void* user, ReplayRecord& out,
                                                   containers::String& reason, OwnedReplaySite* fault = nullptr);

struct DeviceReplayOptions
{
    containers::ConstSpan<crd::u8>            against{};         // empty: the record's own program; else this one
    containers::ConstSpan<DeviceKernelSource> kernels_against{}; // empty: the record's kernels; else these texts
    bool                                      any_build = false; // an ulp envelope replays across builds
};

// NOLINTNEXTLINE(performance-enum-size)
enum class DeviceDivergenceKind : crd::u8
{
    None = 0,
    Outcome, // the executor's error or the dispatch it blamed differs
    Element, // a written buffer's element is outside the envelope
};

// "none", "outcome", "element".
[[nodiscard]] containers::StringView device_divergence_name(DeviceDivergenceKind k) noexcept;

struct DeviceDivergence
{
    DeviceDivergenceKind kind           = DeviceDivergenceKind::None;
    crd::u32             buffer         = 0U; // Element: the buffer's declaration index
    crd::u64             element        = 0U; // Element: the element's index in it
    DeviceElement        type           = DeviceElement::F32;
    crd::u32             recorded       = 0U; // Element: the bit patterns; Outcome: the error codes
    crd::u32             replayed       = 0U;
    crd::u64             distance       = 0U; // Element: device_distance under the record's envelope
    crd::u64             bound          = 0U; // Element: the envelope's bound for this element type
    crd::u64             recorded_fault = 0U; // Outcome: the blamed dispatches' stable ids
    crd::u64             replayed_fault = 0U;
};

struct DeviceReplay
{
    explicit DeviceReplay(memory::IAllocator* a) : reason(a), dispatch(a), declaration(a), fault(a), recorded_fault(a)
    {
    }

    containers::String reason; // a refusal's detail
    DeviceDivergence   divergence;
    OwnedReplaySite    dispatch;    // Element: the last dispatch writing the buffer, in the replayed program
    OwnedReplaySite    declaration; // Element: the buffer's resource.declare, in the replayed program
    OwnedReplaySite    fault;       // the replayed run's blamed dispatch
    OwnedReplaySite    recorded_fault;
    crd::u64           max_distance    = 0U; // the largest f32 distance over every compared element (incomparable: max)
    crd::u64           compared        = 0U; // elements compared
    crd::u64           replayed_hash   = 0U;
    bool               build_differs   = false;
    bool               adapter_differs = false;
    bool               program_differs = false; // `against` is not the recorded content
    bool               kernels_differ  = false; // a `kernels_against` text is not the recorded one
};

// Replay `record` on `executor` (see the header comment). `out.divergence.kind == None` when the run reproduced the
// record within its envelope. `registrar` installs the program's dialects.
[[nodiscard]] DeviceReplayStatus replay_device_record(const ReplayRecord& record, const DeviceExecutor& executor,
                                                      Registrar registrar, void* user,
                                                      const DeviceReplayOptions& options, DeviceReplay& out);
} // namespace crd::ceir::cook
