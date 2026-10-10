// crd-fuzz-perf-capture: validate_capture_buffer and CaptureView -- the CPROF capture reader the profiler panel, the
// cprof-export tool, capture.validate and the Perfetto export read a capture file through -- under the bounded fuzz
// harness (DIAG.10a; docs/design/test-instruments.md).
//
// The view validates once and then trusts the buffer: its accessors hand out spans and C strings pointing into it.
// The oracle is a reference layout computed from the header here, independently of the reader (the CPROF layout of
// crd/perf/capture.hpp): an accepted capture must have a consistent layout -- sections in order inside the buffer, the
// name blob's slots and strings inside the blob with the strings terminated, counts within the profiler's tables --
// and every accessor must answer from exactly where that layout says: a thread, counter or allocator name is its own
// fixed field and terminated there, a resolved name lies in the blob's strings and ends before them, sample arrays lie
// after the frame records, frame records at the end of the blob with their counts within their arrays. Counter kinds
// and types name an enumerator and the Perfetto export completes. The gate and the view must agree, the same bytes
// give the same verdict, and the same capture one byte off its 8-byte alignment is refused (the view reads through
// typed pointers). In a build without profiling the view is a stub that accepts nothing.
//
// Seeds: CPROF buffers laid out exactly as save_capture_to_buffer writes them (threads with samples, counters of every
// kind and type, allocators, an interned-name blob with an empty slot, frame records with stamped allocator names, an
// optional correlation section) and the defects DIAG.10a found in the reader, each in one buffer: a name-blob size
// that wraps the section sum, a sample array past the end or before the frame records, a blob whose slot count or
// string bytes overrun it, an offset past the strings, strings without a final terminator, unterminated thread,
// counter and allocator names, an unknown counter kind or type, header counts above the profiler's tables, a frame
// record whose counter count indexes past its values, plus truncation, a hostile correlation section and garbage.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_export.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/sample.hpp>

#include <cstddef>
#include <cstring>
#include <limits>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using namespace crd::perf;

template <typename T> T read_at(const u8* data, u64 offset) noexcept
{
    T v{};
    std::memcpy(&v, data + offset, sizeof(T));
    return v;
}

// The CPROF layout as the format defines it, computed from the header without the reader.
struct Reference
{
    const u8*   data = nullptr;
    u64         size = 0U;
    CprofHeader header{};
    u64         threads     = 0U; // section offsets
    u64         counters    = 0U;
    u64         allocators  = 0U;
    u64         blob        = 0U;
    u64         strings     = 0U; // the blob's packed strings
    u64         strings_end = 0U;
    u64         frames      = 0U; // the end of the blob
    u64         frames_end  = 0U;
    u32         name_slots  = 0U;

    // A consistent layout: every section in order inside the buffer, counts within the profiler's tables.
    [[nodiscard]] bool build(const u8* d, u64 n) noexcept
    {
        data = d;
        size = n;
        if (n < sizeof(CprofHeader))
        {
            return false;
        }
        header               = read_at<CprofHeader>(d, 0U);
        const CprofHeader& h = header;
        if (h.thread_count > kMaxThreads || h.counter_count > kMaxCounters || h.allocator_count > kMaxAllocators ||
            h.name_blob_byte_size > n || h.frame_count > n / sizeof(FrameRecord))
        {
            return false;
        }
        threads    = sizeof(CprofHeader);
        counters   = threads + static_cast<u64>(h.thread_count) * sizeof(ThreadHeader);
        allocators = counters + static_cast<u64>(h.counter_count) * sizeof(CounterMeta);
        blob       = allocators + static_cast<u64>(h.allocator_count) * sizeof(AllocatorMeta);
        frames     = blob + h.name_blob_byte_size;
        frames_end = frames + static_cast<u64>(h.frame_count) * sizeof(FrameRecord);
        if (frames_end > n || h.name_blob_byte_size < 8U)
        {
            return false;
        }
        name_slots             = read_at<u32>(d, blob);
        const u32 string_bytes = read_at<u32>(d, blob + 4U);
        strings                = blob + 8U + static_cast<u64>(name_slots) * sizeof(u32);
        strings_end            = strings + string_bytes;
        return name_slots == h.name_blob_count && strings_end <= frames &&
               (string_bytes == 0U || d[strings_end - 1U] == 0U);
    }

    [[nodiscard]] bool within(const void* p, u64 bytes, u64 from, u64 to) const noexcept
    {
        const auto* q = static_cast<const u8*>(p);
        return q >= data + from && q <= data + to && bytes <= static_cast<u64>((data + to) - q);
    }

    // A fixed name field: the view hands out the field itself, terminated inside it.
    [[nodiscard]] bool fixed_name(const char* s, u64 field, u64 field_bytes) const noexcept
    {
        return reinterpret_cast<const u8*>(s) == data + field && std::memchr(s, '\0', field_bytes) != nullptr;
    }

    // A resolved name: the empty literal of an absent name, or a string of the blob ending before its strings end.
    [[nodiscard]] bool resolved_name(const char* s) const noexcept
    {
        const auto* q = reinterpret_cast<const u8*>(s);
        if (q < data || q >= data + size)
        {
            return s[0] == '\0';
        }
        return q >= data + strings && q < data + strings_end &&
               std::memchr(q, '\0', static_cast<usize>((data + strings_end) - q)) != nullptr;
    }
};

void walk_view(const CaptureView& view, const Reference& ref)
{
    const CprofHeader& h = ref.header;
    CRD_FUZZ_REQUIRE(view.thread_count() == h.thread_count && view.counter_count() == h.counter_count &&
                     view.allocator_count() == h.allocator_count && view.frame_record_count() == h.frame_count);
    for (u32 t = 0U; t < h.thread_count; ++t)
    {
        const u64 field = ref.threads + t * sizeof(ThreadHeader) + offsetof(ThreadHeader, name);
        CRD_FUZZ_REQUIRE(ref.fixed_name(view.thread_name(t), field, sizeof(ThreadHeader::name)));
        const ConstSpan<Sample> samples = view.thread_samples(t);
        CRD_FUZZ_REQUIRE(view.thread_sample_count(t) == samples.size());
        if (!samples.empty())
        {
            CRD_FUZZ_REQUIRE(ref.within(samples.data(), samples.size() * sizeof(Sample), ref.frames_end, ref.size));
            CRD_FUZZ_REQUIRE((reinterpret_cast<std::uintptr_t>(samples.data()) & 7U) == 0U);
        }
        for (u32 o = 0U; o < samples.size(); ++o)
        {
            CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(NameId{samples[o].name_id})));
            const CorrelationRecord* c = view.correlation_for(t, o);
            if (c != nullptr)
            {
                CRD_FUZZ_REQUIRE(ref.within(c, sizeof(*c), ref.frames_end, ref.size));
                CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(NameId{c->pass_id})));
                CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(NameId{c->resource_id})));
            }
        }
    }
    for (u32 i = 0U; i < h.counter_count; ++i)
    {
        const CounterInfo info  = view.counter_info(i);
        const u64         field = ref.counters + i * sizeof(CounterMeta) + offsetof(CounterMeta, name);
        CRD_FUZZ_REQUIRE(ref.fixed_name(info.name, field, sizeof(CounterMeta::name)));
        CRD_FUZZ_REQUIRE(static_cast<u32>(info.kind) <= static_cast<u32>(CounterKind::Add));
        CRD_FUZZ_REQUIRE(static_cast<u32>(info.type) <= static_cast<u32>(CounterType::DurationNs));
    }
    for (u32 i = 0U; i < h.allocator_count; ++i)
    {
        const u64 field = ref.allocators + i * sizeof(AllocatorMeta) + offsetof(AllocatorMeta, name);
        CRD_FUZZ_REQUIRE(ref.fixed_name(view.allocator_info(i).name, field, sizeof(AllocatorMeta::name)));
    }
    const ConstSpan<FrameRecord> frames = view.frame_records();
    CRD_FUZZ_REQUIRE(frames.size() == h.frame_count);
    if (!frames.empty())
    {
        CRD_FUZZ_REQUIRE(reinterpret_cast<const u8*>(frames.data()) == ref.data + ref.frames);
        CRD_FUZZ_REQUIRE((reinterpret_cast<std::uintptr_t>(frames.data()) & 7U) == 0U);
    }
    for (u32 f = 0U; f < frames.size(); ++f)
    {
        CRD_FUZZ_REQUIRE(frames[f].counter_count <= kMaxCounters && frames[f].allocator_count <= kMaxAllocators);
        for (u32 a = 0U; a < kMaxAllocators; ++a)
        {
            const char* name  = view.frame_allocator_name(f, a);
            const u64   field = ref.allocators + a * sizeof(AllocatorMeta) + offsetof(AllocatorMeta, name);
            const bool  meta  = a < h.allocator_count && ref.fixed_name(name, field, sizeof(AllocatorMeta::name));
            CRD_FUZZ_REQUIRE(meta || ref.resolved_name(name));
        }
    }
    for (u32 i = 0U; i < view.correlation_count(); ++i)
    {
        const CorrelationRecord* c = view.correlation_at(i);
        CRD_FUZZ_REQUIRE(c != nullptr && ref.within(c, sizeof(*c), ref.frames_end, ref.size));
    }
    // Every name slot (a mutated slot count reaches further than the seeds' five), one past them, and the far end.
    CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(kInvalidNameId)));
    CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(NameId{0xFFFF'FFFEU})));
    const u32 probe = ref.name_slots < 512U ? ref.name_slots + 1U : 512U;
    for (u32 id = 0U; id < probe; ++id)
    {
        CRD_FUZZ_REQUIRE(ref.resolved_name(view.resolve_name(NameId{id})));
    }

    String              json;
    PerfettoExportStats stats;
    CRD_FUZZ_REQUIRE(export_perfetto_json(view, json, stats));
    CRD_FUZZ_REQUIRE(!json.empty());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const ConstSpan<u8> input(data, size);
    const bool          gate = validate_capture_buffer(input);
    const CaptureView   view(input);
    CRD_FUZZ_REQUIRE(gate == view.is_valid());
    CRD_FUZZ_REQUIRE(validate_capture_buffer(input) == gate);
#if !CRD_PERF_ENABLED
    CRD_FUZZ_REQUIRE(!gate);
#endif
    if (gate)
    {
        Reference reference;
        CRD_FUZZ_REQUIRE(reference.build(data, size));
        walk_view(view, reference);

        // The same capture one byte off its alignment.
        crd::fuzz::BudgetAllocator root;
        Array<u8>                  shifted(&root);
        shifted.resize(size + 1U);
        std::memcpy(shifted.data() + 1, data, size);
        CRD_FUZZ_REQUIRE(!validate_capture_buffer(ConstSpan<u8>(shifted.data() + 1, size)));
    }
    return 0;
}

namespace
{
// ---- Seed builder: the save_capture_to_buffer layout, written field by field. --------------------------------------

struct SeedThread
{
    const char* name;
    u32         samples;
};

struct SeedLayout
{
    usize threads    = 0U; // offset of the thread headers
    usize counters   = 0U;
    usize allocators = 0U;
    usize blob       = 0U;
    usize frames     = 0U;
    usize samples    = 0U; // the first thread's sample array
};

constexpr const char* kNames[] = {"frame", "update", "", "render", "upload"}; // slot 2 is empty

// A name stamped into a fixed field: copied with its terminator, the rest of the field zero (the writer's
// copy_to_fixed for names that fit, as every seed name does).
void put_name(u8* field, usize size, const char* name)
{
    std::memset(field, 0, size);
    std::memcpy(field, name, std::strlen(name) + 1U);
}

// What a seed capture holds. The writer's own shape is two threads with samples, three counters (every kind and
// type), two allocators and an exact name blob; more threads, counters or allocators are written without samples or
// values, and `blob_pad` declares the blob that many bytes larger than it is (the writer adds none).
struct SeedShape
{
    u32  frames      = 2U;
    bool correlation = false;
    u32  threads     = 2U;
    u32  counters    = 3U;
    u32  allocators  = 2U;
    u32  blob_pad    = 0U;
};

Array<u8> build_capture(const SeedShape& shape, SeedLayout* layout = nullptr)
{
    const SeedThread threads[] = {{"main", 3U}, {"worker-0", 2U}};
    const u32        frame_n     = shape.frames;
    const bool       correlation = shape.correlation;
    const u32        thread_n    = shape.threads;
    const u32        counter_n   = shape.counters;
    const u32        alloc_n     = shape.allocators;
    const u32        name_n      = sizeof(kNames) / sizeof(kNames[0]);
    u32              strings   = 0U;
    for (const char* n : kNames)
    {
        strings += (n[0] != '\0') ? static_cast<u32>(std::strlen(n)) + 1U : 0U;
    }
    const usize blob_bytes = ((8U + name_n * 4U + strings + 7U) & ~static_cast<usize>(7U)) + shape.blob_pad;

    SeedLayout l;
    l.threads    = sizeof(CprofHeader);
    l.counters   = l.threads + thread_n * sizeof(ThreadHeader);
    l.allocators = l.counters + counter_n * sizeof(CounterMeta);
    l.blob       = l.allocators + alloc_n * sizeof(AllocatorMeta);
    l.frames     = l.blob + blob_bytes;
    l.samples    = (l.frames + frame_n * sizeof(FrameRecord) + 7U) & ~static_cast<usize>(7U);
    usize total  = l.samples + (3U + 2U) * sizeof(Sample);
    const usize correlation_at = total;
    if (correlation)
    {
        total += 8U + 2U * sizeof(CorrelationRecord);
    }

    Array<u8> b;
    b.resize(total);
    std::memset(b.data(), 0, total);

    CprofHeader h{};
    h.magic                      = kCprofMagic;
    h.version                    = kCprofVersion;
    h.flags                      = correlation ? kCprofFlagCorrelation : 0U;
    h.captured_at_ns             = 123456789U;
    h.thread_count               = thread_n;
    h.counter_count              = counter_n;
    h.allocator_count            = alloc_n;
    h.frame_count                = frame_n;
    h.sample_struct_size         = sizeof(Sample);
    h.frame_record_size          = sizeof(FrameRecord);
    h.name_blob_byte_size        = blob_bytes;
    h.name_blob_count            = name_n;
    h.correlation_section_offset = correlation ? static_cast<u32>(correlation_at) : 0U;
    std::memcpy(b.data(), &h, sizeof(h));

    usize sample_at = l.samples;
    for (u32 i = 0U; i < thread_n; ++i)
    {
        ThreadHeader th{};
        th.thread_index       = i;
        const u32 samples     = (i < 2U) ? threads[i].samples : 0U;
        th.sample_count       = samples;
        th.sample_byte_offset = sample_at;
        th.dropped_count      = i;
        put_name(reinterpret_cast<u8*>(th.name), sizeof(th.name), (i < 2U) ? threads[i].name : "extra");
        std::memcpy(b.data() + l.threads + i * sizeof(ThreadHeader), &th, sizeof(th));
        for (u32 s = 0U; s < samples; ++s)
        {
            Sample sm{};
            sm.begin_ns     = 1000 + 100 * static_cast<crd::i64>(s);
            sm.end_ns       = sm.begin_ns + 50;
            sm.name_id      = s % name_n;
            sm.begin_thread = static_cast<u8>(i);
            sm.end_thread   = static_cast<u8>(i);
            sm.depth        = static_cast<u8>(s % 2U);
            std::memcpy(b.data() + sample_at, &sm, sizeof(sm));
            sample_at += sizeof(Sample);
        }
    }
    const char* counter_names[] = {"draw-calls", "gpu-ms", "upload-ns"};
    for (u32 i = 0U; i < counter_n; ++i)
    {
        CounterMeta m{};
        m.index = i;
        m.kind  = static_cast<u8>(i == 0U ? CounterKind::Add : CounterKind::Set);
        m.type  = static_cast<u8>(i < 3U ? i : 0U); // I64, F64, DurationNs
        put_name(reinterpret_cast<u8*>(m.name), sizeof(m.name), (i < 3U) ? counter_names[i] : "extra");
        std::memcpy(b.data() + l.counters + i * sizeof(CounterMeta), &m, sizeof(m));
    }
    const char* allocator_names[] = {"frame-arena", "resources"};
    for (u32 i = 0U; i < alloc_n; ++i)
    {
        AllocatorMeta m{};
        m.index = i;
        put_name(reinterpret_cast<u8*>(m.name), sizeof(m.name), (i < 2U) ? allocator_names[i] : "extra");
        std::memcpy(b.data() + l.allocators + i * sizeof(AllocatorMeta), &m, sizeof(m));
    }
    {
        u8* blob = b.data() + l.blob;
        std::memcpy(blob, &name_n, sizeof(u32));
        std::memcpy(blob + 4U, &strings, sizeof(u32));
        u32 cursor = 0U;
        for (u32 i = 0U; i < name_n; ++i)
        {
            u32 offset = 0xFFFF'FFFFU;
            if (kNames[i][0] != '\0')
            {
                offset           = cursor;
                const usize n    = std::strlen(kNames[i]);
                std::memcpy(blob + 8U + name_n * 4U + cursor, kNames[i], n);
                cursor += static_cast<u32>(n) + 1U;
            }
            std::memcpy(blob + 8U + i * 4U, &offset, sizeof(offset));
        }
    }
    for (u32 f = 0U; f < frame_n; ++f)
    {
        FrameRecord fr{};
        fr.frame_index     = f;
        fr.frame_begin_ns  = 1000 + 16000 * static_cast<crd::i64>(f);
        fr.frame_end_ns    = fr.frame_begin_ns + 16000;
        fr.counter_count   = (counter_n < kMaxCounters) ? counter_n : kMaxCounters; // a record holds no more
        fr.allocator_count = (alloc_n < kMaxAllocators) ? alloc_n : kMaxAllocators;
        fr.values[0].bits  = 12U + f;
        fr.values[1].bits  = 0x3FF8000000000000ULL; // 1.5
        fr.values[2].bits  = 250000U;
        fr.allocators[0]._pad = 4U; // stamped name id 3 ("render") + 1
        fr.allocators[1]._pad = 0U; // unset: the AllocatorMeta name
        std::memcpy(b.data() + l.frames + f * sizeof(FrameRecord), &fr, sizeof(fr));
    }
    if (correlation)
    {
        const u32 count = 2U;
        const u32 size  = sizeof(CorrelationRecord);
        std::memcpy(b.data() + correlation_at, &count, sizeof(count));
        std::memcpy(b.data() + correlation_at + 4U, &size, sizeof(size));
        for (u32 i = 0U; i < count; ++i)
        {
            CorrelationRecord c{};
            c.flags                = kCorrelationValid;
            c.thread_index         = 0U;
            c.sample_ordinal       = i;
            c.clock_uncertainty_ns = kUnknownClockUncertainty;
            c.pass_id              = (i == 0U) ? 3U : kNoCorrelationName;
            c.resource_id          = 4U;
            std::memcpy(b.data() + correlation_at + 8U + i * sizeof(CorrelationRecord), &c, sizeof(c));
        }
    }
    if (layout != nullptr)
    {
        *layout = l;
    }
    return b;
}

// Header field offsets (crd/perf/capture.hpp; pinned by test_diag_cprof_version.cpp).
constexpr usize kBlobBytes         = 56U;
constexpr usize kBlobCount         = 64U;
constexpr usize kCorrelationOffset = 68U;

template <typename T> void patch(Array<u8>& b, usize offset, T value)
{
    if (offset + sizeof(T) <= b.size())
    {
        std::memcpy(b.data() + offset, &value, sizeof(T));
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, const Array<u8>& b)
{
    sink.add(name, b.data(), b.size());
}
} // namespace

namespace
{
bool capture_accepted(const u8* data, usize size)
{
    return validate_capture_buffer(ConstSpan<u8>(data, size));
}

// The two well-formed captures, in a build with profiling; every other seed must be refused (and every seed is
// refused by the stub view of a build without it).
#if CRD_PERF_ENABLED
constexpr const char* kAcceptedSeeds[] = {"capture-two-frames-correlation", "capture-no-frames",
                                          "capture-frame-span-overflows"};
constexpr usize       kAcceptedCount   = 3U;
#else
constexpr const char* kAcceptedSeeds[] = {""};
constexpr usize       kAcceptedCount   = 0U;
#endif
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& out)
{
    crd::fuzz::VerdictSink sink(out, &capture_accepted, kAcceptedSeeds, kAcceptedCount);
    SeedLayout      l;
    SeedShape       shape;
    shape.correlation    = true;
    const Array<u8> good = build_capture(shape, &l);
    add(sink, "capture-two-frames-correlation", good);
    SeedShape no_frames;
    no_frames.frames = 0U;
    add(sink, "capture-no-frames", build_capture(no_frames));
    {
        // A well-formed capture whose frame span overflows a 64-bit difference: the reader accepts it, and the
        // Perfetto export must compute the duration without overflowing (an adopted libFuzzer input does the same
        // for a sample span).
        Array<u8> b = good;
        patch<crd::i64>(b, l.frames + 8U, std::numeric_limits<crd::i64>::min());
        patch<crd::i64>(b, l.frames + 16U, std::numeric_limits<crd::i64>::max());
        add(sink, "capture-frame-span-overflows", b);
    }

    // ThreadHeader: sample_count @4, sample_byte_offset @8, name @16. CounterMeta: kind @4, type @5, name @8.
    // AllocatorMeta: name @8. FrameRecord: counter_count @24, allocator_count @28.
    {
        // A name-blob size that wraps the section sum: the frame records then sit at the start of the buffer, where
        // the header's counts read as small frame counts, and every other check passes.
        Array<u8> b = good;
        patch<u64>(b, kBlobBytes, 0ULL - static_cast<u64>(l.blob));
        add(sink, "blob-size-wraps-sum", b);
    }
    {
        Array<u8> b = good;
        patch<u64>(b, l.threads + 8U, static_cast<u64>(b.size()) + 64U);
        add(sink, "samples-past-end", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.threads + 4U, 0x7FFF'FFFFU);
        add(sink, "sample-count-past-end", b);
    }
    {
        Array<u8> b = good; // inside the buffer, but over the thread headers
        patch<u64>(b, l.threads + 8U, sizeof(CprofHeader));
        add(sink, "samples-before-frames", b);
    }
    {
        Array<u8> b = good;
        patch<u64>(b, l.threads + 8U, l.samples + 4U);
        add(sink, "samples-misaligned", b);
    }
    {
        Array<u8> b = good; // the in-blob slot count disagrees with the header and overruns the section
        patch<u32>(b, l.blob, 5000U);
        patch<u32>(b, kBlobCount, 5000U);
        add(sink, "blob-slots-overrun", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.blob, 4U);
        add(sink, "blob-slots-disagree", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.blob + 4U, 4000U);
        add(sink, "blob-strings-overrun", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.blob + 8U + 3U * 4U, 900U);
        add(sink, "blob-offset-past-strings", b);
    }
    {
        Array<u8> b = good; // the last string's terminator replaced
        u32       strings = 0U;
        std::memcpy(&strings, b.data() + l.blob + 4U, sizeof(strings));
        patch<u8>(b, l.blob + 8U + 5U * 4U + strings - 1U, static_cast<u8>('x'));
        add(sink, "blob-strings-unterminated", b);
    }
    {
        // A blob smaller than its own 8 B header, at the end of the buffer: reading the header would pass the end.
        SeedLayout      bare;
        const Array<u8> base = build_capture(no_frames, &bare);
        Array<u8>       b    = base;
        patch<u64>(b, kBlobBytes, 4U);
        sink.add("blob-header-past-end", b.data(), bare.blob + 4U);
    }
    {
        // A blob declared four bytes larger, so the frame records after it are misaligned; everything else is laid
        // out consistently around it.
        SeedShape padded;
        padded.blob_pad = 4U;
        add(sink, "blob-size-unaligned", build_capture(padded));
    }
    {
        Array<u8> b = good;
        std::memset(b.data() + l.threads + 16U, 'n', 32U);
        add(sink, "thread-name-unterminated", b);
    }
    {
        Array<u8> b = good;
        std::memset(b.data() + l.counters + 8U, 'c', 56U);
        add(sink, "counter-name-unterminated", b);
    }
    {
        Array<u8> b = good;
        std::memset(b.data() + l.allocators + 8U, 'a', 56U);
        add(sink, "allocator-name-unterminated", b);
    }
    {
        Array<u8> b = good;
        patch<u8>(b, l.counters + 4U, 7U);
        add(sink, "counter-kind-unknown", b);
    }
    {
        Array<u8> b = good;
        patch<u8>(b, l.counters + sizeof(CounterMeta) + 5U, 9U);
        add(sink, "counter-type-unknown", b);
    }
    {
        // More threads, counters or allocators than the profiler's tables hold, each in a consistent layout (a frame
        // record holds at most kMaxCounters values and kMaxAllocators records).
        SeedShape many_threads;
        many_threads.frames  = 1U;
        many_threads.threads = kMaxThreads + 1U;
        add(sink, "thread-count-past-table", build_capture(many_threads));
        SeedShape many_counters;
        many_counters.frames   = 1U;
        many_counters.counters = kMaxCounters + 1U;
        add(sink, "counter-count-past-table", build_capture(many_counters));
        SeedShape many_allocators;
        many_allocators.frames     = 1U;
        many_allocators.allocators = kMaxAllocators + 1U;
        add(sink, "allocator-count-past-table", build_capture(many_allocators));
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.frames + 24U, kMaxCounters + 8U);
        add(sink, "frame-counter-count-past-values", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, l.frames + sizeof(FrameRecord) + 28U, kMaxAllocators + 1U);
        add(sink, "frame-allocator-count-past-records", b);
    }
    {
        Array<u8> b = good;
        patch<u32>(b, kCorrelationOffset, static_cast<u32>(b.size()) - 4U);
        add(sink, "correlation-offset-at-end", b);
    }
    {
        Array<u8> b = good; // the record count past the buffer
        u32       at = 0U;
        std::memcpy(&at, b.data() + kCorrelationOffset, sizeof(at));
        patch<u32>(b, at, 0x7FFF'FFFFU);
        add(sink, "correlation-count-past-end", b);
    }
    sink.add("truncated-header", good.data(), sizeof(CprofHeader) - 1U);
    sink.add("truncated-in-frames", good.data(), l.frames + 100U);
    sink.add("truncated-in-samples", good.data(), l.samples + 40U);
    sink.add_text("empty", "");
    sink.add_text("garbage", "CPRO is a magic, not a capture");
}
