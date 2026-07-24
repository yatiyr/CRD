// midi.cpp — GEO-10: the SMF parser + the MIDI resource (see midi.hpp).

#include <crd/audio/midi.hpp>

#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::audio
{

namespace
{
    [[nodiscard]] crd::u32 rd_be32(const crd::u8* p) noexcept
    {
        return (static_cast<crd::u32>(p[0]) << 24U) | (static_cast<crd::u32>(p[1]) << 16U) |
               (static_cast<crd::u32>(p[2]) << 8U) | p[3];
    }
    [[nodiscard]] crd::u16 rd_be16(const crd::u8* p) noexcept
    {
        return static_cast<crd::u16>((static_cast<crd::u32>(p[0]) << 8U) | p[1]);
    }

    struct ByteReader
    {
        const crd::u8* p    = nullptr;
        crd::usize     size = 0;
        crd::usize     pos  = 0;
        bool           ok   = true;

        [[nodiscard]] crd::u8 u8v()
        {
            if (pos >= size)
            {
                ok = false;
                return 0;
            }
            return p[pos++];
        }
        [[nodiscard]] crd::u32 vlq() // variable-length quantity (max 4 bytes per SMF)
        {
            crd::u32 v = 0;
            for (int i = 0; i < 4; ++i)
            {
                const crd::u8 b = u8v();
                v = (v << 7U) | (b & 0x7FU);
                if ((b & 0x80U) == 0) { return v; }
            }
            ok = false;
            return 0;
        }
        void skip(crd::usize n)
        {
            if (pos + n > size) { ok = false; }
            else { pos += n; }
        }
    };

    struct OpenNote
    {
        crd::i64 tick     = -1;
        crd::u32 velocity = 0;
    };

    struct MidiHeaders
    {
        crd::u32 note_count    = 0;
        crd::u32 control_count = 0;
        crd::u32 tempo_count   = 0;
        crd::u16 division      = 480;
        crd::u16 pad           = 0;
    };
    static_assert(sizeof(MidiHeaders) == 16);

    void push_bytes(crd::containers::Array<crd::u8>& out, const void* p, crd::usize n)
    {
        const auto* b = static_cast<const crd::u8*>(p);
        for (crd::usize i = 0; i < n; ++i) { out.push_back(b[i]); }
    }

    // insertion by tick keeps merged format-1 tracks ordered without a sort dependency (events per track
    // arrive tick-ordered; a binary-search insert is O(n log n) total against mostly-appended data)
    template <typename T>
    void insert_by_tick(crd::containers::Array<T>& arr, const T& item)
    {
        crd::usize lo = 0;
        crd::usize hi = arr.size();
        while (lo < hi)
        {
            const crd::usize mid = (lo + hi) / 2;
            if (arr[mid].tick <= item.tick) { lo = mid + 1; }
            else { hi = mid; }
        }
        arr.push_back(item); // grow, then shift into place
        for (crd::usize i = arr.size() - 1; i > lo; --i) { arr[i] = arr[i - 1]; }
        arr[lo] = item;
    }
} // namespace

MidiError midi_parse_smf(crd::containers::ConstSpan<crd::u8> bytes, MidiResource& out)
{
    out.notes.clear();
    out.controls.clear();
    out.tempo.clear();
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0 || rd_be32(bytes.data() + 4) != 6)
    {
        return MidiError::NotSmf;
    }
    const crd::u16 format   = rd_be16(bytes.data() + 8);
    const crd::u16 ntracks  = rd_be16(bytes.data() + 10);
    const crd::u16 division = rd_be16(bytes.data() + 12);
    if (format > 1) { return MidiError::UnsupportedFormat; }
    if ((division & 0x8000U) != 0) { return MidiError::UnsupportedFormat; } // SMPTE division — typed refusal
    if (division == 0 || ntracks == 0) { return MidiError::Malformed; }
    out.division = division;

    crd::usize pos = 14;
    for (crd::u16 t = 0; t < ntracks; ++t)
    {
        if (pos + 8 > bytes.size() || std::memcmp(bytes.data() + pos, "MTrk", 4) != 0)
        {
            return MidiError::Malformed;
        }
        const crd::u32 len = rd_be32(bytes.data() + pos + 4);
        if (pos + 8 + len > bytes.size()) { return MidiError::Malformed; }
        ByteReader br{bytes.data() + pos + 8, len, 0, true};
        pos += 8 + len;

        OpenNote open[16][128] = {};
        crd::i64 tick          = 0;
        crd::u8  status        = 0;
        bool     ended         = false;
        while (!ended && br.pos < br.size)
        {
            tick += br.vlq();
            crd::u8 b = br.u8v();
            if (!br.ok) { return MidiError::Malformed; }
            if (b < 0x80U) // running status
            {
                if (status < 0x80U) { return MidiError::Malformed; }
                --br.pos;
                b = status;
            }
            else { status = b; }

            const crd::u8 kind    = b & 0xF0U;
            const crd::u8 channel = b & 0x0FU;
            switch (kind)
            {
            case 0x80U: // note off
            case 0x90U:
            {
                const crd::u8 note = br.u8v() & 0x7FU;
                const crd::u8 vel  = br.u8v() & 0x7FU;
                const bool    on   = kind == 0x90U && vel != 0;
                if (on)
                {
                    open[channel][note] = {tick, midi1_velocity_to_32(vel)};
                }
                else if (open[channel][note].tick >= 0)
                {
                    MidiNote n;
                    n.tick         = open[channel][note].tick;
                    n.duration     = tick - n.tick;
                    n.channel      = channel;
                    n.note         = note;
                    n.velocity     = open[channel][note].velocity;
                    n.off_velocity = midi1_velocity_to_32(vel);
                    insert_by_tick(out.notes, n);
                    open[channel][note].tick = -1;
                }
                break;
            }
            case 0xB0U: // control change
            {
                MidiControl c;
                c.tick    = tick;
                c.channel = channel;
                c.kind    = 0;
                c.index   = br.u8v() & 0x7FU;
                c.value   = static_cast<crd::i32>(midi1_velocity_to_32(br.u8v() & 0x7FU) >> 1U);
                insert_by_tick(out.controls, c);
                break;
            }
            case 0xE0U: // pitch bend (14-bit, center 0x2000 → signed 32-bit center 0)
            {
                const crd::u32 lo = br.u8v() & 0x7FU;
                const crd::u32 hi = br.u8v() & 0x7FU;
                MidiControl    c;
                c.tick    = tick;
                c.channel = channel;
                c.kind    = 1;
                c.value   = (static_cast<crd::i32>((hi << 7U) | lo) - 0x2000) << 17; // full 32-bit span
                insert_by_tick(out.controls, c);
                break;
            }
            case 0xC0U: // program
            case 0xD0U: // channel pressure
            {
                MidiControl c;
                c.tick    = tick;
                c.channel = channel;
                c.kind    = kind == 0xC0U ? 3 : 2;
                const crd::u8 v = br.u8v() & 0x7FU;
                if (kind == 0xC0U) { c.index = v; }
                else { c.value = static_cast<crd::i32>(midi1_velocity_to_32(v) >> 1U); }
                insert_by_tick(out.controls, c);
                break;
            }
            case 0xA0U: // poly pressure — 2 data bytes, folded to pressure kind with note in index
            {
                MidiControl c;
                c.tick    = tick;
                c.channel = channel;
                c.kind    = 2;
                c.index   = br.u8v() & 0x7FU;
                c.value   = static_cast<crd::i32>(midi1_velocity_to_32(br.u8v() & 0x7FU) >> 1U);
                insert_by_tick(out.controls, c);
                break;
            }
            case 0xF0U:
            {
                if (b == 0xFFU) // meta
                {
                    const crd::u8  type = br.u8v();
                    const crd::u32 mlen = br.vlq();
                    if (type == 0x51U && mlen == 3) // tempo
                    {
                        MidiTempo tp;
                        tp.tick = tick;
                        tp.us_per_quarter = (static_cast<crd::u32>(br.u8v()) << 16U) |
                                            (static_cast<crd::u32>(br.u8v()) << 8U) | br.u8v();
                        insert_by_tick(out.tempo, tp);
                    }
                    else if (type == 0x2FU) { ended = true; br.skip(mlen); }
                    else { br.skip(mlen); }
                }
                else if (b == 0xF0U || b == 0xF7U) { br.skip(br.vlq()); } // sysex
                else { return MidiError::Malformed; }
                status = 0; // meta/sysex clear running status
                break;
            }
            default: return MidiError::Malformed;
            }
            if (!br.ok) { return MidiError::Malformed; }
        }
        // dangling note-ons close at end-of-track (honest: a truncated performance still imports)
        for (int ch = 0; ch < 16; ++ch)
        {
            for (int nt = 0; nt < 128; ++nt)
            {
                if (open[ch][nt].tick >= 0)
                {
                    MidiNote n;
                    n.tick     = open[ch][nt].tick;
                    n.duration = tick - n.tick;
                    n.channel  = static_cast<crd::u8>(ch);
                    n.note     = static_cast<crd::u8>(nt);
                    n.velocity = open[ch][nt].velocity;
                    insert_by_tick(out.notes, n);
                }
            }
        }
    }
    return MidiError::Ok;
}

crd::time::RationalTime midi_tick_to_time(const MidiResource& midi, crd::i64 tick) noexcept
{
    if (tick < 0 || midi.division == 0) { return {}; }
    // seconds = Σ segment_ticks × us_per_quarter / (division × 1e6) — accumulate EXACTLY in rational time
    crd::time::RationalTime acc{0, crd::time::kRate24};
    crd::i64                seg_start = 0;
    crd::u32                seg_uspq  = 500000;
    for (crd::usize i = 0; i <= midi.tempo.size(); ++i)
    {
        const crd::i64 seg_end = i < midi.tempo.size() && midi.tempo[i].tick < tick ? midi.tempo[i].tick : tick;
        if (seg_end > seg_start)
        {
            // dt ticks at rate (division × 1e6) / us_per_quarter ticks-per-second
            const crd::time::RationalRate r = crd::time::make_rate(
                static_cast<crd::i64>(midi.division) * 1000000LL, static_cast<crd::i64>(seg_uspq));
            if (!r.valid()) { return {}; }
            acc = crd::time::add(acc, crd::time::RationalTime{seg_end - seg_start, r});
        }
        if (i < midi.tempo.size())
        {
            if (midi.tempo[i].tick >= tick) { break; }
            seg_start = midi.tempo[i].tick;
            seg_uspq  = midi.tempo[i].us_per_quarter;
        }
    }
    return acc;
}

crd::containers::Array<crd::u8> midi_build(const MidiResource& midi, const crd::resources::ResourceId& id,
                                           crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> empty(alloc);
    if (midi.division == 0) { return empty; }
    for (crd::usize i = 1; i < midi.notes.size(); ++i) // tick order IS the format
    {
        if (midi.notes[i].tick < midi.notes[i - 1].tick) { return empty; }
    }

    MidiHeaders h;
    h.division      = midi.division;
    h.note_count    = static_cast<crd::u32>(midi.notes.size());
    h.control_count = static_cast<crd::u32>(midi.controls.size());
    h.tempo_count   = static_cast<crd::u32>(midi.tempo.size());

    crd::containers::Array<crd::u8> notes(alloc);
    push_bytes(notes, &h, sizeof(h));
    push_bytes(notes, midi.notes.data(), midi.notes.size() * sizeof(MidiNote));

    crd::resources::CrdrWriter w(alloc, id, kFourCC_MIDI);
    w.add_chunk(kFourCC_MdNt, crd::containers::as_const_span(notes));
    w.add_chunk(kFourCC_MdCc,
                crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(midi.controls.data()),
                                                    midi.controls.size() * sizeof(MidiControl)));
    w.add_chunk(kFourCC_MdTp,
                crd::containers::ConstSpan<crd::u8>(reinterpret_cast<const crd::u8*>(midi.tempo.data()),
                                                    midi.tempo.size() * sizeof(MidiTempo)));
    return w.finish();
}

void* MidiLoader::load(const crd::resources::LoadContext& ctx)
{
    crd::resources::CrdrFile file(&m_owned);
    if (crd::resources::crdr_read(ctx.bytes, file, &m_owned) != crd::resources::CrdrError::Ok) { return nullptr; }
    const crd::resources::CrdrChunk* nt = crd::resources::crdr_find_chunk(file, kFourCC_MdNt);
    const crd::resources::CrdrChunk* cc = crd::resources::crdr_find_chunk(file, kFourCC_MdCc);
    const crd::resources::CrdrChunk* tp = crd::resources::crdr_find_chunk(file, kFourCC_MdTp);
    if (nt == nullptr || cc == nullptr || tp == nullptr || nt->payload.size() < sizeof(MidiHeaders))
    {
        return nullptr;
    }
    MidiHeaders h;
    std::memcpy(&h, nt->payload.data(), sizeof(h));
    if (h.division == 0 ||
        nt->payload.size() != sizeof(h) + static_cast<crd::usize>(h.note_count) * sizeof(MidiNote) ||
        cc->payload.size() != static_cast<crd::usize>(h.control_count) * sizeof(MidiControl) ||
        tp->payload.size() != static_cast<crd::usize>(h.tempo_count) * sizeof(MidiTempo))
    {
        return nullptr;
    }
    void* raw = m_payload->try_allocate(sizeof(MidiResource), alignof(MidiResource));
    if (raw == nullptr) { return nullptr; }
    auto* m     = new (raw) MidiResource(m_payload);
    m->division = h.division;
    m->notes.resize(h.note_count);
    if (h.note_count > 0)
    {
        std::memcpy(m->notes.data(), nt->payload.data() + sizeof(h), nt->payload.size() - sizeof(h));
    }
    m->controls.resize(h.control_count);
    if (h.control_count > 0) { std::memcpy(m->controls.data(), cc->payload.data(), cc->payload.size()); }
    m->tempo.resize(h.tempo_count);
    if (h.tempo_count > 0) { std::memcpy(m->tempo.data(), tp->payload.data(), tp->payload.size()); }
    return m;
}

void MidiLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto*                    m = static_cast<MidiResource*>(payload);
    crd::memory::IAllocator* a = m_payload;
    m->~MidiResource();
    a->deallocate(m);
}

void register_midi_loader(crd::resources::ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<MidiLoader>(payload_alloc));
}

} // namespace crd::audio
