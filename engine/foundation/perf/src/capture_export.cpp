// crd-perf -- DIAG.6c(c): CPROF -> Perfetto/Chrome trace-events JSON export. See capture_export.hpp.

#include <crd/perf/capture_export.hpp>

#include <crd/perf/capture.hpp> // kCprofVersion (the version a valid view is guaranteed to hold)
#include <crd/perf/sample.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>

#include "json_writer.hpp"

#include <bit>    // std::bit_cast
#include <cstdio> // std::FILE for the _to_file twin

namespace crd::perf
{
namespace cont = crd::containers;

#if CRD_PERF_ENABLED

namespace
{
using detail::append_f64_fixed;
using detail::append_i64;
using detail::append_json_escaped;
using detail::append_u64;
using detail::append_us_from_ns;

// The 9 fixed Category names (sample.hpp Category enum order). Used as the trace-event `cat`.
[[nodiscard]] const char* category_name(crd::u8 c) noexcept
{
    switch (static_cast<Category>(c))
    {
    case Category::User:   return "user";
    case Category::Job:    return "job";
    case Category::System: return "system";
    case Category::Pass:   return "pass";
    case Category::Render: return "render";
    case Category::Gpu:    return "gpu";
    case Category::Memory: return "memory";
    case Category::Io:     return "io";
    case Category::Wait:   return "wait";
    }
    return "unknown";
}

// A reserved tid for the per-frame span row (kept clear of real thread indices).
inline constexpr crd::u32 kFramesTid = 0xFFFF'0000U;

// A '"key":' fragment.
void append_key(cont::String& out, const char* key) noexcept
{
    out.push_back('"');
    out.append(key);
    out.append("\":");
}

// A '"key":"escaped-string"' fragment (with a leading comma when `comma`).
void append_kv_str(cont::String& out, const char* key, const char* val, bool comma) noexcept
{
    if (comma)
        out.push_back(',');
    append_key(out, key);
    out.push_back('"');
    append_json_escaped(out, cont::StringView{val ? val : ""});
    out.push_back('"');
}

// A '"key":<u64>' fragment.
void append_kv_u64(cont::String& out, const char* key, crd::u64 val, bool comma) noexcept
{
    if (comma)
        out.push_back(',');
    append_key(out, key);
    append_u64(out, val);
}

// A bounded, linear-scan "seen" set for (pid,tid) and pid dedup of metadata events -- avoids allocating for the
// common (few-track) case. Up to N distinct keys are recorded and deduped exactly; BEYOND N, `add` can no longer
// record, so those keys report "new" on every sample and their M events repeat once per sample. That is harmless
// (Perfetto keeps the last name for a (pid,tid)) and bounded by the sample count, never unbounded growth.
template <crd::u32 N>
struct SeenSet
{
    crd::u64 items[N]{};
    crd::u32 count = 0U;
    [[nodiscard]] bool add(crd::u64 key) noexcept // true if not already recorded (past N: always true -> repeats)
    {
        for (crd::u32 i = 0U; i < count; ++i)
            if (items[i] == key)
                return false;
        if (count < N)
            items[count++] = key;
        return true;
    }
};

} // namespace

[[nodiscard]] bool export_perfetto_json(const CaptureView& view,
                                        cont::String& out,
                                        PerfettoExportStats& stats) noexcept
{
    stats = PerfettoExportStats{};
    if (!view.is_valid())
        return false;

    out.append("{\"traceEvents\":[");
    bool       need_comma = false;
    const auto comma      = [&]() noexcept {
        if (need_comma)
            out.push_back(',');
        need_comma = true;
    };

    // ---- Process 0 = the CPU timeline (CPU + calibrated GPU), microseconds. ----
    {
        comma();
        out.append(R"({"ph":"M","pid":0,"tid":0,"name":"process_name","args":{)");
        append_kv_str(out, "name", "cpu + calibrated gpu (us)", false);
        out.append("}}");
        ++stats.metadata_events;
    }

    // ---- CPU thread_name metadata + loss instants. ----
    const crd::u32 thread_count = view.thread_count();
    for (crd::u32 ti = 0U; ti < thread_count; ++ti)
    {
        const char*    tname   = view.thread_name(ti);
        const crd::u32 dropped = view.thread_dropped_count(ti);
        comma();
        out.append(R"({"ph":"M","pid":0,"tid":)");
        append_u64(out, ti);
        out.append(R"(,"name":"thread_name","args":{)");
        append_kv_str(out, "name", tname, false);
        append_kv_u64(out, "dropped", dropped, true);
        out.append("}}");
        ++stats.metadata_events;

        if (dropped > 0U)
        {
            // Loss is never hidden: an instant marks that this thread dropped samples to ring overflow.
            comma();
            out.append(R"({"ph":"i","s":"t","pid":0,"tid":)");
            append_u64(out, ti);
            out.append(R"(,"name":"samples_dropped","ts":)");
            append_us_from_ns(out, static_cast<crd::i64>(view.captured_at_ns()));
            out.append(",\"args\":{");
            append_kv_u64(out, "count", dropped, false);
            out.append("}}");
            ++stats.instant_events;
            stats.dropped_samples += dropped;
        }
    }

    SeenSet<64> seen_gpu_track; // packed (pid<<32|tid)
    SeenSet<32> seen_gpu_pid;   // pid

    // ---- Sample "X" complete events. ----
    for (crd::u32 ti = 0U; ti < thread_count; ++ti)
    {
        const auto samples = view.thread_samples(ti);
        for (crd::u32 o = 0U; o < samples.size(); ++o)
        {
            const Sample&            s    = samples[o];
            const CorrelationRecord* corr = view.correlation_for(ti, o);

            // Domain split: a correlation record on a raw GPU-tick domain (clock_domain != CPU) goes on its own
            // process id (1 + device); its timestamps are the RAW ticks, never rescaled into the CPU timeline.
            const bool raw_gpu = (corr != nullptr) && (corr->clock_domain != kClockDomainCpu);
            const crd::u32 pid = raw_gpu ? (1U + corr->device_id) : 0U;
            const crd::u32 tid = raw_gpu ? corr->queue_id : ti;

            if (raw_gpu)
            {
                if (seen_gpu_pid.add(pid))
                {
                    comma();
                    out.append(R"({"ph":"M","pid":)");
                    append_u64(out, pid);
                    out.append(R"(,"tid":0,"name":"process_name","args":{)");
                    out.append(R"("name":"gpu d)");
                    append_u64(out, corr->device_id);
                    out.append(" raw ticks (uncalibrated)\"");
                    out.append("}}");
                    ++stats.metadata_events;
                }
                if (seen_gpu_track.add((static_cast<crd::u64>(pid) << 32) | tid))
                {
                    comma();
                    out.append(R"({"ph":"M","pid":)");
                    append_u64(out, pid);
                    out.append(",\"tid\":");
                    append_u64(out, tid);
                    out.append(R"(,"name":"thread_name","args":{"name":"queue )");
                    append_u64(out, tid);
                    out.append("\"}}");
                    ++stats.metadata_events;
                }
            }

            comma();
            out.append(R"({"ph":"X","pid":)");
            append_u64(out, pid);
            out.append(",\"tid\":");
            append_u64(out, tid);
            out.append(",\"ts\":");
            if (raw_gpu)
                append_i64(out, s.begin_ns);
            else
                append_us_from_ns(out, s.begin_ns);
            out.append(",\"dur\":");
            const crd::i64 dur = s.end_ns - s.begin_ns;
            if (raw_gpu)
                append_i64(out, dur);
            else
                append_us_from_ns(out, dur);
            append_kv_str(out, "name", view.resolve_name(NameId{s.name_id}), true);
            append_kv_str(out, "cat", category_name(s.category), true);

            // args: always depth; fiber/migration when present; correlation identity when present.
            out.append(",\"args\":{");
            bool acomma = false;
            append_kv_u64(out, "depth", s.depth, acomma);
            acomma = true;
            if (s.fiber_id != 0U)
                append_kv_u64(out, "fiber_id", s.fiber_id, acomma);
            if (s.end_thread != s.begin_thread)
                append_kv_u64(out, "end_thread", s.end_thread, acomma);
            if (corr != nullptr)
            {
                append_kv_u64(out, "queue", corr->queue_id, acomma);
                append_kv_u64(out, "device", corr->device_id, acomma);
                append_kv_u64(out, "clock_domain", corr->clock_domain, acomma);
                const bool calibrated = (corr->flags & kCorrelationCalibrated) != 0U;
                out.push_back(',');
                append_key(out, "calibrated");
                out.append(calibrated ? "true" : "false");
                // Uncertainty is meaningful only when calibrated; otherwise it is the sentinel -- report it as such.
                if (calibrated)
                    append_kv_u64(out, "uncertainty_ns", corr->clock_uncertainty_ns, true);
                if (corr->pass_id != kNoCorrelationName)
                    append_kv_str(out, "pass", view.resolve_name(NameId{corr->pass_id}), true);
                if (corr->resource_id != kNoCorrelationName)
                    append_kv_str(out, "resource", view.resolve_name(NameId{corr->resource_id}), true);
            }
            out.append("}}");
            ++stats.sample_events;
        }
    }

    // ---- Frame spans (reserved tid) + per-frame counters. ----
    const auto     frames        = view.frame_records();
    const crd::u32 counter_count = view.counter_count();
    if (frames.size() > 0U)
    {
        comma();
        out.append(R"({"ph":"M","pid":0,"tid":)");
        append_u64(out, kFramesTid);
        out.append(R"(,"name":"thread_name","args":{"name":"frames"}})");
        ++stats.metadata_events;
    }
    for (crd::u32 f = 0U; f < frames.size(); ++f)
    {
        const FrameRecord& fr = frames[f];

        comma();
        out.append(R"({"ph":"X","pid":0,"tid":)");
        append_u64(out, kFramesTid);
        out.append(",\"ts\":");
        append_us_from_ns(out, fr.frame_begin_ns);
        out.append(",\"dur\":");
        append_us_from_ns(out, fr.frame_end_ns - fr.frame_begin_ns);
        out.append(R"(,"name":"frame )");
        append_u64(out, fr.frame_index);
        out.append(R"(","cat":"frame"})");
        ++stats.frame_events;

        const crd::u32 n = fr.counter_count < counter_count ? fr.counter_count : counter_count;
        for (crd::u32 ci = 0U; ci < n; ++ci)
        {
            const CounterInfo info = view.counter_info(ci);
            comma();
            out.append(R"({"ph":"C","pid":0,"ts":)");
            append_us_from_ns(out, fr.frame_end_ns);
            append_kv_str(out, "name", info.name, true);
            out.append(",\"args\":{");
            append_key(out, "value");
            const crd::u64 bits = fr.values[ci].bits;
            switch (info.type)
            {
            case CounterType::F64:
                append_f64_fixed(out, std::bit_cast<double>(bits));
                break;
            case CounterType::I64:
            case CounterType::DurationNs:
            default:
                append_i64(out, std::bit_cast<crd::i64>(bits));
                break;
            }
            out.append("}}");
            ++stats.counter_events;
        }
    }

    out.append(R"(],"displayTimeUnit":"ns","metadata":{)");
    // ADR-0133 ID-1: an offline export DECLARES the schema/version it wrote -- required, not decoration.
    append_kv_str(out, "exporter", "crd-perf", false);
    // A valid CaptureView passed validate_capture_buffer, which accepts only version == kCprofVersion.
    append_kv_u64(out, "cprof_version", kCprofVersion, true);
    append_kv_u64(out, "captured_at_ns", view.captured_at_ns(), true);
    append_kv_str(out, "schema", "cerid-diagnostics/1", true);
    out.append("}}");
    return true;
}

[[nodiscard]] bool export_perfetto_json_to_file(const CaptureView& view,
                                                const char* path,
                                                PerfettoExportStats& stats,
                                                crd::memory::IAllocator* alloc) noexcept
{
    if (path == nullptr || alloc == nullptr)
        return false;
    cont::String out{alloc};
    if (!export_perfetto_json(view, out, stats))
        return false;

    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, "wb") != 0)
        return false;
#else
    fp = std::fopen(path, "wb");
#endif
    if (fp == nullptr)
        return false;
    const auto written = std::fwrite(out.c_str(), 1U, out.size(), fp);
    std::fclose(fp);
    return written == out.size();
}

#else // CRD_PERF_ENABLED == 0

[[nodiscard]] bool export_perfetto_json(const CaptureView&, cont::String&, PerfettoExportStats& stats) noexcept
{
    stats = PerfettoExportStats{};
    return false;
}

[[nodiscard]] bool export_perfetto_json_to_file(const CaptureView&,
                                                const char*,
                                                PerfettoExportStats& stats,
                                                crd::memory::IAllocator*) noexcept
{
    stats = PerfettoExportStats{};
    return false;
}

#endif // CRD_PERF_ENABLED

} // namespace crd::perf
