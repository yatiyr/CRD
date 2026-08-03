#include <crd/geometry/mesh_processing/cluster_bvh.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cfloat>
#include <cmath>

namespace crd::geometry::mesh_processing
{
namespace
{

// ── AABB proxy for SAH cost computation ──────────────────────────────────

struct Aabb
{
    crd::f32 mn[3]{FLT_MAX, FLT_MAX, FLT_MAX};
    crd::f32 mx[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    void expand_sphere(const crd::f32* c, crd::f32 r) noexcept
    {
        for (crd::u32 a = 0U; a < 3U; ++a)
        {
            if (c[a] - r < mn[a]) mn[a] = c[a] - r;
            if (c[a] + r > mx[a]) mx[a] = c[a] + r;
        }
    }

    void merge(const Aabb& o) noexcept
    {
        for (crd::u32 a = 0U; a < 3U; ++a)
        {
            if (o.mn[a] < mn[a]) mn[a] = o.mn[a];
            if (o.mx[a] > mx[a]) mx[a] = o.mx[a];
        }
    }

    crd::f32 half_area() const noexcept
    {
        const crd::f32 dx = mx[0] - mn[0];
        const crd::f32 dy = mx[1] - mn[1];
        const crd::f32 dz = mx[2] - mn[2];
        return dx * dy + dy * dz + dz * dx;
    }

    bool valid() const noexcept { return mn[0] <= mx[0]; }
};

// ── Bounding-sphere merge ────────────────────────────────────────────────
// Safe when c1==oc (in-place accumulation): r1 is by-value, and each
// oc[k] write follows the c1[k] read in the same expression.

static void merge_sphere(const crd::f32* c1, crd::f32 r1,
                         const crd::f32* c2, crd::f32 r2,
                         crd::f32* oc, crd::f32& out_r) noexcept
{
    const crd::f32 dx = c2[0] - c1[0];
    const crd::f32 dy = c2[1] - c1[1];
    const crd::f32 dz = c2[2] - c1[2];
    const crd::f32 d  = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (d < 1.0e-12F)
    {
        if (r1 >= r2) { oc[0] = c1[0]; oc[1] = c1[1]; oc[2] = c1[2]; out_r = r1; }
        else          { oc[0] = c2[0]; oc[1] = c2[1]; oc[2] = c2[2]; out_r = r2; }
        return;
    }
    if (d + r2 <= r1)
    {
        oc[0] = c1[0]; oc[1] = c1[1]; oc[2] = c1[2]; out_r = r1;
        return;
    }
    if (d + r1 <= r2)
    {
        oc[0] = c2[0]; oc[1] = c2[1]; oc[2] = c2[2]; out_r = r2;
        return;
    }

    const crd::f32 new_r = (d + r1 + r2) * 0.5F;
    const crd::f32 t     = (new_r - r1) / d;
    oc[0] = c1[0] + dx * t;
    oc[1] = c1[1] + dy * t;
    oc[2] = c1[2] + dz * t;
    out_r = new_r;
}

// ── 16-bin SAH ───────────────────────────────────────────────────────────

static constexpr crd::u32 kBinCount = 16U;

struct SahBin
{
    Aabb     bounds;
    crd::u32 count = 0U;
};

struct SahSplit
{
    crd::u32 axis      = 0U;
    crd::f32 cost      = FLT_MAX;
    crd::f32 threshold = 0.0F;
};

static SahSplit find_best_split(const DagCluster* clusters, const crd::u32* idx,
                                crd::u32 begin, crd::u32 end) noexcept
{
    SahSplit best;

    for (crd::u32 axis = 0U; axis < 3U; ++axis)
    {
        crd::f32 cmin = FLT_MAX, cmax = -FLT_MAX;
        for (crd::u32 i = begin; i < end; ++i)
        {
            const crd::f32 c = clusters[idx[i]].center[axis];
            if (c < cmin) cmin = c;
            if (c > cmax) cmax = c;
        }
        const crd::f32 extent = cmax - cmin;
        if (extent < 1.0e-12F) continue;

        SahBin bins[kBinCount]{};
        const crd::f32 scale = static_cast<crd::f32>(kBinCount) / extent;
        for (crd::u32 i = begin; i < end; ++i)
        {
            const auto& cl = clusters[idx[i]];
            crd::u32 b = static_cast<crd::u32>((cl.center[axis] - cmin) * scale);
            if (b >= kBinCount) b = kBinCount - 1U;
            bins[b].bounds.expand_sphere(cl.center, cl.radius);
            bins[b].count++;
        }

        Aabb     prefix_bounds[kBinCount]{};
        crd::u32 prefix_count[kBinCount]{};
        prefix_bounds[0] = bins[0].bounds;
        prefix_count[0]  = bins[0].count;
        for (crd::u32 k = 1U; k < kBinCount; ++k)
        {
            prefix_bounds[k] = prefix_bounds[k - 1U];
            if (bins[k].bounds.valid()) prefix_bounds[k].merge(bins[k].bounds);
            prefix_count[k] = prefix_count[k - 1U] + bins[k].count;
        }

        Aabb     right_bounds = bins[kBinCount - 1U].bounds;
        crd::u32 right_count  = bins[kBinCount - 1U].count;

        for (crd::u32 k = kBinCount - 2U; k < kBinCount; --k)
        {
            if (prefix_count[k] > 0U && right_count > 0U
                && prefix_bounds[k].valid() && right_bounds.valid())
            {
                const crd::f32 cost =
                    static_cast<crd::f32>(prefix_count[k]) * prefix_bounds[k].half_area()
                  + static_cast<crd::f32>(right_count)      * right_bounds.half_area();
                if (cost < best.cost)
                {
                    best.cost      = cost;
                    best.axis      = axis;
                    best.threshold = cmin + static_cast<crd::f32>(k + 1U) * extent
                                         / static_cast<crd::f32>(kBinCount);
                }
            }

            if (bins[k].bounds.valid()) right_bounds.merge(bins[k].bounds);
            right_count += bins[k].count;
        }
    }

    if (best.cost >= FLT_MAX)
    {
        crd::f32 best_extent = -1.0F;
        for (crd::u32 axis = 0U; axis < 3U; ++axis)
        {
            crd::f32 cmin = FLT_MAX, cmax = -FLT_MAX;
            for (crd::u32 i = begin; i < end; ++i)
            {
                const crd::f32 c = clusters[idx[i]].center[axis];
                if (c < cmin) cmin = c;
                if (c > cmax) cmax = c;
            }
            if (cmax - cmin > best_extent)
            {
                best_extent    = cmax - cmin;
                best.axis      = axis;
                best.threshold = (cmin + cmax) * 0.5F;
            }
        }
    }

    return best;
}

// ── Partition ────────────────────────────────────────────────────────────

static crd::u32 partition_indices(crd::u32* idx, crd::u32 begin, crd::u32 end,
                                  crd::u32 axis, crd::f32 threshold,
                                  const DagCluster* clusters) noexcept
{
    crd::u32 lo = begin, hi = end;
    while (lo < hi)
    {
        if (clusters[idx[lo]].center[axis] < threshold)
            ++lo;
        else
        {
            --hi;
            const crd::u32 tmp = idx[lo];
            idx[lo] = idx[hi];
            idx[hi] = tmp;
        }
    }
    if (lo == begin) ++lo;
    if (lo == end)   --lo;
    return lo;
}

// ── Recursive DFS builder ────────────────────────────────────────────────

struct BvhBuilder
{
    const DagCluster*                       clusters;
    crd::u32*                               idx;
    crd::containers::Array<ClusterBvhNode>& nodes;
    crd::u32                                next_node  = 0U;
    crd::u32                                max_depth  = 0U;
    crd::u32                                leaf_count = 0U;

    void build(crd::u32 begin, crd::u32 end, crd::u32 depth)
    {
        const crd::u32 ni = next_node++;
        auto&          node = nodes[ni];

        node.max_error        = 0.0F;
        node.min_parent_error = FLT_MAX;
        for (crd::u32 i = begin; i < end; ++i)
        {
            const auto& c = clusters[idx[i]];
            if (c.error > node.max_error)               node.max_error        = c.error;
            if (c.parent_error < node.min_parent_error)  node.min_parent_error = c.parent_error;
        }

        if (depth > max_depth) max_depth = depth;

        if (end - begin == 1U)
        {
            const auto& c = clusters[idx[begin]];
            node.center[0] = c.center[0];
            node.center[1] = c.center[1];
            node.center[2] = c.center[2];
            node.radius    = c.radius;
            node.left  = idx[begin];
            node.right = 0xFFFFFFFFU;
            ++leaf_count;
            return;
        }

        const auto     split = find_best_split(clusters, idx, begin, end);
        const crd::u32 mid   = partition_indices(idx, begin, end,
                                                  split.axis, split.threshold, clusters);

        node.left = ni + 1U;
        build(begin, mid, depth + 1U);

        node.right = next_node;
        build(mid, end, depth + 1U);

        merge_sphere(nodes[node.left].center, nodes[node.left].radius,
                     nodes[node.right].center, nodes[node.right].radius,
                     node.center, node.radius);
    }
};

} // anonymous namespace

// ── Public entry point ───────────────────────────────────────────────────

ClusterBvhReport build_cluster_bvh(const DagCluster* clusters, crd::u32 cluster_count,
                                   ClusterBvhResult& out,
                                   crd::memory::IAllocator* scratch)
{
    ClusterBvhReport report;

    if (cluster_count == 0U)
    {
        report.status = ClusterBvhStatus::EmptyInput;
        return report;
    }

    const crd::u32 max_nodes = cluster_count * 2U - 1U;
    out.nodes.resize(max_nodes);

    crd::containers::Array<crd::u32> indices(scratch);
    indices.resize(cluster_count);
    for (crd::u32 i = 0U; i < cluster_count; ++i) indices[i] = i;

    BvhBuilder builder{clusters, indices.data(), out.nodes, 0U, 0U, 0U};
    builder.build(0U, cluster_count, 0U);

    out.node_count = builder.next_node;
    out.leaf_count = builder.leaf_count;
    out.depth      = builder.max_depth;
    if (out.node_count < max_nodes)
        out.nodes.resize(out.node_count);

    report.status     = ClusterBvhStatus::Ok;
    report.node_count = out.node_count;
    report.leaf_count = out.leaf_count;
    report.depth      = builder.max_depth;
    return report;
}

} // namespace crd::geometry::mesh_processing
