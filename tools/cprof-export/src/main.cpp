// crd-cprof-export -- DIAG.6c(e): the offline CPROF -> Perfetto trace-events JSON export CLI.
//
//   cprof_export <in.cprof> <out.json>
//
// Loads a CPROF capture, validates it, and writes a Chrome/Perfetto trace-events JSON document that
// ui.perfetto.dev loads offline (no service account, no dependency). A thin wrapper over the same
// crd::perf::export_perfetto_json_to_file path the (d) acceptance test verifies byte-for-byte. Links crd-perf only
// -- the native viewer (crd-perf-ui) stays optional and is never pulled in.
#include <crd/perf/config.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/capture_export.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: cprof_export <in.cprof> <out.json>\n");
        return 2;
    }

    // Profiling compiled out -> the loader/exporter are inert stubs. Refuse LOUDLY with a non-zero exit rather than
    // emit an empty/zero-byte JSON (the tools-side of "record unavailable, never treat as zero").
    if (!crd::perf::kEnabled)
    {
        std::fprintf(stderr,
                     "cprof_export: this build has profiling compiled out (CRD_ENABLE_PROFILING=0); no capture can "
                     "be exported. Use a win-debug or win-shipping-profile build.\n");
        return 3;
    }

    const char* in  = argv[1];
    const char* out = argv[2];

    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "cprof-export"};

    auto buf = crd::perf::load_capture_from_file(in, &alloc);
    if (buf.size() == 0U)
    {
        std::fprintf(stderr, "cprof_export: cannot read %s\n", in);
        return 1;
    }

    const crd::containers::ConstSpan<crd::u8> span{buf.data(), buf.size()};
    if (!crd::perf::validate_capture_buffer(span))
    {
        std::fprintf(stderr, "cprof_export: %s is not a valid CPROF v1 capture (bad magic/version/size)\n", in);
        return 1;
    }

    crd::perf::CaptureView view{span};
    if (!view.is_valid())
    {
        std::fprintf(stderr, "cprof_export: %s failed to parse\n", in);
        return 1;
    }

    crd::perf::PerfettoExportStats stats;
    if (!crd::perf::export_perfetto_json_to_file(view, out, stats, &alloc))
    {
        std::fprintf(stderr, "cprof_export: failed to write %s\n", out);
        return 1;
    }

    std::fprintf(stdout,
                 "cprof_export: wrote %s -- %llu sample, %llu counter, %llu frame, %llu metadata, %llu instant "
                 "events; %llu samples dropped (loss); schema cerid-diagnostics/1\n",
                 out,
                 static_cast<unsigned long long>(stats.sample_events),
                 static_cast<unsigned long long>(stats.counter_events),
                 static_cast<unsigned long long>(stats.frame_events),
                 static_cast<unsigned long long>(stats.metadata_events),
                 static_cast<unsigned long long>(stats.instant_events),
                 static_cast<unsigned long long>(stats.dropped_samples));
    return 0;
}
