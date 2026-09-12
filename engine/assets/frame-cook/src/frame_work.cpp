// crd-frame-cook — CEIR-20b: extract_work_desc (see frame_work.hpp). Walks the authored frame's work passes → a
// WorkBuildDesc.

#include <crd/framecook/frame_work.hpp>

namespace crd::framecook
{
namespace ceg = crd::ceir::gpu;
namespace
{
using crd::containers::StringView;

[[nodiscard]] StringView sv(const crd::containers::String& s) noexcept
{
    return StringView(s.data(), s.size());
}

// A stable id for a resource NAME — the caller correlates each queue/binding Value → its device/host buffer by the SAME
// hash.
[[nodiscard]] crd::u64 name_hash(StringView s) noexcept
{
    crd::u64 h = 1469598103934665603ULL; // FNV-1a
    for (crd::usize i = 0; i < s.size(); ++i)
    {
        h ^= static_cast<crd::u8>(s.data()[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

[[nodiscard]] const FrameResourceDesc* find_res(const FrameGraphDesc& desc, StringView name) noexcept
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(desc.resources.size()); ++i)
    {
        if (sv(desc.resources[i].name) == name)
        {
            return &desc.resources[i];
        }
    }
    return nullptr;
}
[[nodiscard]] bool is_counter(const FrameGraphDesc& desc, StringView name) noexcept
{
    const FrameResourceDesc* const r = find_res(desc, name);
    return r != nullptr && r->kind == FrameResourceKind::CounterBuffer;
}

// The single CounterBuffer name among `refs` + the count of counters seen (for the exactly-1 validation).
[[nodiscard]] StringView sole_counter(const FrameGraphDesc& desc, const crd::containers::Array<FrameResourceRef>& refs,
                                      crd::u32& n_counter) noexcept
{
    n_counter = 0U;
    StringView found;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(refs.size()); ++i)
    {
        const StringView rn = sv(refs[i].name);
        if (is_counter(desc, rn))
        {
            ++n_counter;
            found = rn;
        }
    }
    return found;
}

// Find (or add) the queue for a CounterBuffer name; returns its index in out.queues (deduped — a `count` written by
// produce + read by consume is ONE queue). Reads capacity/record_stride from the resource desc (a counter is >= 4 bytes
// / >= 1 record).
[[nodiscard]] crd::u32 find_or_add_queue(ceg::WorkBuildDesc& out, const FrameGraphDesc& desc, StringView name) noexcept
{
    const crd::u64 h = name_hash(name);
    for (crd::u32 i = 0; i < out.num_queues; ++i)
    {
        if (out.queues[i].source_param == h)
        {
            return i;
        }
    }
    const crd::u32 idx = out.num_queues < 8U ? out.num_queues : 0U;
    if (out.num_queues < 8U)
    {
        const FrameResourceDesc* const r = find_res(desc, name);
        ceg::WorkQueueDesc& q = out.queues[idx];
        q.source_param = h;
        q.capacity = (r != nullptr && r->count > 0U) ? r->count : 1U;
        q.record_stride = (r != nullptr && r->stride > 0U) ? r->stride : 4U;
        ++out.num_queues;
    }
    return idx;
}
} // namespace

FrameCookError extract_work_desc(const FrameGraphDesc& desc, ceg::WorkBuildDesc& out, const char** where)
{
    out = ceg::WorkBuildDesc{};

    for (crd::u32 pi = 0; pi < static_cast<crd::u32>(desc.passes.size()); ++pi)
    {
        const FramePassDesc& p = desc.passes[pi];
        if (!pass_is_work(p))
        {
            continue;
        } // MIXED frame: a non-work pass (trace = compute.dispatch) is skipped
        if (out.num_stages >= 8U)
        {
            continue;
        }

        ceg::WorkStageDesc& st = out.stages[out.num_stages];
        st = ceg::WorkStageDesc{}; // default grid {1,1,1} (produce's serial fallback const grid)
        st.kind = pass_is_work_produce(p)   ? ceg::WorkStageKind::Produce
                  : pass_is_work_consume(p) ? ceg::WorkStageKind::Consume
                                            : ceg::WorkStageKind::Compact;
        st.kernel = pass_str(p, StringView(pp::kKernel)); // a StringView into `p` — `desc` must outlive `out`

        // ── identify the %queue(s) by CounterBuffer kind (the refined counter rule) + validate. ──
        crd::u32 nw = 0U;
        crd::u32 nr = 0U;
        const StringView cw = sole_counter(desc, p.writes, nw); // the written counter (produce %queue / compact %dst)
        const StringView cr = sole_counter(desc, p.reads, nr);  // the read counter    (consume %queue / compact %src)

        StringView q_write;
        StringView q_read;
        if (st.kind == ceg::WorkStageKind::Produce)
        {
            if (nw != 1U)
            {
                if (where != nullptr)
                {
                    *where = p.name.c_str();
                }
                return FrameCookError::WorkQueueNotOne;
            }
            q_write = cw;
            st.queue = find_or_add_queue(out, desc, cw);
        }
        else if (st.kind == ceg::WorkStageKind::Consume)
        {
            if (nr != 1U)
            {
                if (where != nullptr)
                {
                    *where = p.name.c_str();
                }
                return FrameCookError::WorkQueueNotOne;
            }
            q_read = cr;
            st.queue = find_or_add_queue(out, desc, cr);
        }
        else // Compact: src = read counter, dst = written counter (a real compact consumer ships later; validated for
             // symmetry)
        {
            if (nw != 1U || nr != 1U)
            {
                if (where != nullptr)
                {
                    *where = p.name.c_str();
                }
                return FrameCookError::WorkQueueNotOne;
            }
            q_write = cw;
            q_read = cr;
            st.src_queue = find_or_add_queue(out, desc, cr);
            st.queue = find_or_add_queue(out, desc, cw);
        }

        // ── every OTHER referenced resource (not the queue) is a binding. Order is deterministic (writes then reads);
        // the
        //    record-time caller re-orders to the kernel's SSBO slots by a fixed per-kernel table (source_param
        //    identity). ──
        const auto add_binding = [&](StringView name, ceg::WorkAccess acc)
        {
            if (name == q_write || name == q_read || st.num_bindings >= 8U)
            {
                return;
            }
            st.bindings[st.num_bindings].source_param = name_hash(name);
            st.bindings[st.num_bindings].access = acc;
            ++st.num_bindings;
        };
        for (crd::u32 w = 0; w < static_cast<crd::u32>(p.writes.size()); ++w)
        {
            add_binding(sv(p.writes[w].name), ceg::WorkAccess::Write);
        }
        for (crd::u32 r = 0; r < static_cast<crd::u32>(p.reads.size()); ++r)
        {
            add_binding(sv(p.reads[r].name), ceg::WorkAccess::Read);
        }

        ++out.num_stages;
    }
    return FrameCookError::Ok;
}
} // namespace crd::framecook
