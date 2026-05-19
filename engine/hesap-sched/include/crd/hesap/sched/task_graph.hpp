#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::hesap::sched
{
// -----------------------------------------------------------------------
// TileId / TileAccess / TileDep — value types for tile-level scheduling.
// Phase 3.1.6 v0d foundation. Formal dep-tracking DAG lands later per
// `v0d-formal-dag` follow-on.
// -----------------------------------------------------------------------

struct TileId
{
    crd::u32 idx = 0;

    [[nodiscard]] constexpr bool operator==(const TileId& other) const noexcept = default;
};

enum class TileAccess : crd::u8
{
    Read = 0,
    Write = 1,
    ReadWrite = 2,
};

struct TileDep
{
    TileId tile;
    TileAccess access = TileAccess::Read;
};

// -----------------------------------------------------------------------
// parallel_tiles_for — convenience wrapper around `crd::jobs::parallel_for`
// for 2D tile grids. Maps (i, j) ∈ [0, tile_rows) × [0, tile_cols) to a
// linear index; dispatches `num_jobs` workers each handling a contiguous
// range. Independent tiles only (no inter-tile dep tracking).
//
// Caller signature: `void(crd::u32 tile_row, crd::u32 tile_col)`.
// -----------------------------------------------------------------------

template <typename Fn>
inline void parallel_tiles_for(crd::u32 tile_rows, crd::u32 tile_cols, crd::u32 num_jobs, Fn fn)
{
    const crd::u32 total = tile_rows * tile_cols;
    if (total == 0)
    {
        return;
    }
    auto* counter = crd::jobs::parallel_for(total, num_jobs,
        [tile_cols, fn](crd::u32 begin, crd::u32 end)
        {
            for (crd::u32 k = begin; k < end; ++k)
            {
                const crd::u32 i = k / tile_cols;
                const crd::u32 j = k % tile_cols;
                fn(i, j);
            }
        });
    crd::jobs::wait(counter);
}

// -----------------------------------------------------------------------
// TaskGraph — accumulates independent tasks then dispatches via
// `crd::jobs::parallel_for`. v0d foundation form: no formal dep tracking;
// tasks added to a TaskGraph batch run concurrently when `execute()` is
// called. Adding inter-task deps is a no-op (the deps span is stored but
// not consulted in execute() — the formal DAG follow-on will use it).
// -----------------------------------------------------------------------

class TaskGraph
{
public:
    using TaskFn = void (*)(void* user_data, crd::u32 task_index);

    explicit TaskGraph(crd::memory::IAllocator* alloc) noexcept : m_tasks(alloc) {}

    struct Task
    {
        TaskFn fn = nullptr;
        void* user_data = nullptr;
    };

    // Submit a task with raw (TaskFn, user_data) pair. `deps` are recorded but
    // ignored in v0d foundation execute(); a future formal-DAG executor will
    // consume them.
    void add_task(TaskFn fn, void* user_data,
                  crd::containers::ConstSpan<TileDep> /*deps*/ = {})
    {
        m_tasks.push_back(Task{fn, user_data});
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_tasks.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_tasks.empty(); }
    void clear() noexcept { m_tasks.clear(); }

    // Execute all tasks in parallel using `num_jobs` worker fibers.
    // Caller must have called `crd::jobs::init()` beforehand.
    void execute(crd::u32 num_jobs)
    {
        if (m_tasks.empty())
        {
            return;
        }
        const auto* tasks_data = m_tasks.data();
        const crd::u32 total = static_cast<crd::u32>(m_tasks.size());
        auto* counter = crd::jobs::parallel_for(total, num_jobs,
            [tasks_data](crd::u32 begin, crd::u32 end)
            {
                for (crd::u32 k = begin; k < end; ++k)
                {
                    tasks_data[k].fn(tasks_data[k].user_data, k);
                }
            });
        crd::jobs::wait(counter);
    }

private:
    crd::containers::Array<Task> m_tasks;
};

// -----------------------------------------------------------------------
// DependencyGraph (v0d-formal-dag) — PLASMA/PaRSEC-style task DAG with
// per-tile read/write tracking. Topologically dispatches tasks as their
// in-degree reaches zero. Use for pipelined BLAS / LAPACK workloads where
// one tile's output feeds another tile's input (Cholesky panel/trailing,
// LU step, iterative refinement, etc.).
//
// Submission semantics:
//   - Each call to add_task takes a `deps` span describing the tiles
//     this task reads / writes / read-writes.
//   - Read(T)  depends on whoever wrote T last (RAW dep).
//   - Write(T) depends on whoever wrote T last (WAW) AND on every reader
//     of T since that writer (WAR).
//
// Determinism: when two tasks become ready simultaneously, they may execute
// in any order. Algorithms whose final result depends on dispatch order
// must encode that as additional deps (the DAG cannot be wrong; misuse
// can).
//
// Lifetime: `user_data` must outlive `execute()`. `m_tasks` storage
// remains valid until the next `clear()` or destruction.
// -----------------------------------------------------------------------

class DependencyGraph
{
public:
    using TaskFn = void (*)(void* user_data, crd::u32 task_index);

    explicit DependencyGraph(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_nodes(alloc), m_last_writer(alloc), m_pending_readers(alloc)
    {
    }

    struct Node
    {
        TaskFn fn = nullptr;
        void* user_data = nullptr;
        // Successors waiting on this task. Each successor's `unmet_in` is
        // decremented when this task completes.
        crd::containers::Array<crd::u32> successors;
        // How many predecessors still un-finished. Initialised at submission;
        // an atomic at execute() time so workers can decrement concurrently.
        crd::u32 unmet_in = 0;

        explicit Node(crd::memory::IAllocator* alloc) noexcept : successors(alloc) {}
    };

    [[nodiscard]] crd::usize size() const noexcept { return m_nodes.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
    void clear() noexcept
    {
        m_nodes.clear();
        m_last_writer.clear();
        m_pending_readers.clear();
    }

    // Add a task with explicit tile dependencies. Returns the task's index
    // (useful for diagnostics / further dep wiring; the executor doesn't
    // need it externally).
    crd::u32 add_task(TaskFn fn, void* user_data, crd::containers::ConstSpan<TileDep> deps = {})
    {
        const crd::u32 task_idx = static_cast<crd::u32>(m_nodes.size());
        Node node{m_alloc};
        node.fn = fn;
        node.user_data = user_data;
        // Compute in-edges: each dep determines which prior tasks this new
        // task waits on. Avoid duplicates so unmet_in counts edges, not deps.
        crd::containers::Array<crd::u32> preds(m_alloc);
        auto add_pred = [&](crd::u32 p)
        {
            for (crd::u32 q : preds)
            {
                if (q == p)
                {
                    return;
                }
            }
            preds.push_back(p);
        };
        for (const TileDep& dep : deps)
        {
            const crd::u32* writer = m_last_writer.find(dep.tile.idx);
            if (dep.access == TileAccess::Read)
            {
                if (writer != nullptr)
                {
                    add_pred(*writer);
                }
                // Record that this task reads this tile (pending until a new
                // writer arrives). HashMap insert preserves existing values,
                // so initialise the per-tile array if absent before pushing.
                if (m_pending_readers.find(dep.tile.idx) == nullptr)
                {
                    m_pending_readers.emplace(dep.tile.idx,
                                              crd::containers::Array<crd::u32>{m_alloc});
                }
                m_pending_readers.find(dep.tile.idx)->push_back(task_idx);
            }
            else // Write or ReadWrite
            {
                if (writer != nullptr)
                {
                    add_pred(*writer);
                }
                // WAR: must run after every reader since the last write.
                auto* readers = m_pending_readers.find(dep.tile.idx);
                if (readers != nullptr)
                {
                    for (crd::u32 r : *readers)
                    {
                        if (r != task_idx)
                        {
                            add_pred(r);
                        }
                    }
                    readers->clear();
                }
                // Upsert: HashMap.insert() preserves; explicit erase+insert
                // gives overwrite semantics.
                m_last_writer.erase(dep.tile.idx);
                m_last_writer.insert(dep.tile.idx, task_idx);
            }
        }
        node.unmet_in = static_cast<crd::u32>(preds.size());
        m_nodes.push_back(std::move(node));
        // Wire successor edges on predecessor nodes.
        for (crd::u32 p : preds)
        {
            m_nodes[p].successors.push_back(task_idx);
        }
        return task_idx;
    }

    // Run the DAG level-by-level. Each "level" is the set of currently-ready
    // tasks (unmet_in == 0). Tasks at one level run in parallel via
    // `crd::jobs::parallel_for`. When a level finishes, successors whose
    // unmet_in dropped to zero become the next level. Repeat until empty.
    //
    // This is simpler than a worker-pull-from-ring design and avoids fiber-
    // yield concerns. Trade-off: no overlap across levels (lose ~5-10% of
    // pipelined workloads). Filed: `v0d-formal-dag-pipeline` for the
    // pipelined variant when first consumer demands it.
    void execute(crd::u32 num_jobs)
    {
        if (m_nodes.empty())
        {
            return;
        }
        const crd::u32 n = static_cast<crd::u32>(m_nodes.size());

        // Per-task remaining-predecessor count (mutable across levels).
        crd::containers::Array<crd::u32> unmet(m_alloc);
        unmet.reserve(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            unmet.push_back(m_nodes[i].unmet_in);
        }

        // Initial ready set.
        crd::containers::Array<crd::u32> current(m_alloc);
        crd::containers::Array<crd::u32> next(m_alloc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            if (unmet[i] == 0)
            {
                current.push_back(i);
            }
        }

        const Node* nodes_ptr = m_nodes.data();
        while (!current.empty())
        {
            // Run all tasks at this level in parallel.
            const crd::u32* batch = current.data();
            const crd::u32 batch_n = static_cast<crd::u32>(current.size());
            const crd::u32 chunks = std::min(batch_n, num_jobs);
            auto* counter = crd::jobs::parallel_for(
                batch_n, chunks,
                [batch, nodes_ptr](crd::u32 begin, crd::u32 end) {
                    for (crd::u32 k = begin; k < end; ++k)
                    {
                        const crd::u32 task_idx = batch[k];
                        const Node& node = nodes_ptr[task_idx];
                        node.fn(node.user_data, task_idx);
                    }
                });
            crd::jobs::wait(counter);

            // Build next level: successors whose unmet count drops to zero.
            next.clear();
            for (crd::u32 idx : current)
            {
                for (crd::u32 succ_idx : m_nodes[idx].successors)
                {
                    --unmet[succ_idx];
                    if (unmet[succ_idx] == 0)
                    {
                        next.push_back(succ_idx);
                    }
                }
            }
            // Swap current ← next.
            current.clear();
            for (crd::u32 idx : next)
            {
                current.push_back(idx);
            }
        }
    }

private:
    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<Node> m_nodes;
    // Per-tile: the index of the LAST task that wrote this tile.
    crd::containers::HashMap<crd::u32, crd::u32> m_last_writer;
    // Per-tile: tasks that have read this tile since its last write. Cleared
    // when a new writer arrives (since the new writer subsumes them via WAR).
    crd::containers::HashMap<crd::u32, crd::containers::Array<crd::u32>> m_pending_readers;
};

} // namespace crd::hesap::sched
