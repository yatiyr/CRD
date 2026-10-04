// crd-perf -- DIAG.6c(c): CPROF -> Perfetto/Chrome trace-events JSON export.
//
// Drives a REAL capture (a nested CPU hotspot + a counter + one uncalibrated GPU span) through
// export_perfetto_json and asserts the document's structure: X/C/M/i events, the schema/version declaration, the
// uncalibrated GPU span on its own (non-zero) pid with raw-tick timestamps, and JSON escaping of a quoted name.
// The ExportStats mirror the capture. With CRD_PERFETTO_EXPORT_OUT set, it also writes the file so an external
// `python -c "json.load(...)"` gate can confirm it is well-formed, right-`ph`-set, numeric-`ts` JSON (assertions run
// regardless -- never skip-as-pass).

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/capture_export.hpp>
#include <crd/perf/sample.hpp>
#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>  // std::FILE for the .cprof identity-oracle drop
#include <cstdlib> // std::getenv
#include <cstring> // std::strstr

#if CRD_PERF_ENABLED

// The gpu-track producer entry point (same forward-decl the correlation test uses).
namespace crd::perf::detail
{
void write_external_sample(crd::u8 thread_index, const Sample& s, const CorrelationRecord* corr) noexcept;
}

namespace
{
struct Fx
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "perfetto-export-test"};
    Fx() { crd::perf::init({}); }
    ~Fx() { crd::perf::shutdown(); }
};

[[nodiscard]] bool contains(const crd::containers::String& s, const char* needle) noexcept
{
    return std::strstr(s.c_str(), needle) != nullptr;
}
} // namespace

TEST_CASE("perfetto export: real capture -> structured trace-events JSON", "[perf][diag][perfetto]")
{
    Fx fx;

    // A known nested CPU hotspot.
    {
        CRD_PERF_SCOPE("outer_scope");
        {
            CRD_PERF_SCOPE("known_hotspot");
        }
    }

    // A counter.
    const auto cid = crd::perf::register_counter_i64("draws.this_frame", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(cid, 42);

    // One UNCALIBRATED GPU span: device 7, queue 3, raw-tick domain, a pass name containing a quote.
    const crd::u8 th = crd::perf::current_thread_index();
    crd::perf::enable_thread_correlation(th);
    const crd::perf::NameId pass = crd::perf::intern_name("gpu\"pass");
    crd::perf::Sample gs{};
    gs.begin_ns     = 500000;             // raw ticks (not ns) -- honest separate track
    gs.end_ns       = 500000 + 4096;
    gs.name_id      = crd::perf::intern_name("shadow_depth").value;
    gs.color_rgba   = 0U;
    gs.begin_thread = th;
    gs.end_thread   = th;
    gs.depth        = 0U;
    gs.category     = static_cast<crd::u8>(crd::perf::Category::Gpu);
    gs.fiber_id     = 0U;
    crd::perf::CorrelationRecord cr{};
    cr.flags                = crd::perf::kCorrelationValid; // NOT calibrated
    cr.queue_id             = 3U;
    cr.device_id            = 7U;
    cr.clock_domain         = crd::perf::kClockDomainGpuBase; // 1 = raw GPU-tick domain (device 0); >0 => separate pid
    cr.clock_uncertainty_ns = crd::perf::kUnknownClockUncertainty;
    cr.pass_id              = pass.value;
    cr.resource_id          = crd::perf::kNoCorrelationName;
    crd::perf::detail::write_external_sample(th, gs, &cr);

    CRD_PERF_FRAME_MARK();

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);
    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    crd::containers::String       out{&fx.alloc};
    crd::perf::PerfettoExportStats stats;
    REQUIRE(crd::perf::export_perfetto_json(view, out, stats));
    REQUIRE(out.size() > 0U);

    // ---- Structure ----
    CHECK(contains(out, "\"traceEvents\""));
    CHECK(contains(out, "\"ph\":\"X\""));      // complete events
    CHECK(contains(out, "known_hotspot"));     // the CPU hotspot name (native == external, (d) seed)
    CHECK(contains(out, "\"ph\":\"C\""));      // counter event
    CHECK(contains(out, "draws.this_frame"));
    CHECK(contains(out, "\"displayTimeUnit\""));

    // ---- Schema/version declaration (ADR-0133 ID-1) ----
    CHECK(contains(out, "\"cprof_version\":1"));
    CHECK(contains(out, "\"schema\":\"cerid-diagnostics/1\""));
    CHECK(contains(out, "\"exporter\":\"crd-perf\""));

    // ---- Uncalibrated GPU span on its own process (1 + device 7 = 8), raw ticks, not on the CPU timeline ----
    CHECK(contains(out, "\"pid\":8"));
    CHECK(contains(out, "raw ticks (uncalibrated)"));
    CHECK(contains(out, "shadow_depth"));
    CHECK(contains(out, "\"calibrated\":false"));
    // Raw GPU ticks are emitted AS-IS (integer 500000), never rescaled into the CPU timeline's microseconds.
    CHECK(contains(out, "\"ts\":500000"));

    // ---- JSON escaping: a quote in a name arrives escaped ----
    CHECK(contains(out, "gpu\\\"pass"));

    // ---- ExportStats mirror what we produced ----
    CHECK(stats.sample_events >= 3U);   // outer + hotspot + gpu
    CHECK(stats.counter_events >= 1U);
    CHECK(stats.frame_events >= 1U);
    CHECK(stats.metadata_events >= 2U); // cpu process + at least one thread name
    CHECK(stats.dropped_samples == 0U);

    // ---- Real Perfetto-compatibility evidence (opt-in file for the external json.load gate) ----
    // MSVC /WX deprecates std::getenv (C4996); use its blessed _dupenv_s there, plain getenv elsewhere.
#if defined(_MSC_VER)
    char*  outpath = nullptr;
    size_t outlen  = 0U;
    (void)_dupenv_s(&outpath, &outlen, "CRD_PERFETTO_EXPORT_OUT");
#else
    const char* outpath = std::getenv("CRD_PERFETTO_EXPORT_OUT");
#endif
    if (outpath != nullptr)
    {
        crd::perf::PerfettoExportStats fstats;
        CHECK(crd::perf::export_perfetto_json_to_file(view, outpath, fstats, &fx.alloc));
        CHECK(fstats.sample_events == stats.sample_events);
        // Drop the EXACT capture bytes next to the json so the (e) CLI can run on the SAME capture: same `buf` ->
        // same captured_at_ns, so `cprof_export <this>.cprof` must produce a byte-identical json (the (e) identity
        // oracle). Deliberately NOT save_capture_to_file again (that would re-snapshot captured_at_ns).
        crd::containers::String cprofpath{&fx.alloc};
        cprofpath.append(outpath);
        cprofpath.append(".cprof");
        std::FILE* cf = nullptr;
#if defined(_MSC_VER)
        (void)fopen_s(&cf, cprofpath.c_str(), "wb");
#else
        cf = std::fopen(cprofpath.c_str(), "wb");
#endif
        if (cf != nullptr)
        {
            std::fwrite(buf.data(), 1U, buf.size(), cf);
            std::fclose(cf);
        }
    }
#if defined(_MSC_VER)
    std::free(outpath); // free(nullptr) is a no-op
#endif
}

namespace
{
// The Perfetto ts/dur SPEC (microseconds): whole = ns/1000, then '.', then the 3-digit ns remainder. An independent
// reimplementation of the exporter's formatter -- it is the oracle the exporter must match, byte for byte.
void append_us(crd::containers::String& s, crd::i64 ns)
{
    if (ns < 0)
    {
        ns = 0;
    }
    const crd::u64 whole = static_cast<crd::u64>(ns) / 1000ULL;
    const crd::u64 frac  = static_cast<crd::u64>(ns) % 1000ULL;
    char           buf[20];
    int            n = 0;
    crd::u64       w = whole;
    if (w == 0U)
    {
        s.push_back('0');
    }
    else
    {
        while (w > 0U)
        {
            buf[n++] = static_cast<char>('0' + (w % 10U));
            w /= 10U;
        }
        while (n-- > 0)
        {
            s.push_back(buf[n]);
        }
    }
    s.push_back('.');
    s.push_back(static_cast<char>('0' + (frac / 100ULL) % 10ULL));
    s.push_back(static_cast<char>('0' + (frac / 10ULL) % 10ULL));
    s.push_back(static_cast<char>('0' + frac % 10ULL));
}

// Find the first captured sample with the given interned name across all capture threads (the capture-side index is
// dense and NOT the live thread index, so we scan rather than assume).
[[nodiscard]] bool find_sample_by_name(const crd::perf::CaptureView& v, crd::u32 name_id, crd::perf::Sample& out)
{
    for (crd::u32 ti = 0U; ti < v.thread_count(); ++ti)
    {
        const auto samples = v.thread_samples(ti);
        for (crd::u32 o = 0U; o < samples.size(); ++o)
        {
            if (samples[o].name_id == name_id)
            {
                out = samples[o];
                return true;
            }
        }
    }
    return false;
}

// A container-free structural JSON check: balanced {}/[] outside strings, correct escape handling, ends at depth 0
// not mid-string, and no unescaped control byte inside a string. This is "parse-back" without a parser.
[[nodiscard]] bool json_structurally_valid(const crd::containers::String& s)
{
    int  depth     = 0;
    bool in_string = false;
    bool escaped   = false;
    const char* p  = s.c_str();
    for (crd::usize i = 0U; i < s.size(); ++i)
    {
        const char c = p[i];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            else if (static_cast<unsigned char>(c) < 0x20U)
            {
                return false; // unescaped control byte in a string
            }
        }
        else
        {
            if (c == '"')
            {
                in_string = true;
            }
            else if (c == '{' || c == '[')
            {
                ++depth;
            }
            else if (c == '}' || c == ']')
            {
                if (--depth < 0)
                {
                    return false;
                }
            }
        }
    }
    return depth == 0 && !in_string && !escaped;
}
} // namespace

TEST_CASE("perfetto export: native == external timings (export/import acceptance)",
          "[perf][diag][perfetto][acceptance]")
{
    Fx fx;

    {
        CRD_PERF_SCOPE("outer_scope");
        {
            CRD_PERF_SCOPE("known_hotspot");
        }
    }
    const auto cid = crd::perf::register_counter_i64("draws.this_frame", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(cid, 42);

    const crd::u8 th = crd::perf::current_thread_index();
    crd::perf::enable_thread_correlation(th);
    crd::perf::Sample gs{};
    gs.begin_ns     = 500000;
    gs.end_ns       = 500000 + 4096;
    gs.name_id      = crd::perf::intern_name("shadow_depth").value;
    gs.begin_thread = th;
    gs.end_thread   = th;
    gs.category     = static_cast<crd::u8>(crd::perf::Category::Gpu);
    crd::perf::CorrelationRecord cr{};
    cr.flags                = crd::perf::kCorrelationValid;
    cr.queue_id             = 3U;
    cr.device_id            = 7U;
    cr.clock_domain         = crd::perf::kClockDomainGpuBase;
    cr.clock_uncertainty_ns = crd::perf::kUnknownClockUncertainty;
    cr.pass_id              = crd::perf::kNoCorrelationName;
    cr.resource_id          = crd::perf::kNoCorrelationName;
    crd::perf::detail::write_external_sample(th, gs, &cr);

    CRD_PERF_FRAME_MARK();

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);
    crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    crd::containers::String       out{&fx.alloc};
    crd::perf::PerfettoExportStats stats;
    REQUIRE(crd::perf::export_perfetto_json(view, out, stats));

    // ---- The document is structurally valid JSON (parse-back without a parser). ----
    CHECK(json_structurally_valid(out));

    // ---- Native (CaptureView) == external (exported JSON) for the SAME scopes, to the nanosecond. ----
    crd::perf::Sample outer{};
    crd::perf::Sample hot{};
    REQUIRE(find_sample_by_name(view, crd::perf::intern_name("outer_scope").value, outer));
    REQUIRE(find_sample_by_name(view, crd::perf::intern_name("known_hotspot").value, hot));

    // Nesting preserved natively: outer encloses the hotspot, and the hotspot is one level deeper.
    CHECK(outer.begin_ns <= hot.begin_ns);
    CHECK(hot.end_ns <= outer.end_ns);
    CHECK(hot.depth == static_cast<crd::u8>(outer.depth + 1U));

    // Exact contiguous export fragment "ts":<us>,"dur":<us>,"name":"<name>" built from the NATIVE values via the µs
    // spec -- if the exporter rounded, rescaled, or reordered, this fails.
    const auto frag = [&](const crd::perf::Sample& s, const char* name) {
        crd::containers::String f{&fx.alloc};
        f.append("\"ts\":");
        append_us(f, s.begin_ns);
        f.append(",\"dur\":");
        append_us(f, s.end_ns - s.begin_ns);
        f.append(",\"name\":\"");
        f.append(name);
        f.push_back('"');
        return f;
    };
    const auto outer_frag = frag(outer, "outer_scope");
    const auto hot_frag   = frag(hot, "known_hotspot");
    CHECK(contains(out, outer_frag.c_str()));
    CHECK(contains(out, hot_frag.c_str()));

    // The hotspot's native depth (and category) is carried into args. A plain CRD_PERF_SCOPE is Category::User;
    // require that so the fragment below asserts the real category, not a silently-empty one.
    REQUIRE(hot.category == static_cast<crd::u8>(crd::perf::Category::User));
    {
        crd::containers::String depthfrag{&fx.alloc};
        depthfrag.append("\"name\":\"known_hotspot\",\"cat\":\"user\",\"args\":{\"depth\":");
        // depth is a small integer; append it directly.
        char db[4];
        int  dn = 0;
        crd::u32 dv = hot.depth;
        if (dv == 0U)
        {
            depthfrag.push_back('0');
        }
        else
        {
            while (dv > 0U)
            {
                db[dn++] = static_cast<char>('0' + (dv % 10U));
                dv /= 10U;
            }
            while (dn-- > 0)
            {
                depthfrag.push_back(db[dn]);
            }
        }
        CHECK(contains(out, depthfrag.c_str()));
    }

    // ---- Counter C-event equality: value 42 at the frame whose record actually holds 42. ----
    const auto frames = view.frame_records();
    bool       found_counter_frame = false;
    for (crd::u32 f = 0U; f < frames.size(); ++f)
    {
        if (cid.value < frames[f].counter_count && static_cast<crd::i64>(frames[f].values[cid.value].bits) == 42)
        {
            crd::containers::String cfrag{&fx.alloc};
            cfrag.append("\"ts\":");
            append_us(cfrag, frames[f].frame_end_ns);
            cfrag.append(",\"name\":\"draws.this_frame\",\"args\":{\"value\":42");
            CHECK(contains(out, cfrag.c_str()));
            found_counter_frame = true;
            break;
        }
    }
    CHECK(found_counter_frame);

    // ---- Raw GPU path: raw ticks emitted as-is (integer), full fragment. ----
    CHECK(contains(out, "\"ts\":500000,\"dur\":4096,\"name\":\"shadow_depth\""));
}

#endif // CRD_PERF_ENABLED
