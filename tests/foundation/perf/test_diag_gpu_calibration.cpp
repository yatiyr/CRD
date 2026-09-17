// DIAG.6b(d): CPU<->GPU clock calibration + explicit uncertainty. A backend that can sample both clocks at one instant
// installs a per-device calibration; crd-perf then converts GPU ticks to the CPU timeline itself (emit_gpu_span_on) and
// stamps each sample's CorrelationRecord with a REAL uncertainty bound (kCorrelationCalibrated set, bound > 0). A device
// with no calibration still records on its own track in the raw GPU-tick domain -- flagged uncalibrated, sentinel
// uncertainty (never a misleading 0), counted. Oracles use a fake backend with a known truth relation T(ticks): the
// honesty check |begin_ns - T(ticks)| <= uncertainty is non-vacuous, and the drift bound must widen to cover a real
// inter-calibration drift. Proof lanes: win-debug + win-asan.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/gpu_timestamp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
// The fake backend's ground truth: a fixed base plus a linear tick rate, optionally with an extra drift term applied
// between two instants (used only by the drift case). period is exact (2.0) so tick->ns conversion adds no noise.
constexpr crd::i64 kBase   = 1'000'000;
constexpr crd::f64 kPeriod = 2.0;

[[nodiscard]] crd::i64 truth_no_drift(crd::u64 ticks) noexcept
{
    return kBase + crd::time::gpu_round_ns(static_cast<crd::f64>(ticks) * kPeriod);
}

// Dense capture-side index of the track named `name`, or 0xFFFFFFFF.
[[nodiscard]] crd::u32 find_track(const crd::perf::CaptureView& v, const char* name) noexcept
{
    for (crd::u32 i = 0U; i < v.thread_count(); ++i)
    {
        const char* const n = v.thread_name(i);
        if (n != nullptr && std::strcmp(n, name) == 0)
        {
            return i;
        }
    }
    return 0xFFFF'FFFFU;
}

[[nodiscard]] crd::u64 abs_i64(crd::i64 v) noexcept
{
    return v >= 0 ? static_cast<crd::u64>(v) : static_cast<crd::u64>(-v);
}
} // namespace

TEST_CASE("gpu-calib: a calibrated span lands on the CPU timeline with a real, honest uncertainty bound",
          "[perf][diag][gpu-calib]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-calib-rt"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("shadow");

    // One calibration for device 0: sampled at tick t0, with CPU jitter of +30 ns (its own reported deviation bound).
    constexpr crd::u64 t0     = 500U;
    constexpr crd::i64 jitter = 30;
    crd::time::GpuClockCalibration c{};
    c.cpu_ns           = truth_no_drift(t0) + jitter;
    c.gpu_ticks        = t0;
    c.ns_per_tick      = kPeriod;
    c.max_deviation_ns = static_cast<crd::u64>(jitter);
    crd::perf::set_gpu_clock_calibration(0U, c);

    constexpr crd::u64 t1 = 1000U;
    constexpr crd::u64 t2 = 1100U;
    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass, t1, t2,
                                kPeriod);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto samples = view.thread_samples(idx);
    REQUIRE(samples.size() == 1U);
    CHECK(samples[0].category == static_cast<crd::u8>(crd::perf::Category::Gpu));

    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) != 0U);       // TEETH: skip the flag -> flips
    CHECK(rec->clock_domain == crd::perf::kClockDomainCpu);
    CHECK(rec->clock_uncertainty_ns != crd::perf::kUnknownClockUncertainty);
    CHECK(rec->clock_uncertainty_ns > 0U);                              // TEETH: force unc=0 -> flips
    CHECK(rec->clock_uncertainty_ns >= static_cast<crd::u64>(jitter));  // >= the reported deviation
    // Honesty: the placed CPU time is within the stated uncertainty of the fake backend's ground truth.
    // TEETH: drop the offset (convert with round(ticks*period) alone) -> off by kBase -> this fails.
    CHECK(abs_i64(samples[0].begin_ns - truth_no_drift(t1)) <= rec->clock_uncertainty_ns);
    CHECK(abs_i64(samples[0].end_ns - truth_no_drift(t2)) <= rec->clock_uncertainty_ns);

    crd::perf::shutdown();
}

TEST_CASE("gpu-calib: a span between two calibrations widens the bound by the measured drift", "[perf][diag][gpu-calib]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-calib-drift"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("drifting");

    // Two calibration instants; the CPU/GPU relation drifts by D ns between them (a real, measured drift, not a ppm
    // constant). The truth for a tick between them is a linear interpolation of that offset.
    constexpr crd::u64 tA = 1000U;
    constexpr crd::u64 tB = 2000U;
    constexpr crd::i64 kD = 100;
    constexpr crd::u64 devdev = 5U; // each calibration's own reported deviation

    crd::time::GpuClockCalibration a{};
    a.cpu_ns           = truth_no_drift(tA); // offset 0 at A
    a.gpu_ticks        = tA;
    a.ns_per_tick      = kPeriod;
    a.max_deviation_ns = devdev;
    crd::perf::set_gpu_clock_calibration(0U, a);

    crd::time::GpuClockCalibration b{};
    b.cpu_ns           = truth_no_drift(tB) + kD; // offset D at B
    b.gpu_ticks        = tB;
    b.ns_per_tick      = kPeriod;
    b.max_deviation_ns = devdev;
    crd::perf::set_gpu_clock_calibration(0U, b);

    // A span at ts in (tA, tB), converted via the latest calibration (B). The fake truth applies a linear-interpolated
    // offset: near A the true offset is ~0, so converting via B (offset D) is wrong by ~D -- the bound must cover it.
    constexpr crd::u64 ts  = 1100U;
    constexpr crd::u64 tse = 1150U;
    const crd::f64     frac  = (static_cast<crd::f64>(ts) - static_cast<crd::f64>(tA)) /
                          (static_cast<crd::f64>(tB) - static_cast<crd::f64>(tA));
    const crd::i64 true_begin = truth_no_drift(ts) + crd::time::gpu_round_ns(frac * static_cast<crd::f64>(kD));

    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass, ts, tse,
                                kPeriod);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto samples = view.thread_samples(idx);
    REQUIRE(samples.size() == 1U);
    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);

    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) != 0U);
    CHECK(rec->clock_uncertainty_ns >= static_cast<crd::u64>(kD)); // TEETH: ignore drift (cur.max_dev only) -> fails
    // Honesty against the drifted truth. The correct bound (max_dev_B + |drift| + max_dev_A) covers the ~D conversion
    // error; a drift-ignoring bound would not.
    CHECK(abs_i64(samples[0].begin_ns - true_begin) <= rec->clock_uncertainty_ns);

    crd::perf::shutdown();
}

TEST_CASE("gpu-calib: a third calibration shifts prev to the second, not the first (bound tracks recent drift)",
          "[perf][diag][gpu-calib]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-calib-shift"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("shift");

    // Three calibrations. The big drift D1 happens A->B; only a small drift D2 happens B->C. A span AFTER C must be
    // bounded by the RECENT drift (|D2|), i.e. prev==B. A store that only ever kept the first calibration as prev would
    // bound by |D1+D2| and this discriminates.
    constexpr crd::u64 tA = 1000U, tB = 2000U, tC = 3000U;
    constexpr crd::i64 kD1 = 1000, kD2 = 10;
    constexpr crd::u64 devdev = 5U;

    crd::time::GpuClockCalibration a{};
    a.cpu_ns = truth_no_drift(tA);            a.gpu_ticks = tA; a.ns_per_tick = kPeriod; a.max_deviation_ns = devdev;
    crd::perf::set_gpu_clock_calibration(0U, a);
    crd::time::GpuClockCalibration b{};
    b.cpu_ns = truth_no_drift(tB) + kD1;      b.gpu_ticks = tB; b.ns_per_tick = kPeriod; b.max_deviation_ns = devdev;
    crd::perf::set_gpu_clock_calibration(0U, b);
    crd::time::GpuClockCalibration c{};
    c.cpu_ns = truth_no_drift(tC) + kD1 + kD2; c.gpu_ticks = tC; c.ns_per_tick = kPeriod; c.max_deviation_ns = devdev;
    crd::perf::set_gpu_clock_calibration(0U, c);

    constexpr crd::u64 ts = 3100U, tse = 3150U;
    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass, ts, tse,
                                kPeriod);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto samples = view.thread_samples(idx);
    REQUIRE(samples.size() == 1U);
    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);
    CHECK(rec->clock_uncertainty_ns >= static_cast<crd::u64>(kD2)); // covers the recent drift
    CHECK(rec->clock_uncertainty_ns < 100U);                        // NOT the stale |D1+D2| ~= 1015 (prev must be B)
    // The span sits after C, so the true offset is the accumulated D1+D2; converting via C is exact.
    const crd::i64 true_begin = truth_no_drift(ts) + kD1 + kD2;
    CHECK(abs_i64(samples[0].begin_ns - true_begin) <= rec->clock_uncertainty_ns);

    crd::perf::shutdown();
}

TEST_CASE("gpu-calib: an uncalibrated device retains a separate GPU-domain track, sentinel uncertainty, counted",
          "[perf][diag][gpu-calib]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-calib-uncal"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("uncal");

    CHECK(crd::perf::uncalibrated_span_count() == 0U);
    // No calibration installed for device 0 -> the span stays in the raw GPU-tick domain.
    constexpr crd::u64 tb = 100U;
    constexpr crd::u64 te = 200U;
    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Transfer, pass, tb, te,
                                kPeriod);
    CHECK(crd::perf::uncalibrated_span_count() == 1U); // TEETH: don't count -> flips

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto samples = view.thread_samples(idx);
    REQUIRE(samples.size() == 1U);
    CHECK(samples[0].category == static_cast<crd::u8>(crd::perf::Category::Io)); // Transfer -> Io
    CHECK(samples[0].begin_ns == crd::time::gpu_round_ns(static_cast<crd::f64>(tb) * kPeriod)); // raw GPU domain

    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) == 0U);                     // NOT calibrated
    CHECK(rec->clock_uncertainty_ns == crd::perf::kUnknownClockUncertainty);           // TEETH: sentinel->0 -> flips
    CHECK(rec->clock_domain == crd::perf::kClockDomainGpuBase + 0U);                   // device 0's raw GPU domain

    crd::perf::shutdown();
}

TEST_CASE("gpu-calib: the ns-native emit path is honest about not calibrating (sentinel, flag clear)",
          "[perf][diag][gpu-calib]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-calib-nsnative"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("nsnative");

    crd::perf::Sample s{};
    s.begin_ns = 4242;
    s.end_ns   = 4342;
    s.name_id  = pass.value;
    crd::perf::emit_gpu_sample_on(s, crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) == 0U);
    CHECK(rec->clock_uncertainty_ns == crd::perf::kUnknownClockUncertainty); // never a misleading 0
    CHECK(rec->clock_domain == crd::perf::kClockDomainCpu);

    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
