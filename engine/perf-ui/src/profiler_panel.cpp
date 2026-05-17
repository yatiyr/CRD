// ---------------------------------------------------------------------------
// crd-perf-ui -- ProfilerPanel rendering (D-003 v0g).
//
// Seven sub-panels wired into a single dockable surface. Each
// sub-panel reads from the polymorphic IProfilerSource; the same code
// renders the live profiler and a loaded CaptureView.
//
// Naming: the ImGui window IDs are "Profiler/<subpanel>" so they group
// in ImGui's ini file under a single dock node.
// ---------------------------------------------------------------------------

#include <crd/perf/ui/profiler_panel.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/ui/panel_helpers.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crd::perf::ui
{

namespace
{

// Process-default capture allocator if the user doesn't inject one.
crd::memory::MallocAllocator& default_capture_alloc() noexcept
{
    static crd::memory::MallocAllocator s_alloc{"crd-perf-ui:capture"};
    return s_alloc;
}

// Format a percentage like "12.3%" into a small buffer.
void fmt_pct(double frac, char* buf, std::size_t n) noexcept
{
    std::snprintf(buf, n, "%.1f%%", frac * 100.0);
}

// Find the time bounds (begin_ns, end_ns) across every sample on every
// thread. Returns false if there are no samples.
bool compute_time_bounds(IProfilerSource& src, crd::i64& begin_ns, crd::i64& end_ns) noexcept
{
    bool found = false;
    begin_ns = 0;
    end_ns   = 0;
    const crd::u32 nt = src.thread_count();
    for (crd::u32 t = 0U; t < nt; ++t)
    {
        const auto samples = src.thread_samples(t);
        for (const auto& s : samples)
        {
            if (!found)
            {
                begin_ns = s.begin_ns;
                end_ns   = s.end_ns;
                found    = true;
            }
            else
            {
                if (s.begin_ns < begin_ns) begin_ns = s.begin_ns;
                if (s.end_ns   > end_ns)   end_ns   = s.end_ns;
            }
        }
    }
    return found;
}

// ===========================================================================
// Frame Summary
// ===========================================================================

void draw_frame_summary(IProfilerSource& src) noexcept
{
    if (!ImGui::Begin("Profiler / Frame Summary"))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Source: %s", src.source_label());
    ImGui::SameLine();
    ImGui::TextDisabled(src.is_live() ? "(live)" : "(loaded capture)");

    const crd::u32 nframes = src.frame_record_count();
    ImGui::Text("Frames captured: %u / %u", nframes, kFrameHistorySlots);

    if (nframes == 0U)
    {
        ImGui::TextDisabled("No frame data yet -- call frame_mark() to populate.");
        ImGui::End();
        return;
    }

    const auto* latest = src.frame_record(0U);
    if (latest == nullptr)
    {
        ImGui::TextDisabled("No latest frame record");
        ImGui::End();
        return;
    }

    const crd::i64 frame_ns =
        latest->frame_end_ns - latest->frame_begin_ns > 0
            ? latest->frame_end_ns - latest->frame_begin_ns
            : 0;
    char frame_buf[64];
    format_duration(frame_ns, frame_buf, sizeof(frame_buf));
    const double fps = frame_ns > 0 ? 1.0e9 / static_cast<double>(frame_ns) : 0.0;

    ImGui::Separator();
    ImGui::Text("Last frame: %s (%.1f FPS)", frame_buf, fps);

    // CPU time breakdown -- iterate every thread's depth-0 samples in the
    // most-recent frame's time window.
    const crd::i64 frame_begin = latest->frame_begin_ns;
    const crd::i64 frame_end   = latest->frame_end_ns;

    NameTotal totals[64]{};
    crd::u32  total_count = 0U;
    for (crd::u32 t = 0U; t < src.thread_count(); ++t)
    {
        const auto samples = src.thread_samples(t);
        // Reuse aggregator but only over samples in this frame's window.
        // For v0g we approximate by aggregating top-level samples on each
        // thread; future v0g2 may scope strictly to begin_ns >= frame_begin.
        (void)frame_begin;
        (void)frame_end;
        NameTotal local[32]{};
        const crd::u32 nl = aggregate_top_level_by_name(samples, local, 32U);
        for (crd::u32 i = 0U; i < nl; ++i)
        {
            bool merged = false;
            for (crd::u32 j = 0U; j < total_count; ++j)
            {
                if (totals[j].name.value == local[i].name.value)
                {
                    totals[j].total_ns += local[i].total_ns;
                    totals[j].occurrences += local[i].occurrences;
                    merged = true;
                    break;
                }
            }
            if (!merged && total_count < 64U)
            {
                totals[total_count++] = local[i];
            }
        }
    }

    // Sort descending by total_ns.
    std::sort(totals, totals + total_count,
              [](const NameTotal& a, const NameTotal& b) noexcept {
                  return a.total_ns > b.total_ns;
              });

    ImGui::Separator();
    ImGui::Text("Top regions by total time:");
    if (ImGui::BeginTable("frame_summary_top", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Avg",   ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Hits",  ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const crd::u32 show = total_count > 16U ? 16U : total_count;
        for (crd::u32 i = 0U; i < show; ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(src.resolve_name(totals[i].name));
            char tbuf[32];
            ImGui::TableSetColumnIndex(1);
            format_duration(static_cast<crd::i64>(totals[i].total_ns), tbuf, sizeof(tbuf));
            ImGui::TextUnformatted(tbuf);
            ImGui::TableSetColumnIndex(2);
            const crd::i64 avg = totals[i].occurrences > 0U
                                     ? static_cast<crd::i64>(totals[i].total_ns / totals[i].occurrences)
                                     : 0;
            format_duration(avg, tbuf, sizeof(tbuf));
            ImGui::TextUnformatted(tbuf);
            ImGui::TableSetColumnIndex(3);
            char cbuf[16];
            format_count(totals[i].occurrences, cbuf, sizeof(cbuf));
            ImGui::TextUnformatted(cbuf);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ===========================================================================
// Timeline -- horizontal per-thread tracks.
// ===========================================================================

void draw_timeline(IProfilerSource& src, float& ns_per_pixel, float& scroll_ns) noexcept
{
    if (!ImGui::Begin("Profiler / Timeline"))
    {
        ImGui::End();
        return;
    }

    crd::i64 win_begin = 0;
    crd::i64 win_end   = 0;
    if (!compute_time_bounds(src, win_begin, win_end))
    {
        ImGui::TextDisabled("No samples -- record some scopes to populate the timeline.");
        ImGui::End();
        return;
    }
    const double span_ns = static_cast<double>(win_end - win_begin);

    // Zoom controls.
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) { ns_per_pixel = static_cast<float>(span_ns / 1024.0); scroll_ns = 0.0F; }
    ImGui::SameLine();
    if (ImGui::Button("- 2x")) { ns_per_pixel *= 2.0F; }
    ImGui::SameLine();
    if (ImGui::Button("+ 2x")) { ns_per_pixel *= 0.5F; if (ns_per_pixel < 1.0F) ns_per_pixel = 1.0F; }
    ImGui::SameLine();
    ImGui::Text("(%.1f ns/px)", static_cast<double>(ns_per_pixel));

    // Track viewport.
    const float track_h = 22.0F;
    const float label_w = 140.0F;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float viewport_w =
        ImGui::GetContentRegionAvail().x - label_w;
    if (viewport_w <= 0.0F)
    {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const crd::u32 nt = src.thread_count();

    // Optional hover capture for tooltip.
    const Sample* hover_sample = nullptr;
    crd::u32       hover_thread = 0xFFFFFFFFU;
    const ImVec2   mouse = ImGui::GetMousePos();

    for (crd::u32 t = 0U; t < nt; ++t)
    {
        const float y0 = origin.y + static_cast<float>(t) * track_h;
        const float y1 = y0 + track_h - 2.0F;

        // Label.
        ImGui::SetCursorScreenPos({origin.x, y0 + 2.0F});
        const char* name = src.thread_name(t);
        ImGui::TextUnformatted(name != nullptr && name[0] != '\0' ? name : "(unnamed)");

        // Track background.
        dl->AddRectFilled({origin.x + label_w, y0},
                          {origin.x + label_w + viewport_w, y1},
                          IM_COL32(40, 40, 48, 200));

        // Draw each sample.
        const auto samples = src.thread_samples(t);
        for (const auto& s : samples)
        {
            const double off_begin =
                static_cast<double>(s.begin_ns - win_begin) - static_cast<double>(scroll_ns);
            const double off_end =
                static_cast<double>(s.end_ns - win_begin) - static_cast<double>(scroll_ns);
            const double ns_per_pixel_d = static_cast<double>(ns_per_pixel);
            const float x0 = origin.x + label_w + static_cast<float>(off_begin / ns_per_pixel_d);
            const float x1 = origin.x + label_w + static_cast<float>(off_end / ns_per_pixel_d);
            if (x1 < origin.x + label_w || x0 > origin.x + label_w + viewport_w)
            {
                continue;
            }
            // Color.
            crd::u32 col = s.color_rgba != 0U ? s.color_rgba
                                              : color_for_name(NameId{s.name_id}).value;
            if (s.color_rgba == 0U && s.category != 0U)
            {
                col = color_for_category(static_cast<Category>(s.category)).value;
            }
            // Depth-based vertical offset to show nesting (cap at track height).
            const float depth_off =
                static_cast<float>(s.depth) * 2.0F;
            dl->AddRectFilled({x0, y0 + 1.0F + depth_off}, {x1, y1 - depth_off}, col);
            // Migrate-split visual: red border if begin_thread != end_thread.
            if (s.begin_thread != s.end_thread)
            {
                dl->AddRect({x0, y0 + 1.0F + depth_off}, {x1, y1 - depth_off},
                            IM_COL32(255, 80, 80, 255), 0.0F, 0, 1.5F);
            }
            // Hover detection.
            if (mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1)
            {
                hover_sample = &s;
                hover_thread = t;
            }
        }
    }
    // Reserve canvas space.
    ImGui::Dummy({label_w + viewport_w, static_cast<float>(nt) * track_h + 4.0F});

    if (hover_sample != nullptr)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(src.resolve_name(NameId{hover_sample->name_id}));
        char tbuf[64];
        const crd::i64 dur = hover_sample->end_ns - hover_sample->begin_ns;
        format_duration(dur, tbuf, sizeof(tbuf));
        ImGui::Text("dur: %s", tbuf);
        ImGui::Text("thread: %s%s", src.thread_name(hover_thread),
                    hover_sample->begin_thread != hover_sample->end_thread ? " (migrated)" : "");
        ImGui::Text("depth: %u  fiber: 0x%08X", hover_sample->depth, hover_sample->fiber_id);
        ImGui::EndTooltip();
    }

    // Mouse pan (middle-button drag on window).
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0F))
    {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        scroll_ns -= d.x * ns_per_pixel;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }
    ImGui::End();
}

// ===========================================================================
// Flame Graph -- top-level region aggregation across all threads.
// ===========================================================================

void draw_flame_graph(IProfilerSource& src) noexcept
{
    if (!ImGui::Begin("Profiler / Flame Graph"))
    {
        ImGui::End();
        return;
    }

    NameTotal totals[128]{};
    crd::u32  count = 0U;
    for (crd::u32 t = 0U; t < src.thread_count(); ++t)
    {
        const auto samples = src.thread_samples(t);
        NameTotal local[64]{};
        const crd::u32 nl = aggregate_top_level_by_name(samples, local, 64U);
        for (crd::u32 i = 0U; i < nl; ++i)
        {
            bool merged = false;
            for (crd::u32 j = 0U; j < count; ++j)
            {
                if (totals[j].name.value == local[i].name.value)
                {
                    totals[j].total_ns += local[i].total_ns;
                    totals[j].occurrences += local[i].occurrences;
                    merged = true;
                    break;
                }
            }
            if (!merged && count < 128U)
            {
                totals[count++] = local[i];
            }
        }
    }
    if (count == 0U)
    {
        ImGui::TextDisabled("No samples -- record some scopes to populate the flame graph.");
        ImGui::End();
        return;
    }
    std::sort(totals, totals + count, [](const NameTotal& a, const NameTotal& b) noexcept {
        return a.total_ns > b.total_ns;
    });
    crd::u64 grand_total = 0U;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        grand_total += totals[i].total_ns;
    }
    if (grand_total == 0U)
    {
        ImGui::TextDisabled("All sampled durations are zero.");
        ImGui::End();
        return;
    }

    const float row_h = 22.0F;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    for (crd::u32 i = 0U; i < count; ++i)
    {
        const double frac = static_cast<double>(totals[i].total_ns)
                            / static_cast<double>(grand_total);
        const float y0 = origin.y + static_cast<float>(i) * row_h;
        const float y1 = y0 + row_h - 2.0F;
        const float x1 = origin.x + static_cast<float>(static_cast<double>(width) * frac);
        const crd::u32 col = color_for_name(totals[i].name).value;
        dl->AddRectFilled({origin.x, y0}, {x1, y1}, col);

        char tbuf[32];
        format_duration(static_cast<crd::i64>(totals[i].total_ns), tbuf, sizeof(tbuf));
        char pbuf[16];
        fmt_pct(frac, pbuf, sizeof(pbuf));
        char line[160];
        std::snprintf(line, sizeof(line), "%s   %s   %s",
                      src.resolve_name(totals[i].name), tbuf, pbuf);
        dl->AddText({origin.x + 4.0F, y0 + 3.0F}, IM_COL32(255, 255, 255, 230), line);
    }
    ImGui::Dummy({width, static_cast<float>(count) * row_h + 4.0F});
    ImGui::End();
}

// ===========================================================================
// Counters -- multi-series line plot over the rolling frame history.
// ===========================================================================

void draw_counters(IProfilerSource& src) noexcept
{
    if (!ImGui::Begin("Profiler / Counters"))
    {
        ImGui::End();
        return;
    }

    const crd::u32 nc = src.counter_count();
    const crd::u32 nf = src.frame_record_count();
    if (nc == 0U || nf == 0U)
    {
        ImGui::TextDisabled("No counters registered or no frames captured yet.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("counters_table", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Latest", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (crd::u32 i = 0U; i < nc; ++i)
        {
            ImGui::TableNextRow();
            const auto info = src.counter_info(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(info.name != nullptr ? info.name : "");
            ImGui::TableSetColumnIndex(1);
            const auto* rec = src.frame_record(0U);
            if (rec != nullptr && i < rec->counter_count)
            {
                char buf[32];
                if (info.type == CounterType::I64)
                {
                    std::snprintf(buf, sizeof(buf), "%lld",
                                  static_cast<long long>(rec->values[i].bits));
                }
                else if (info.type == CounterType::F64)
                {
                    // memcpy bit-cast — strict-aliasing safe (clang-cl
                    // -Werror=strict-aliasing rejects the reinterpret_cast form).
                    double d;
                    std::memcpy(&d, &rec->values[i].bits, sizeof(d));
                    std::snprintf(buf, sizeof(buf), "%.3f", d);
                }
                else
                {
                    const crd::i64 ns = static_cast<crd::i64>(rec->values[i].bits);
                    format_duration(ns, buf, sizeof(buf));
                }
                ImGui::TextUnformatted(buf);
            }
            else
            {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(info.kind == CounterKind::Set ? "Set" : "Add");
            ImGui::TableSetColumnIndex(3);
            const char* tn = "dur";
            if (info.type == CounterType::I64)      { tn = "i64"; }
            else if (info.type == CounterType::F64) { tn = "f64"; }
            ImGui::TextUnformatted(tn);
        }
        ImGui::EndTable();
    }

    // Mini per-counter plots.
    ImGui::Separator();
    for (crd::u32 i = 0U; i < nc; ++i)
    {
        const auto info = src.counter_info(i);
        float       values[240]{};
        // N = clamped frame-history sample count for this plot.
        const crd::u32 N = nf > 240U ? 240U : nf; // NOLINT(readability-identifier-naming)
        float       vmin = 1e30F;
        float       vmax = -1e30F;
        for (crd::u32 k = 0U; k < N; ++k)
        {
            const auto* rec = src.frame_record(N - 1U - k); // oldest first for plot
            if (rec == nullptr || i >= rec->counter_count)
            {
                values[k] = 0.0F;
            }
            else if (info.type == CounterType::I64)
            {
                values[k] = static_cast<float>(static_cast<crd::i64>(rec->values[i].bits));
            }
            else if (info.type == CounterType::F64)
            {
                // memcpy bit-cast — strict-aliasing safe (clang-cl
                // -Werror=strict-aliasing rejects the reinterpret_cast form).
                double d_val;
                std::memcpy(&d_val, &rec->values[i].bits, sizeof(d_val));
                values[k] = static_cast<float>(d_val);
            }
            else
            {
                values[k] =
                    static_cast<float>(static_cast<crd::i64>(rec->values[i].bits)) * 1e-6F;
            }
            if (values[k] < vmin) vmin = values[k];
            if (values[k] > vmax) vmax = values[k];
        }
        char label[80];
        std::snprintf(label, sizeof(label), "%s##plot_%u",
                      info.name != nullptr ? info.name : "", i);
        ImGui::PlotLines(label, values, static_cast<int>(N), 0, nullptr, vmin, vmax,
                         ImVec2{0.0F, 50.0F});
    }
    ImGui::End();
}

// ===========================================================================
// GPU Passes -- list of GPU samples from the gpu track (latest frame).
// ===========================================================================

void draw_gpu_passes(IProfilerSource& src) noexcept
{
    if (!ImGui::Begin("Profiler / GPU Passes"))
    {
        ImGui::End();
        return;
    }
    const crd::u8 gpu_idx = src.gpu_thread_index();
    if (gpu_idx == 0xFFU)
    {
        ImGui::TextDisabled("No GPU backend registered.");
        ImGui::End();
        return;
    }
    const auto samples = src.thread_samples(gpu_idx);
    if (samples.size() == 0U)
    {
        ImGui::TextDisabled("No GPU samples captured yet.");
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("gpu_table", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        // N = sample count (Profile DSL convention).
        const auto N = samples.size(); // NOLINT(readability-identifier-naming)
        const auto first = N > 64U ? N - 64U : 0U;
        for (auto i = first; i < N; ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(src.resolve_name(NameId{samples[i].name_id}));
            ImGui::TableSetColumnIndex(1);
            char buf[32];
            format_duration(samples[i].end_ns - samples[i].begin_ns, buf, sizeof(buf));
            ImGui::TextUnformatted(buf);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ===========================================================================
// Memory -- allocator table + bytes_in_use plots.
// ===========================================================================

void draw_memory(IProfilerSource& src) noexcept
{
    if (!ImGui::Begin("Profiler / Memory"))
    {
        ImGui::End();
        return;
    }

    const crd::u32 na = src.allocator_count();
    const crd::u32 nf = src.frame_record_count();
    if (na == 0U)
    {
        ImGui::TextDisabled("No allocators registered.");
        ImGui::End();
        return;
    }

    const auto* latest = nf > 0U ? src.frame_record(0U) : nullptr;
    if (ImGui::BeginTable("alloc_table", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Allocator", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("In-use",    ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Peak",      ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Allocs",    ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Deallocs",  ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (crd::u32 i = 0U; i < na; ++i)
        {
            ImGui::TableNextRow();
            const auto info = src.allocator_info(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(info.name != nullptr ? info.name : "");
            char buf[32];
            const auto& ar = (latest != nullptr && i < latest->allocator_count)
                                 ? latest->allocators[i]
                                 : AllocatorRecord{};
            ImGui::TableSetColumnIndex(1);
            format_bytes(ar.bytes_in_use, buf, sizeof(buf));
            ImGui::TextUnformatted(buf);
            ImGui::TableSetColumnIndex(2);
            format_bytes(ar.peak_bytes, buf, sizeof(buf));
            ImGui::TextUnformatted(buf);
            ImGui::TableSetColumnIndex(3);
            format_count(ar.alloc_count, buf, sizeof(buf));
            ImGui::TextUnformatted(buf);
            ImGui::TableSetColumnIndex(4);
            format_count(ar.dealloc_count, buf, sizeof(buf));
            ImGui::TextUnformatted(buf);
        }
        ImGui::EndTable();
    }

    // Per-allocator bytes_in_use plot.
    ImGui::Separator();
    for (crd::u32 i = 0U; i < na; ++i)
    {
        const auto info = src.allocator_info(i);
        float values[240]{};
        // N = clamped frame-history sample count for this plot.
        const crd::u32 N = nf > 240U ? 240U : nf; // NOLINT(readability-identifier-naming)
        float vmin = 1e30F;
        float vmax = -1e30F;
        for (crd::u32 k = 0U; k < N; ++k)
        {
            const auto* rec = src.frame_record(N - 1U - k);
            if (rec == nullptr || i >= rec->allocator_count)
            {
                values[k] = 0.0F;
            }
            else
            {
                values[k] = static_cast<float>(rec->allocators[i].bytes_in_use)
                            * (1.0F / 1024.0F);
            }
            if (values[k] < vmin) vmin = values[k];
            if (values[k] > vmax) vmax = values[k];
        }
        char label[80];
        std::snprintf(label, sizeof(label), "%s (KB)##alloc_plot_%u",
                      info.name != nullptr ? info.name : "", i);
        ImGui::PlotLines(label, values, static_cast<int>(N), 0, nullptr, vmin, vmax,
                         ImVec2{0.0F, 50.0F});
    }
    ImGui::End();
}

// ===========================================================================
// Capture Controls -- save / load / pause / recent.
// ===========================================================================

void draw_capture_controls(ProfilerPanel& panel,
                           char* save_buf, std::size_t save_n,
                           char* load_buf, std::size_t load_n,
                           bool& paused) noexcept
{
    if (!ImGui::Begin("Profiler / Capture Controls"))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Recording: %s", paused ? "PAUSED" : "ACTIVE");
    ImGui::SameLine();
    if (ImGui::Button(paused ? "Resume" : "Pause"))
    {
        paused = !paused;
        // For v0g we just toggle the visible state; clear_samples is the
        // mechanism users typically want. A future v0g2 may expose a
        // proper "stop recording" API on the profiler.
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear samples"))
    {
        crd::perf::clear_samples();
    }

    ImGui::Separator();
    ImGui::Text("Save capture:");
    ImGui::InputText("##save_path", save_buf, save_n);
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (save_buf[0] != '\0')
        {
            (void)panel.save_capture_to_file(save_buf);
        }
    }

    ImGui::Separator();
    ImGui::Text("Load capture:");
    ImGui::InputText("##load_path", load_buf, load_n);
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        if (load_buf[0] != '\0')
        {
            (void)panel.load_capture_from_file(load_buf);
        }
    }
    if (ImGui::Button("Back to live"))
    {
        panel.set_source(nullptr);
    }

    ImGui::End();
}

} // namespace

// ===========================================================================
// ProfilerPanel implementation
// ===========================================================================

ProfilerPanel::ProfilerPanel(crd::memory::IAllocator* capture_alloc) noexcept
    : m_capture_alloc(capture_alloc != nullptr ? capture_alloc : &default_capture_alloc())
    , m_source(&m_live)
    , m_loaded_buf(m_capture_alloc)
{
}

ProfilerPanel::~ProfilerPanel() = default;

void ProfilerPanel::set_source(IProfilerSource* src) noexcept
{
    m_source = src != nullptr ? src : static_cast<IProfilerSource*>(&m_live);
}

[[nodiscard]] IProfilerSource& ProfilerPanel::current_source() noexcept
{
    CRD_ASSERT(m_source != nullptr);
    return *m_source;
}

[[nodiscard]] LiveProfilerSource& ProfilerPanel::live_source() noexcept
{
    return m_live;
}

void ProfilerPanel::draw() noexcept
{
    if (m_source == nullptr)
    {
        m_source = &m_live;
    }
    IProfilerSource& src = *m_source;

    if (m_show_frame_summary)    { draw_frame_summary(src); }
    if (m_show_timeline)         { draw_timeline(src, m_timeline_ns_per_pixel, m_timeline_scroll_ns); }
    if (m_show_flame_graph)      { draw_flame_graph(src); }
    if (m_show_counters)         { draw_counters(src); }
    if (m_show_gpu_passes)       { draw_gpu_passes(src); }
    if (m_show_memory)           { draw_memory(src); }
    if (m_show_capture_controls)
    {
        draw_capture_controls(*this, m_save_path_buf, sizeof(m_save_path_buf),
                              m_load_path_buf, sizeof(m_load_path_buf),
                              m_recording_paused);
    }
}

[[nodiscard]] bool ProfilerPanel::save_capture_to_file(const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }
    return crd::perf::save_capture_to_file(path, m_capture_alloc);
}

[[nodiscard]] bool ProfilerPanel::load_capture_from_file(const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, "rb") != 0)
    {
        return false;
    }
#else
    fp = std::fopen(path, "rb");
#endif
    if (fp == nullptr)
    {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0)
    {
        std::fclose(fp);
        return false;
    }
    m_loaded_buf.resize(static_cast<crd::usize>(len));
    const auto read = std::fread(m_loaded_buf.data(), 1U, m_loaded_buf.size(), fp);
    std::fclose(fp);
    if (read != m_loaded_buf.size())
    {
        return false;
    }
    if (!crd::perf::validate_capture_buffer(
            crd::containers::ConstSpan<crd::u8>{m_loaded_buf.data(), m_loaded_buf.size()}))
    {
        return false;
    }
    m_loaded_view = std::make_unique<CaptureView>(
        crd::containers::ConstSpan<crd::u8>{m_loaded_buf.data(), m_loaded_buf.size()});
    if (!m_loaded_view->is_valid())
    {
        m_loaded_view.reset();
        return false;
    }
    m_loaded_src = std::make_unique<CaptureViewSource>(*m_loaded_view, "loaded");
    set_source(m_loaded_src.get());
    return true;
}

} // namespace crd::perf::ui
