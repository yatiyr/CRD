// RAF-11 Inc2 — the single-asset hot-reload engine (device-free).
//
// Gates the reload CORE (`RenderAssetReloader`) against a REAL cook: a reloadable frame graph whose "source" is an
// in-memory `.frame.toml` string, cooked with the real `parse_frame_toml`, its generation carried by RAF-3's
// `RuntimeSlot`. The three §13 properties an engine gets nothing for free without:
//   • a byte-identical re-cook is a clean NO-OP (no swap, no generation bump);
//   • a changed+valid source SWAPS the live object, bumps the generation, and makes a pre-reload handle STALE;
//   • a broken source is REJECTED — the previous valid generation and live object are preserved (last-good), with a
//     named diagnostic, never a partial install.
//
// ⛔ named allocator throughout (construct<>/destroy<> — no hidden malloc); ASCII-only test names; NO device.

#include "../../engine/scene-render/src/reload.hpp"

#include <crd/framecook/frame_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/renderasset/renderasset.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // std::strlen

using crd::usize;
using crd::containers::String;
using crd::containers::StringView;
using crd::framecook::FrameCookError;
using crd::framecook::FrameGraphDesc;
using crd::scenerender::DeferredReleaseQueue;
using crd::scenerender::ReloadableVtbl;
using crd::scenerender::ReloadOutcome;
using crd::scenerender::RenderAssetReloader;
using namespace crd::renderasset;

namespace
{
StringView sv(const String& s) noexcept { return StringView(s.c_str(), s.size()); }

void set_source(String& s, const char* lit)
{
    s.clear();
    s.append(lit, std::strlen(lit));
}

// A minimal VALID frame graph (a shipped shape): one mesh-amplify pass into @output.
constexpr const char* kFrameA =
    "schema = 1\n"
    "name   = \"engine://frame/reload_test\"\n"
    "\n"
    "[[pass]]\n"
    "name        = \"amplify\"\n"
    "kind        = \"raster.mesh\"\n"
    "shader      = \"engine://scene/mesh\"\n"
    "writes      = [\"@output\"]\n"
    "clear_color = [0.0, 0.0, 0.0, 1.0]\n"
    "params      = { groups = 2 }\n";

// The SAME valid shape, one field changed (clear_color) — a real content change, still a legal frame.
constexpr const char* kFrameB =
    "schema = 1\n"
    "name   = \"engine://frame/reload_test\"\n"
    "\n"
    "[[pass]]\n"
    "name        = \"amplify\"\n"
    "kind        = \"raster.mesh\"\n"
    "shader      = \"engine://scene/mesh\"\n"
    "writes      = [\"@output\"]\n"
    "clear_color = [1.0, 0.0, 0.0, 1.0]\n"
    "params      = { groups = 2 }\n";

// Not a legal frame — parse/validate must reject it.
constexpr const char* kFrameBroken = "this is not a frame graph {{{ = = =\n";

// A reloadable frame-graph adapter: the reload plug-in the scene renderer will use for real, exercised here with an
// in-memory source. Owns the live/staged/retired FrameGraphDesc objects (named-allocator construct/destroy) and a
// RuntimeSlot for the generation. `retired` stands in for the Inc4 deferred-destruction queue (freed one reload late).
struct FrameReloadable
{
    crd::memory::IAllocator*     alloc;
    AssetId                      id;
    String                       source;
    RuntimeSlot<FrameGraphDesc>  slot;
    FrameGraphDesc*              staged  = nullptr;
    FrameGraphDesc*              retired = nullptr;
    InterfaceHash                iface{};
    InterfaceHash                staged_iface{};
    int                          commits  = 0;
    int                          discards = 0;

    FrameReloadable(crd::memory::IAllocator* a, AssetId i, const char* src) : alloc(a), id(i), source(a)
    {
        set_source(source, src);
    }
    ~FrameReloadable()
    {
        crd::memory::destroy(*alloc, slot.current());
        crd::memory::destroy(*alloc, staged);
        crd::memory::destroy(*alloc, retired);
    }
    FrameReloadable(const FrameReloadable&)            = delete;
    FrameReloadable& operator=(const FrameReloadable&) = delete;

    // Initial load — install generation 1; returns {content hash, cook error} so the caller asserts validity.
    ContentHash load(FrameCookError& err_out)
    {
        auto* d = crd::memory::construct<FrameGraphDesc>(*alloc, alloc);
        err_out = crd::framecook::parse_frame_toml(sv(source), *d);
        slot.install(d, id);
        iface = interface_hash_of(source.c_str(), source.size());
        return content_hash_of(source.c_str(), source.size());
    }
};

bool frame_stage(void* u, DiagnosticList& diags, ContentHash& out_content, InterfaceHash& out_iface)
{
    auto* f = static_cast<FrameReloadable*>(u);
    auto* d = crd::memory::construct<FrameGraphDesc>(*f->alloc, f->alloc);
    const FrameCookError err = crd::framecook::parse_frame_toml(sv(f->source), *d);
    if (err != FrameCookError::Ok)
    {
        crd::memory::destroy(*f->alloc, d); // nothing staged → last-good untouched
        diags.error(DiagCode::AssetCookFailed, StringView(crd::framecook::frame_cook_error_text(err)));
        return false;
    }
    f->staged        = d;
    out_content       = content_hash_of(f->source.c_str(), f->source.size());
    out_iface         = interface_hash_of(f->source.c_str(), f->source.size());
    f->staged_iface   = out_iface;
    return true;
}
void frame_commit(void* u)
{
    auto* f = static_cast<FrameReloadable*>(u);
    crd::memory::destroy(*f->alloc, f->retired); // free what was retired a reload ago (Inc4 gates this to a fence)
    f->retired = f->slot.current();
    f->slot.install(f->staged, f->id);
    f->staged = nullptr;
    f->iface  = f->staged_iface;
    ++f->commits;
}
void frame_discard(void* u)
{
    auto* f = static_cast<FrameReloadable*>(u);
    crd::memory::destroy(*f->alloc, f->staged);
    f->staged = nullptr;
    ++f->discards;
}
Generation    frame_generation(void* u) { return static_cast<FrameReloadable*>(u)->slot.generation(); }
InterfaceHash frame_iface(void* u) { return static_cast<FrameReloadable*>(u)->iface; }

const ReloadableVtbl kFrameVtbl{frame_stage, frame_commit, frame_discard, frame_generation,
                                                  frame_iface};
} // namespace

TEST_CASE("raf11 reload is no-op / swap / last-good on a real frame cook", "[raf11][reload]")
{
    crd::memory::TlsfAllocator alloc(1U << 22U, nullptr, "raf11-reload");
    DiagnosticList             diags(&alloc);

    const AssetId   fid = asset_id_of("engine://frame/reload_test");
    FrameReloadable f(&alloc, fid, kFrameA);
    FrameCookError    load_err = FrameCookError::Ok;
    const ContentHash c0       = f.load(load_err);
    REQUIRE(load_err == FrameCookError::Ok);
    REQUIRE(f.slot.generation() == Generation{1});

    RenderAssetReloader reloader(&alloc);
    reloader.register_asset(fid, &kFrameVtbl, &f, c0);
    REQUIRE(reloader.is_registered(fid));
    REQUIRE(reloader.generation_of(fid) == Generation{1});

    // 1) Re-cook of UNCHANGED source → clean no-op: no swap, no generation bump.
    const ReloadOutcome r1 = reloader.reload(fid, diags);
    CHECK(r1.ok);
    CHECK_FALSE(r1.changed);
    CHECK(r1.generation == Generation{1});
    CHECK(f.commits == 0);
    CHECK(f.discards == 1);
    CHECK_FALSE(diags.has_errors());

    // A handle minted now must stay current across a no-op but go STALE after the real swap below.
    const RuntimeHandle<FrameGraphDesc> h_before{f.slot.current(), fid, f.slot.generation()};
    REQUIRE(f.slot.is_current(h_before));

    // 2) CHANGED valid source → real swap: new object, generation bumps, the pre-reload handle is stale.
    FrameGraphDesc* const before = f.slot.current();
    set_source(f.source, kFrameB);
    const ReloadOutcome r2 = reloader.reload(fid, diags);
    CHECK(r2.ok);
    CHECK(r2.changed);
    CHECK(r2.generation == Generation{2});
    CHECK(f.commits == 1);
    CHECK(f.slot.current() != before);            // a genuinely new immutable object
    CHECK_FALSE(f.slot.is_current(h_before));      // the handle minted before the reload is now stale
    CHECK(reloader.generation_of(fid) == Generation{2});
    CHECK_FALSE(diags.has_errors());

    // 3) BROKEN source → REJECTED: last-good generation + live object preserved, named diagnostic, no partial install.
    FrameGraphDesc* const good = f.slot.current();
    set_source(f.source, kFrameBroken);
    const ReloadOutcome r3 = reloader.reload(fid, diags);
    CHECK_FALSE(r3.ok);
    CHECK_FALSE(r3.changed);
    CHECK(r3.generation == Generation{2});         // unchanged — the previous valid generation stands
    CHECK(f.slot.current() == good);               // last-good is still the live object
    CHECK(f.commits == 1);                          // no new commit happened
    CHECK(diags.contains(DiagCode::AssetCookFailed));

    // 4) Reloading an asset the reloader never registered is REPORTED, never silent.
    DiagnosticList      d2(&alloc);
    const ReloadOutcome r4 = reloader.reload(asset_id_of("engine://frame/unregistered"), d2);
    CHECK_FALSE(r4.ok);
    CHECK(d2.contains(DiagCode::AssetNotFound));
}

// ── Inc3: a dependency chain — a module + a consumer that BINDS the module's interface. Exercises the reloader's
//    orchestration (dependency-ordered rebuild · unaffected-dependent skip · interface-change rejection of the whole
//    set · atomic all-or-none commit / no mixed generation), independent of any specific asset kind. ──
namespace
{
constexpr crd::u64 kKvPrime            = 1000003ULL;
constexpr crd::u64 kKvMaxSupportedDep  = 2; // the consumer can bind module interfaces 1..2; interface 3 breaks it

struct Unit // the "cooked object" whose pointer identity the RuntimeSlot tracks
{
    crd::u64 content = 0;
    crd::u64 iface   = 0;
};

// content hash a cook produces: an asset folds its own content with the INTERFACE of the dependency it binds (so a
// dependency's interface change re-cooks it, but a dependency's content-only change leaves it byte-identical).
crd::u64 kv_content_hash(crd::u64 own_content, crd::u64 dep_iface) noexcept { return own_content * kKvPrime + dep_iface; }

struct KvReloadable
{
    crd::memory::IAllocator* alloc;
    AssetId                  id;
    crd::u64                 src_iface   = 1; // the "source" the test edits to simulate a change
    crd::u64                 src_content = 1;
    KvReloadable*            dep         = nullptr; // the module this consumer binds (null for the module itself)
    RuntimeSlot<Unit>        slot;
    Unit*                    staged      = nullptr;
    Unit*                    retired     = nullptr;
    int                      commits     = 0;
    int                      discards    = 0;
    int                      stage_fails = 0;

    KvReloadable(crd::memory::IAllocator* a, AssetId i, crd::u64 iface, crd::u64 content, KvReloadable* d)
        : alloc(a), id(i), src_iface(iface), src_content(content), dep(d)
    {
    }
    ~KvReloadable()
    {
        crd::memory::destroy(*alloc, slot.current());
        crd::memory::destroy(*alloc, staged);
        crd::memory::destroy(*alloc, retired);
    }
    KvReloadable(const KvReloadable&)            = delete;
    KvReloadable& operator=(const KvReloadable&) = delete;

    void                     load() { slot.install(crd::memory::construct<Unit>(*alloc, Unit{src_content, src_iface}), id); }
    [[nodiscard]] crd::u64   dep_iface() const noexcept { return (dep != nullptr) ? dep->src_iface : src_iface; }
    [[nodiscard]] ContentHash content_hash() const noexcept { return ContentHash{kv_content_hash(src_content, dep_iface())}; }
};

bool kv_stage(void* u, DiagnosticList& diags, ContentHash& out_content, InterfaceHash& out_iface)
{
    auto* k = static_cast<KvReloadable*>(u);
    if (k->dep != nullptr && k->dep->src_iface > kKvMaxSupportedDep)
    {
        diags.error(DiagCode::InterfaceIncompatible, "consumer cannot bind the module's new interface");
        ++k->stage_fails;
        return false; // nothing staged → last-good untouched, and the whole set is rejected
    }
    k->staged   = crd::memory::construct<Unit>(*k->alloc, Unit{k->src_content, k->src_iface});
    out_content = k->content_hash();
    out_iface   = InterfaceHash{k->src_iface};
    return true;
}
void kv_commit(void* u)
{
    auto* k = static_cast<KvReloadable*>(u);
    crd::memory::destroy(*k->alloc, k->retired);
    k->retired = k->slot.current();
    k->slot.install(k->staged, k->id);
    k->staged = nullptr;
    ++k->commits;
}
void kv_discard(void* u)
{
    auto* k = static_cast<KvReloadable*>(u);
    crd::memory::destroy(*k->alloc, k->staged);
    k->staged = nullptr;
    ++k->discards;
}
Generation    kv_generation(void* u) { return static_cast<KvReloadable*>(u)->slot.generation(); }
InterfaceHash kv_iface(void* u) { return InterfaceHash{static_cast<KvReloadable*>(u)->src_iface}; }

const ReloadableVtbl kKvVtbl{kv_stage, kv_commit, kv_discard, kv_generation, kv_iface};
} // namespace

TEST_CASE("raf11 reload rebuilds dependents atomically with interface-change rejection", "[raf11][reload]")
{
    crd::memory::TlsfAllocator alloc(1U << 22U, nullptr, "raf11-deps");
    DiagnosticList             diags(&alloc);

    const AssetId mid = asset_id_of("engine://shader/module");
    const AssetId cid = asset_id_of("engine://shader/consumer");

    KvReloadable mod(&alloc, mid, /*iface*/ 1, /*content*/ 1, /*dep*/ nullptr);
    KvReloadable con(&alloc, cid, /*iface*/ 1, /*content*/ 1, /*dep*/ &mod);
    mod.load();
    con.load();
    REQUIRE(mod.slot.generation() == Generation{1});
    REQUIRE(con.slot.generation() == Generation{1});

    RenderAssetReloader reloader(&alloc);
    reloader.register_asset(mid, &kKvVtbl, &mod, mod.content_hash());
    const AssetId con_deps[1] = {mid};
    reloader.register_asset(cid, &kKvVtbl, &con, con.content_hash(), con_deps, 1); // consumer DEPENDS ON module

    // 1) Module CONTENT-only change (interface unchanged) → module rebuilt, consumer UNAFFECTED (transparent).
    mod.src_content = 2;
    const ReloadOutcome r1 = reloader.reload(mid, diags);
    CHECK(r1.ok);
    CHECK(r1.changed);
    CHECK(mod.slot.generation() == Generation{2});
    CHECK(con.slot.generation() == Generation{1}); // binds the interface, not the body → its cook is byte-identical
    CHECK(mod.commits == 1);
    CHECK(con.commits == 0);
    CHECK_FALSE(diags.has_errors());

    // 2) Module COMPATIBLE interface change (1 → 2) → BOTH rebuild atomically, module before consumer.
    mod.src_iface = 2;
    const ReloadOutcome r2 = reloader.reload(mid, diags);
    CHECK(r2.ok);
    CHECK(r2.changed);
    CHECK(mod.slot.generation() == Generation{3});
    CHECK(con.slot.generation() == Generation{2}); // consumer re-cooked against the new interface
    CHECK(mod.commits == 2);
    CHECK(con.commits == 1);
    CHECK_FALSE(diags.has_errors());

    // 3) Module BREAKING interface change (2 → 3) → the consumer can no longer bind it → the WHOLE SET is rejected:
    //    NEITHER generation moves (no mixed generation), the previous valid versions stand, and it is reported.
    mod.src_iface = 3;
    const ReloadOutcome r3 = reloader.reload(mid, diags);
    CHECK_FALSE(r3.ok);
    CHECK(mod.slot.generation() == Generation{3}); // unchanged
    CHECK(con.slot.generation() == Generation{2}); // unchanged
    CHECK(mod.commits == 2);
    CHECK(con.commits == 1);
    CHECK(con.stage_fails == 1);
    CHECK(diags.contains(DiagCode::InterfaceIncompatible));
}

// ── Inc4: deferred GPU destruction. A retired object survives until `frames_in_flight` frames have passed (so every
//    frame that could reference it has completed), then is released EXACTLY ONCE; a shutdown drain frees the rest. ──
namespace
{
struct ReleaseSink
{
    int count = 0;
};
void sink_release(void* /*object*/, void* ctx) { static_cast<ReleaseSink*>(ctx)->count += 1; }
} // namespace

TEST_CASE("raf11 deferred release frees only after the in-flight window", "[raf11][reload]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf11-defer");
    ReleaseSink                sink;

    DeferredReleaseQueue q(&alloc, /*frames_in_flight*/ 2U);
    int                  a = 0;
    int                  b = 0;
    int                  c = 0;

    // Retire three objects during frame 0; a 2-deep presenter may reference them across the next two frames.
    q.retire(&a, sink_release, &sink);
    q.retire(&b, sink_release, &sink);
    q.retire(&c, sink_release, &sink);
    CHECK(q.pending() == 3);
    CHECK(sink.count == 0);

    q.begin_frame(); // frame 1: still in the in-flight window → NOTHING freed (freeing now would race the GPU)
    CHECK(q.pending() == 3);
    CHECK(sink.count == 0);

    q.begin_frame(); // frame 2: the window elapsed → all three released, exactly once each
    CHECK(q.pending() == 0);
    CHECK(sink.count == 3);

    // Objects retired on different frames are released on their OWN schedule (retire-frame + frames_in_flight).
    q.retire(&a, sink_release, &sink); // retired at frame 2
    q.begin_frame();                   // frame 3: 2 + 2 <= 3 is false → survives
    CHECK(q.pending() == 1);
    q.retire(&b, sink_release, &sink); // retired at frame 3
    q.begin_frame();                   // frame 4: a(2)+2<=4 releases a; b(3)+2<=4 is false → b survives
    CHECK(q.pending() == 1);
    CHECK(sink.count == 4);

    // Shutdown drain (device idle) frees whatever remains, immediately and exactly once.
    CHECK(q.drain_all() == 1);
    CHECK(q.pending() == 0);
    CHECK(sink.count == 5);
}
