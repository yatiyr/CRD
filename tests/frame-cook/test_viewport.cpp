// test_viewport.cpp — REN-37.8 + 37.9 GATE (D-007 row 140): the FRAME-LEVEL graph and its viewport scheduler.
//
// The editor-shaped test from the spec (§15.6): 1 main viewport (EveryFrame) + 16 thumbnails (OnDemand) + 1
// animation preview, with a budget admitting a handful of on-demand viewports per frame.
//
// What each claim is protecting against, stated so a future edit cannot quietly weaken it:
//   · the main viewport is admitted FIRST and is never starved by thumbnails — non-negotiable;
//   · every dirty thumbnail renders within a BOUNDED number of frames, oldest-first (⭐ ageing; without it a
//     stream of high-priority invalidations keeps one permanently unrendered and nothing would report it);
//   · a SETTLED browser costs ZERO viewport work — that is the whole point of `OnDemand`;
//   · invalidating one material re-renders EXACTLY the viewports that declared a dependency on it;
//   · deferred viewports are REPORTED and stay dirty — a silent cap reads as "everything rendered";
//   · composing N viewports yields ONE graph with N namespaced includes and AT MOST ONE presenter.

#include <crd/framecook/viewport.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace fc = crd::framecook;

namespace
{
void set_s(crd::containers::String& d, const char* s)
{
    d.clear();
    d.append(s);
}

// "thumb.<n>" without std::string — the repo bans owning STL containers, tests included.
void thumb_id(crd::containers::String& d, crd::u32 n)
{
    d.clear();
    d.append("thumb.");
    char buf[8];
    int  i = 0;
    if (n == 0U) { buf[i++] = '0'; }
    while (n > 0U)
    {
        buf[i++] = static_cast<char>('0' + (n % 10U));
        n /= 10U;
    }
    while (i-- > 0)
    {
        const char one[2] = {buf[i], '\0'};
        d.append(static_cast<const char*>(one));
    }
}

[[nodiscard]] bool contains(const crd::containers::Array<crd::u32>& v, crd::u32 x)
{
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        if (v[i] == x) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("REN-37.9 GATE: an editor-shaped viewport set schedules within budget and never starves", "[framecook][ren37]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    fc::ViewportRegistry       reg(&alloc);

    // the main viewport — EveryFrame, and expensive
    fc::ViewportDesc main_vp(&alloc);
    set_s(main_vp.id, "main");
    set_s(main_vp.graph, "crd://frame/forward_csm");
    main_vp.policy  = fc::ViewportPolicy::EveryFrame;
    main_vp.present = true;
    main_vp.width   = 1920;
    main_vp.height  = 1080;
    const crd::u32 main_i = reg.add(main_vp);
    reg.note_cost(main_i, 2.0);

    // a live animation preview — also EveryFrame, cheaper
    fc::ViewportDesc anim(&alloc);
    set_s(anim.id, "anim.preview");
    set_s(anim.graph, "crd://frame/forward_lite");
    anim.policy = fc::ViewportPolicy::EveryFrame;
    anim.width  = 320;
    anim.height = 240;
    const crd::u32 anim_i = reg.add(anim);
    reg.note_cost(anim_i, 0.2);

    // 16 file-browser thumbnails — OnDemand, each showing one asset
    constexpr crd::u32 kThumbs = 16U;
    crd::u32           thumb_first = 0U;
    for (crd::u32 t = 0; t < kThumbs; ++t)
    {
        fc::ViewportDesc d(&alloc);
        thumb_id(d.id, t);
        set_s(d.graph, "crd://frame/thumbnail"); // ⛔ a DIFFERENT authored graph, not the main one dimmed down
        d.policy   = fc::ViewportPolicy::OnDemand;
        d.width    = 256;
        d.height   = 256;
        d.readback = true; // a thumbnail CAPTURE pays the stall once; the live viewports never do
        const crd::u32 i = reg.add(d);
        if (t == 0U) { thumb_first = i; }
        reg.note_cost(i, 0.1);
        reg.depends_on(i, fc::DependencyKind::Asset, 1000ULL + t);
        reg.depends_on(i, fc::DependencyKind::Material, 500ULL); // they all share one material
    }
    REQUIRE(reg.count() == kThumbs + 2U);

    // A budget that admits the two live viewports (2.2 ms) plus a HANDFUL of thumbnails.
    // ⛔ The exact number is deliberately NOT asserted anywhere below. `charged_ms` accumulates in floating point,
    // so `charged + cost <= max` is knife-edge: 2.2 + 0.1x4 lands either side of 2.6 depending on rounding. A test
    // that pinned "exactly 4 per frame" would be asserting IEEE-754 accumulation order, not the scheduler. What
    // the scheduler actually promises is asserted instead: the live viewports always run, every dirty viewport
    // renders in BOUNDED time, a settled set costs nothing, and nothing is ever silently dropped.
    fc::ViewportBudget budget;
    budget.max_viewports = 6;
    budget.max_gpu_ms    = 2.6;
    budget.max_pixels    = 1ULL << 30U; // not the binding constraint here

    fc::ViewportSelection sel(&alloc);

    // ── frame 0: the two live viewports plus as many thumbnails as fit; the rest are DEFERRED and REPORTED.
    fc::select_viewports(reg, budget, 0U, sel);
    // ⛔ the live viewports are admitted FIRST and unconditionally — a thumbnail can never displace them
    CHECK(contains(sel.active, main_i));
    CHECK(contains(sel.active, anim_i));
    CHECK(sel.active.size() > 2U);                       // ...and some thumbnails DID fit
    CHECK(sel.active.size() <= budget.max_viewports);    // ...within the declared cap
    CHECK(sel.charged_ms <= budget.max_gpu_ms);          // ...and within the measured-time budget
    CHECK(sel.deferred.size() == kThumbs - (sel.active.size() - 2U)); // everything else is REPORTED, not dropped
    fc::commit_selection(reg, sel, 0U);

    // ── the drain. ⭐ THE PROPERTY THAT MATTERS: every dirty thumbnail renders within a BOUNDED number of frames,
    // because each deferred frame raises a viewport's effective priority. Without the ageing term a steady stream
    // of higher-priority work would keep one permanently unrendered and nothing in the system would report it.
    // The bound is generous on purpose — it is a starvation check, not a throughput assertion.
    crd::u32 frame = 1U;
    for (; frame < 32U; ++frame)
    {
        fc::select_viewports(reg, budget, frame, sel);
        CHECK(contains(sel.active, main_i)); // never starved, on any frame
        fc::commit_selection(reg, sel, frame);
        if (sel.active.size() == 2U && sel.deferred.empty()) { break; }
    }
    CHECK(frame < 32U);
    for (crd::u32 t = 0; t < kThumbs; ++t) { CHECK_FALSE(reg.at(thumb_first + t).dirty); }

    // ── SETTLED: only the two live viewports run; the thumbnails contribute NOTHING. That is the property that
    // makes a 400-asset folder viable at all.
    fc::select_viewports(reg, budget, ++frame, sel);
    CHECK(sel.active.size() == 2U);
    CHECK(sel.deferred.size() == 0U);
    fc::commit_selection(reg, sel, frame);

    // ── invalidating ONE asset re-renders EXACTLY that thumbnail, and nothing else.
    CHECK(reg.invalidate(fc::DependencyKind::Asset, 1000ULL + 7ULL) == 1U);
    fc::select_viewports(reg, budget, ++frame, sel);
    CHECK(sel.active.size() == 3U);
    CHECK(contains(sel.active, thumb_first + 7U));
    fc::commit_selection(reg, sel, frame);

    // ── invalidating the SHARED MATERIAL dirties all 16 — and they drain over several frames rather than
    // blowing the frame budget in one.
    CHECK(reg.invalidate(fc::DependencyKind::Material, 500ULL) == kThumbs);
    fc::select_viewports(reg, budget, ++frame, sel);
    CHECK(sel.active.size() > 2U);
    CHECK(sel.active.size() <= budget.max_viewports);
    CHECK(sel.deferred.size() == kThumbs - (sel.active.size() - 2U));
    // ⛔ deferred viewports stay DIRTY. Marking them clean here is the silent-cap failure in its purest form.
    for (crd::usize i = 0; i < sel.deferred.size(); ++i) { CHECK(reg.at(sel.deferred[i]).dirty); }
    fc::commit_selection(reg, sel, frame);
    for (crd::usize i = 0; i < sel.active.size(); ++i) { CHECK_FALSE(reg.at(sel.active[i]).dirty); }
}

TEST_CASE("REN-37.9: an UNMEASURED viewport is charged pessimistically", "[framecook][ren37]")
{
    // ⛔ A viewport with no REN-8 measurement yet must not be charged zero — a first-time expensive viewport
    // would then be admitted alongside everything else and blow the frame. Pessimism is the safe direction.
    crd::memory::TlsfAllocator alloc(1U << 20U);
    fc::ViewportRegistry       reg(&alloc);
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        fc::ViewportDesc d(&alloc);
        thumb_id(d.id, i);
        set_s(d.graph, "crd://frame/thumbnail");
        d.policy = fc::ViewportPolicy::OnDemand;
        (void)reg.add(d); // deliberately no note_cost
    }
    fc::ViewportBudget budget;
    budget.max_gpu_ms         = 2.0;
    budget.unmeasured_cost_ms = 1.0;
    budget.max_viewports      = 16;

    fc::ViewportSelection sel(&alloc);
    fc::select_viewports(reg, budget, 0U, sel);
    CHECK(sel.active.size() == 2U);   // 2 x 1.0 ms fills a 2.0 ms budget
    CHECK(sel.deferred.size() == 2U);
    CHECK(sel.charged_ms == 2.0);
}

TEST_CASE("REN-37.8 GATE: N viewports compose into ONE graph with N namespaced includes", "[framecook][ren37]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    fc::ViewportRegistry       reg(&alloc);
    crd::containers::String    where(&alloc);

    fc::ViewportDesc a(&alloc);
    set_s(a.id, "main");
    set_s(a.graph, "crd://frame/forward_csm");
    a.present = true;
    const crd::u32 ia = reg.add(a);

    fc::ViewportDesc b(&alloc);
    set_s(b.id, "thumb.0");
    set_s(b.graph, "crd://frame/thumbnail");
    const crd::u32 ib = reg.add(b);

    fc::ViewportDesc c(&alloc);
    set_s(c.id, "thumb.1");
    set_s(c.graph, "crd://frame/thumbnail"); // the SAME graph as thumb.0 — the collision case
    const crd::u32 ic = reg.add(c);

    const crd::u32          active[3] = {ia, ib, ic};
    crd::containers::String shared(&alloc);
    shared.append("crd://frame/shared_shadows");

    fc::FrameGraphDesc frame(&alloc);
    REQUIRE(fc::compose_frame(reg, crd::containers::ConstSpan<crd::u32>(static_cast<const crd::u32*>(active), 3U),
                              &shared, frame, &where)
            == fc::FrameCookError::Ok);

    // ONE graph: the shared producer + one include per viewport.
    REQUIRE(frame.includes.size() == 4U);
    // ⭐ two instances of the SAME thumbnail graph, distinguished ONLY by their namespace. Without that they
    // would both declare the same resources and render into each other — which no validator would flag.
    CHECK(frame.includes[2].as.size() == 7U); // "thumb.0"
    CHECK(frame.includes[3].as.size() == 7U); // "thumb.1"
    bool distinct = frame.includes[2].as.size() != frame.includes[3].as.size();
    for (crd::usize i = 0; i < frame.includes[2].as.size() && !distinct; ++i)
    {
        distinct = frame.includes[2].as.c_str()[i] != frame.includes[3].as.c_str()[i];
    }
    CHECK(distinct);

    // the PRESENTING viewport binds `@output` to the frame's output; the others bind to their own.
    REQUIRE(frame.includes[1].bind.size() == 1U);
    CHECK(frame.includes[1].bind[0].to.size() == 7U); // "@output" — the presenter
    REQUIRE(frame.includes[2].bind.size() == 1U);
    CHECK(frame.includes[2].bind[0].to.size() > 7U);  // "@vp.thumb.0" — its own target, never the swapchain

    // ⛔ TWO presenters would each blit over the other and the winner would depend on declaration order — a bug
    // that looks like flicker and points nowhere. Rejected.
    {
        fc::ViewportDesc d(&alloc);
        set_s(d.id, "second_present");
        set_s(d.graph, "crd://frame/forward_csm");
        d.present            = true;
        const crd::u32 id2   = reg.add(d);
        const crd::u32 two[2] = {ia, id2};
        fc::FrameGraphDesc bad(&alloc);
        CHECK(fc::compose_frame(reg, crd::containers::ConstSpan<crd::u32>(static_cast<const crd::u32*>(two), 2U),
                                nullptr, bad, &where)
              != fc::FrameCookError::Ok);
    }

    // ...and ZERO presenters is legal — an offscreen frame (a thumbnail bake, a test) is not an error.
    {
        const crd::u32     off[2] = {ib, ic};
        fc::FrameGraphDesc offscreen(&alloc);
        CHECK(fc::compose_frame(reg, crd::containers::ConstSpan<crd::u32>(static_cast<const crd::u32*>(off), 2U),
                                nullptr, offscreen, &where)
              == fc::FrameCookError::Ok);
    }
}
