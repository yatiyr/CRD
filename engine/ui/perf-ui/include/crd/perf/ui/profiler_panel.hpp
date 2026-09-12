#pragma once

// ---------------------------------------------------------------------------
// crd-perf-ui -- ProfilerPanel (Detour D-003 v0g).
//
// Top-level ImGui panel for the profiler. One per process; constructed
// after `crd::imgui::ImGuiLayer` is attached. Renders a dockable
// multi-panel UI:
//
//   - Frame Summary    -- FPS, last-frame CPU/GPU bar, top-N regions
//   - Timeline         -- horizontal per-thread + GPU track; zoom/pan + hover
//   - Flame Graph      -- self-time aggregated by name
//   - Counters         -- multi-series line plots (rolling 240-frame history)
//   - GPU Passes       -- list of GPU spans (most recent frame)
//   - Memory           -- allocator table + bytes_in_use plot + peak markers
//   - Capture Controls -- record/clear/save/load + recent-captures list
//
// The panel renders from a polymorphic IProfilerSource so the same code
// handles live profiler + loaded CaptureView. Switching sources is one
// call: `set_source(...)`.
//
// Usage in an app:
//
//   crd::perf::ui::ProfilerPanel panel;
//   panel.set_source(&panel.live_source());  // start on the live profiler
//
//   // every frame, inside ImGui scope:
//   panel.draw();
//
// Saving / loading is driven through Capture Controls; the panel owns
// the IAllocator* used for capture buffers (default = MallocAllocator,
// inject your own via constructor).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/ui/capture_view_source.hpp>
#include <crd/perf/ui/profiler_source.hpp>

#include <memory>

namespace crd::perf::ui
{

class ProfilerPanel
{
public:
    explicit ProfilerPanel(crd::memory::IAllocator* capture_alloc = nullptr) noexcept;
    ~ProfilerPanel();

    ProfilerPanel(const ProfilerPanel&)            = delete;
    ProfilerPanel& operator=(const ProfilerPanel&) = delete;

    // Source switching. set_source(nullptr) == reset to live.
    void                       set_source(IProfilerSource* src) noexcept;
    [[nodiscard]] IProfilerSource& current_source() noexcept;
    [[nodiscard]] LiveProfilerSource& live_source() noexcept;

    // Draw one frame of the panel. Must be called inside an ImGui frame
    // (between NewFrame() and Render()). Begins / Ends its own windows
    // for each sub-panel.
    void draw() noexcept;

    // Capture lifecycle. These are also exposed via the Capture Controls
    // sub-panel; tests / engine code can drive them directly.
    [[nodiscard]] bool save_capture_to_file(const char* path) noexcept;
    [[nodiscard]] bool load_capture_from_file(const char* path) noexcept;

    // Per-panel toggles. Default = all on.
    void show_frame_summary(bool on) noexcept    { m_show_frame_summary = on; }
    void show_timeline(bool on) noexcept         { m_show_timeline = on; }
    void show_flame_graph(bool on) noexcept      { m_show_flame_graph = on; }
    void show_counters(bool on) noexcept         { m_show_counters = on; }
    void show_gpu_passes(bool on) noexcept       { m_show_gpu_passes = on; }
    void show_memory(bool on) noexcept           { m_show_memory = on; }
    void show_capture_controls(bool on) noexcept { m_show_capture_controls = on; }

private:
    crd::memory::IAllocator*        m_capture_alloc = nullptr;
    LiveProfilerSource              m_live;
    IProfilerSource*                m_source = nullptr;

    // Loaded-capture state. The buffer owns the bytes; the view + source
    // reference into it. When a new file loads, we replace the buffer +
    // rebuild the view + source.
    crd::containers::Array<crd::u8> m_loaded_buf;
    std::unique_ptr<CaptureView>    m_loaded_view;
    std::unique_ptr<CaptureViewSource> m_loaded_src;

    // Per-panel show flags.
    bool m_show_frame_summary    = true;
    bool m_show_timeline         = true;
    bool m_show_flame_graph      = true;
    bool m_show_counters         = true;
    bool m_show_gpu_passes       = true;
    bool m_show_memory           = true;
    bool m_show_capture_controls = true;

    // Timeline zoom/pan state -- persists across frames.
    float m_timeline_ns_per_pixel = 1000.0f; // 1 us per pixel default
    float m_timeline_scroll_ns    = 0.0f;

    // Capture controls -- save/load path buffers + paused toggle.
    char  m_save_path_buf[256]{};
    char  m_load_path_buf[256]{};
    bool  m_recording_paused = false;
};

} // namespace crd::perf::ui
