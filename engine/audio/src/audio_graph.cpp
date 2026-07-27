// audio_graph.cpp — GEO-10: the offline graph renderer (see audio_graph.hpp).

#include <crd/audio/audio_graph.hpp>

#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/rbj.hpp>
#include <crd/hesap/interp/keyframe.hpp>
#include <crd/math/cmath.hpp>
#include <crd/time/rational_time.hpp>

namespace crd::audio
{

namespace
{
    // automation sample at frame `f` (sample-rate ticks) — rational-exact segment pick, hesap-interp
    // semantics inside the segment (the GEO-9 evaluator's shape, scalar lane)
    [[nodiscard]] crd::f32 auto_value_at(const AudioGraphResource& g, const AudioAutoRec& a, crd::i64 frame,
                                         crd::u32 sample_rate)
    {
        const crd::time::RationalTime t{frame, crd::time::make_rate(sample_rate, 1)};
        const crd::i64*               ticks = g.auto_ticks.data() + a.ticks_off;
        const crd::f32*               vals  = g.auto_values.data() + a.values_off;
        const auto                    inter = static_cast<crd::hesap::interp::KeyInterp>(a.interp);
        const crd::u32                elems = crd::hesap::interp::key_elements(inter);
        const auto key_time = [&](crd::u32 k) { return crd::time::RationalTime{ticks[k], a.rate}; };
        const auto key_value = [&](crd::u32 k) -> crd::f32 {
            return inter == crd::hesap::interp::KeyInterp::CubicHermite ? vals[k * elems + 1U]
                                                                        : vals[k * elems];
        };
        if (crd::time::compare(t, key_time(0)) <= 0) { return key_value(0); }
        if (crd::time::compare(t, key_time(a.key_count - 1)) >= 0) { return key_value(a.key_count - 1); }
        crd::u32 seg = 0;
        for (crd::u32 k = 1; k < a.key_count; ++k)
        {
            if (crd::time::compare(t, key_time(k)) < 0)
            {
                seg = k - 1;
                break;
            }
        }
        const crd::f64 s0 = crd::time::to_seconds_f64(key_time(seg));
        const crd::f64 s1 = crd::time::to_seconds_f64(key_time(seg + 1));
        const crd::f64 ts = crd::time::to_seconds_f64(t);
        switch (inter)
        {
        case crd::hesap::interp::KeyInterp::Step: return key_value(seg);
        case crd::hesap::interp::KeyInterp::Linear:
        {
            const crd::f64 u  = (ts - s0) / (s1 - s0);
            const crd::f64 v0 = static_cast<crd::f64>(key_value(seg));
            const crd::f64 v1 = static_cast<crd::f64>(key_value(seg + 1));
            return static_cast<crd::f32>(v0 + u * (v1 - v0));
        }
        case crd::hesap::interp::KeyInterp::CubicHermite:
        default:
        {
            const crd::f64 x2[2] = {s0, s1};
            const crd::f64 y2[2] = {static_cast<crd::f64>(vals[seg * 3U + 1U]),
                                    static_cast<crd::f64>(vals[(seg + 1U) * 3U + 1U])};
            const crd::f64 d2[2] = {static_cast<crd::f64>(vals[seg * 3U + 2U]),
                                    static_cast<crd::f64>(vals[(seg + 1U) * 3U])};
            crd::usize     cache = 0;
            return static_cast<crd::f32>(crd::hesap::interp::interp_hermite(
                crd::containers::ConstSpan<crd::f64>(x2, 2U), crd::containers::ConstSpan<crd::f64>(y2, 2U),
                crd::containers::ConstSpan<crd::f64>(d2, 2U), ts, cache));
        }
        }
    }

    // the automation record targeting (node, param), or nullptr
    [[nodiscard]] const AudioAutoRec* find_auto(const AudioGraphResource& g, crd::u32 node, AudioParam param)
    {
        for (const AudioAutoRec& a : g.automation)
        {
            if (a.node == node && a.param == static_cast<crd::u8>(param)) { return &a; }
        }
        return nullptr;
    }

    [[nodiscard]] crd::f32 db_to_linear(crd::f32 db) { return crd::math::pow(10.0F, db / 20.0F); }
} // namespace

crd::i64 render_graph(const AudioGraphResource& graph, crd::containers::ConstSpan<GraphSourceBinding> bindings,
                      crd::i64 frames, crd::containers::Array<crd::f32>& out)
{
    const crd::u32 n = static_cast<crd::u32>(graph.nodes.size());
    out.clear();
    if (n == 0 || n > 256 || graph.out_node >= n || frames <= 0 || bindings.size() < n) { return 0; }
    for (crd::u32 i = 0; i < n; ++i) // every Source must be bound — a missing buffer is a refusal, not silence
    {
        if (static_cast<AudioNodeType>(graph.nodes[i].type) == AudioNodeType::Source &&
            (bindings[i].samples.size() == 0 || bindings[i].channels == 0))
        {
            return 0;
        }
    }

    // topological order (validated DAG — Kahn)
    crd::u32 indegree[256] = {};
    crd::u32 order[256];
    crd::u32 count = 0;
    for (const AudioEdgeRec& e : graph.edges) { ++indegree[e.to]; }
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (indegree[i] == 0) { order[count++] = i; }
    }
    for (crd::u32 head = 0; head < count; ++head)
    {
        for (const AudioEdgeRec& e : graph.edges)
        {
            if (e.from == order[head] && --indegree[e.to] == 0) { order[count++] = e.to; }
        }
    }
    if (count != n) { return 0; }

    crd::memory::IAllocator* alloc = out.allocator();
    out.resize(static_cast<crd::usize>(frames) * 2U);
    for (crd::usize i = 0; i < out.size(); ++i) { out[i] = 0.0F; }

    // per-node stereo block buses + biquad state (f64, DF2T, per channel)
    constexpr crd::i64                block_cap = 256;
    crd::containers::Array<crd::f32>  bus(alloc);
    bus.resize(static_cast<crd::usize>(n) * block_cap * 2U);
    crd::f64 bq_z1[256][2] = {};
    crd::f64 bq_z2[256][2] = {};

    for (crd::i64 start = 0; start < frames; start += block_cap)
    {
        const crd::i64 len = frames - start < block_cap ? frames - start : block_cap;
        for (crd::usize i = 0; i < bus.size(); ++i) { bus[i] = 0.0F; }

        for (crd::u32 oi = 0; oi < n; ++oi)
        {
            const crd::u32      node_index = order[oi];
            const AudioNodeRec& node       = graph.nodes[node_index];
            crd::f32*           mine       = bus.data() + static_cast<crd::usize>(node_index) * block_cap * 2U;

            // sum inputs (every edge into me) — Mix semantics for every node type
            for (const AudioEdgeRec& e : graph.edges)
            {
                if (e.to != node_index) { continue; }
                const crd::f32* theirs = bus.data() + static_cast<crd::usize>(e.from) * block_cap * 2U;
                for (crd::i64 i = 0; i < len * 2; ++i) { mine[i] += theirs[i]; }
            }

            switch (static_cast<AudioNodeType>(node.type))
            {
            case AudioNodeType::Source:
            {
                const GraphSourceBinding& b       = bindings[node_index];
                const crd::u64            bframes = b.samples.size() / b.channels;
                for (crd::i64 i = 0; i < len; ++i)
                {
                    crd::u64 f = static_cast<crd::u64>(start + i) + static_cast<crd::u64>(node.start_frame);
                    if (node.loop != 0) { f %= bframes; }
                    if (f >= bframes) { continue; } // one-shot past the end = silence
                    if (b.channels >= 2)
                    {
                        mine[i * 2] += b.samples[f * b.channels];
                        mine[i * 2 + 1] += b.samples[f * b.channels + 1];
                    }
                    else // mono centers
                    {
                        const crd::f32 s = b.samples[f];
                        mine[i * 2] += s;
                        mine[i * 2 + 1] += s;
                    }
                }
                break;
            }
            case AudioNodeType::Gain:
            case AudioNodeType::Send:
            {
                const AudioAutoRec* autom = find_auto(graph, node_index, AudioParam::GainDb);
                for (crd::i64 i = 0; i < len; ++i)
                {
                    const crd::f32 db =
                        autom != nullptr ? auto_value_at(graph, *autom, start + i, graph.sample_rate)
                                         : node.gain_db;
                    const crd::f32 g = db_to_linear(db);
                    mine[i * 2] *= g;
                    mine[i * 2 + 1] *= g;
                }
                break;
            }
            case AudioNodeType::Biquad:
            {
                const AudioAutoRec* autom  = find_auto(graph, node_index, AudioParam::CutoffNyq);
                const crd::f32      cutoff = autom != nullptr
                                                 ? auto_value_at(graph, *autom, start, graph.sample_rate)
                                                 : node.cutoff; // coeffs per block (state carries across)
                crd::hesap::dsp::Biquad<crd::f64> bq;
                const crd::f64                    f0 = static_cast<crd::f64>(cutoff);
                const crd::f64                    q  = static_cast<crd::f64>(node.q);
                switch (static_cast<BiquadType>(node.filter))
                {
                case BiquadType::Lowpass: bq = crd::hesap::dsp::rbj_lowpass<crd::f64>(f0, q); break;
                case BiquadType::Highpass: bq = crd::hesap::dsp::rbj_highpass<crd::f64>(f0, q); break;
                case BiquadType::Bandpass: bq = crd::hesap::dsp::rbj_bandpass<crd::f64>(f0, q); break;
                case BiquadType::Notch:
                default: bq = crd::hesap::dsp::rbj_notch<crd::f64>(f0, q); break;
                }
                for (crd::i64 i = 0; i < len; ++i)
                {
                    for (int c = 0; c < 2; ++c)
                    {
                        const crd::f64 x = static_cast<crd::f64>(mine[i * 2 + c]);
                        const crd::f64 y = bq.b0 * x + bq_z1[node_index][c];
                        bq_z1[node_index][c] = bq.b1 * x - bq.a1 * y + bq_z2[node_index][c];
                        bq_z2[node_index][c] = bq.b2 * x - bq.a2 * y;
                        mine[i * 2 + c]      = static_cast<crd::f32>(y);
                    }
                }
                break;
            }
            case AudioNodeType::Mix:
            default: break; // inputs already summed
            }
        }

        const crd::f32* final_bus = bus.data() + static_cast<crd::usize>(graph.out_node) * block_cap * 2U;
        for (crd::i64 i = 0; i < len * 2; ++i) { out[static_cast<crd::usize>(start) * 2U + i] = final_bus[i]; }
    }
    return frames;
}

} // namespace crd::audio
