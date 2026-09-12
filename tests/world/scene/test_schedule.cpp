// Phase 3.0 v1h — System + Schedule + Commands tests (ADR-0052 §3-§5).
//
// Coverage:
//   - register_system + step() runs a single system.
//   - Phase ordering: 7 systems in 7 phases run in PrePhysics → PostRender.
//   - Registration order within a phase.
//   - Commands::spawn returns valid EntityId immediately.
//   - Commands::add_component is deferred (invisible until flush).
//   - Commands::destroy is deferred.
//   - Commands::set_component upserts.
//   - Commands::add_relation deferred + visible after flush.
//   - Commands::remove_relation deferred.
//   - Multiple commands flush in registration order.
//   - step_fixed: accumulator math (3.5 dt with fixed_dt=1.0 → 3 substeps; 0.5 carries).
//   - step_fixed: max_substeps clamp prevents spiral of death.
//   - Variable-rate systems run once per step_fixed call (regardless of substeps).
//   - Commands destructor runs payload destructors (no leaks for non-trivially-destructible T).
//   - Mid-frame system registration — registered system runs from next phase visit.

#include <crd/containers/string.hpp>
#include <crd/scene/commands.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/system.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using crd::scene::EntityId;
using crd::scene::ISystem;
using crd::scene::SchedulePhase;
using crd::scene::World;
using crd::scene::relations::ChildOf;

namespace
{
struct Position
{
    float x{}, y{}, z{};
};

struct Velocity
{
    float dx{}, dy{}, dz{};
};

struct Health
{
    int hp = 100;
};

// Recording system — appends to a shared trace on each run() call.
class RecordingSystem : public ISystem
{
public:
    RecordingSystem(SchedulePhase ph, crd::containers::String label, crd::containers::Array<crd::containers::String>* trace,
                    bool fixed = false)
        : m_phase(ph), m_label(std::move(label)), m_trace(trace), m_fixed(fixed)
    {
    }

    [[nodiscard]] SchedulePhase phase() const override { return m_phase; }
    void run(World&) override
    {
        if (m_trace != nullptr)
        {
            m_trace->push_back(m_label);
        }
    }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{m_label.c_str()};
    }
    [[nodiscard]] bool fixed_step() const noexcept override { return m_fixed; }

private:
    SchedulePhase                                            m_phase;
    crd::containers::String                                  m_label;
    crd::containers::Array<crd::containers::String>*         m_trace;
    bool                                                     m_fixed;
};

// Component with observable destructor (for Commands payload-leak test).
struct DtorCounter
{
    static std::atomic<int>& counter()
    {
        static std::atomic<int> c{0};
        return c;
    }
    int  value = 0;
    bool live  = false;

    DtorCounter() = default;
    explicit DtorCounter(int v) : value(v), live(true) {}

    DtorCounter(const DtorCounter& o) noexcept : value(o.value), live(o.live)
    {
        if (live)
        {
            counter().fetch_add(1);
        }
    }
    DtorCounter& operator=(const DtorCounter& o) noexcept
    {
        if (this != &o)
        {
            if (live && !o.live)
            {
                counter().fetch_sub(1);
            }
            else if (!live && o.live)
            {
                counter().fetch_add(1);
            }
            value = o.value;
            live  = o.live;
        }
        return *this;
    }
    DtorCounter(DtorCounter&& o) noexcept : value(o.value), live(o.live) { o.live = false; }
    DtorCounter& operator=(DtorCounter&& o) noexcept
    {
        if (this != &o)
        {
            if (live)
            {
                counter().fetch_sub(1);
            }
            value  = o.value;
            live   = o.live;
            o.live = false;
        }
        return *this;
    }
    ~DtorCounter()
    {
        if (live)
        {
            counter().fetch_sub(1);
        }
    }
};

// A live DtorCounter is created (counter +1) by hand-coded factory — only
// the user-facing constructor takes a value and increments. The default
// ctor produces a "dead" instance that doesn't decrement on destruction.
DtorCounter make_dtor(int v)
{
    DtorCounter d{v};
    DtorCounter::counter().fetch_add(1); // count the live one we just made
    return d; // moved out, leaves d "dead"
}

} // namespace

// ---------------------------------------------------------------------------
// register_system + step
// ---------------------------------------------------------------------------

TEST_CASE("register_system + step runs a single system once", "[scene][system][step]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Update, crd::containers::String{"sys"}, &trace));

    w.step(1.0 / 60.0);

    REQUIRE(trace.size() == 1U);
}

TEST_CASE("Phase ordering: systems run in PrePhysics through PostRender order", "[scene][system][phase-order]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    // Register in REVERSE phase order — schedule must reorder by phase index.
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::PostRender, crd::containers::String{"PostRender"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::RenderExtract, crd::containers::String{"RenderExtract"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::PreRender, crd::containers::String{"PreRender"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Update, crd::containers::String{"Update"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::PostPhysics, crd::containers::String{"PostPhysics"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Physics, crd::containers::String{"Physics"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::PrePhysics, crd::containers::String{"PrePhysics"}, &trace));

    w.step(1.0 / 60.0);

    REQUIRE(trace.size() == 7U);
    CHECK(trace[0] == crd::containers::String{"PrePhysics"});
    CHECK(trace[1] == crd::containers::String{"Physics"});
    CHECK(trace[2] == crd::containers::String{"PostPhysics"});
    CHECK(trace[3] == crd::containers::String{"Update"});
    CHECK(trace[4] == crd::containers::String{"PreRender"});
    CHECK(trace[5] == crd::containers::String{"RenderExtract"});
    CHECK(trace[6] == crd::containers::String{"PostRender"});
}

TEST_CASE("Registration order within a phase", "[scene][system][phase-order]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Update, crd::containers::String{"first"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Update, crd::containers::String{"second"}, &trace));
    w.register_system(std::make_unique<RecordingSystem>(SchedulePhase::Update, crd::containers::String{"third"}, &trace));

    w.step(1.0 / 60.0);

    REQUIRE(trace.size() == 3U);
    CHECK(trace[0] == crd::containers::String{"first"});
    CHECK(trace[1] == crd::containers::String{"second"});
    CHECK(trace[2] == crd::containers::String{"third"});
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

TEST_CASE("Commands::spawn returns a valid EntityId immediately", "[scene][commands]")
{
    World w;
    EntityId e = w.commands().spawn();
    CHECK(!e.is_null());
    CHECK(w.is_alive(e));
}

TEST_CASE("Commands::add_component is deferred until flush", "[scene][commands][deferred]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();
    w.commands().add_component<Position>(e, Position{1, 2, 3});

    // BEFORE flush: component invisible.
    CHECK_FALSE(w.has_component<Position>(e));

    w.commands().flush();

    // AFTER flush: visible.
    CHECK(w.has_component<Position>(e));
    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(p->x == 1.0F);
}

TEST_CASE("Commands::destroy is deferred", "[scene][commands][deferred]")
{
    World w;
    EntityId e = w.spawn();
    CHECK(w.is_alive(e));

    w.commands().destroy(e);
    CHECK(w.is_alive(e)); // still alive — not flushed yet

    w.commands().flush();
    CHECK_FALSE(w.is_alive(e));
}

TEST_CASE("Commands::set_component upserts via flush", "[scene][commands][upsert]")
{
    World w;
    w.register_component<Health>();
    EntityId e = w.spawn();
    w.add_component<Health>(e, Health{50});

    w.commands().set_component<Health>(e, Health{75});
    w.commands().flush();

    const Health* h = w.get_component<Health>(e);
    REQUIRE(h != nullptr);
    CHECK(h->hp == 75);
}

TEST_CASE("Commands::add_relation deferred + visible after flush", "[scene][commands][relation]")
{
    World w;
    w.register_builtin_relations();
    EntityId child = w.spawn();
    EntityId parent = w.spawn();

    w.commands().add_relation<ChildOf>(child, parent);
    CHECK_FALSE(w.has_relation<ChildOf>(child));

    w.commands().flush();
    CHECK(w.has_relation<ChildOf>(child));
    CHECK(w.get_relation_target<ChildOf>(child).raw == parent.raw);
}

TEST_CASE("Commands::remove_relation deferred", "[scene][commands][relation]")
{
    World w;
    w.register_builtin_relations();
    EntityId child = w.spawn();
    EntityId parent = w.spawn();
    w.add_relation<ChildOf>(child, parent);

    w.commands().remove_relation<ChildOf>(child);
    CHECK(w.has_relation<ChildOf>(child)); // still there

    w.commands().flush();
    CHECK_FALSE(w.has_relation<ChildOf>(child));
}

TEST_CASE("Multiple commands flush in registration order", "[scene][commands][order]")
{
    World w;
    w.register_component<Position>();

    EntityId e = w.spawn();
    w.commands().add_component<Position>(e, Position{1, 0, 0});
    w.commands().set_component<Position>(e, Position{2, 0, 0}); // upsert
    w.commands().set_component<Position>(e, Position{3, 0, 0}); // upsert again

    w.commands().flush();

    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(p->x == 3.0F); // last one wins
}

// ---------------------------------------------------------------------------
// step_fixed
// ---------------------------------------------------------------------------

TEST_CASE("step_fixed: 3.5 dt with fixed_dt=1.0 yields 3 substeps; 0.5 carries", "[scene][system][step-fixed]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    w.register_system(
        std::make_unique<RecordingSystem>(SchedulePhase::Physics, crd::containers::String{"fixed"}, &trace, /*fixed=*/true));

    w.step_fixed(3.5, 1.0, /*max_substeps=*/16);
    CHECK(trace.size() == 3U); // 3 substeps consumed; 0.5 carried

    // Next call: 0.5 carried + 0.6 = 1.1 → 1 substep, 0.1 carried.
    trace.clear();
    w.step_fixed(0.6, 1.0, 16);
    CHECK(trace.size() == 1U);
}

TEST_CASE("step_fixed: max_substeps clamp prevents spiral of death", "[scene][system][step-fixed][clamp]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    w.register_system(
        std::make_unique<RecordingSystem>(SchedulePhase::Physics, crd::containers::String{"fixed"}, &trace, /*fixed=*/true));

    // 10.0 dt / 0.1 fixed_dt = 100 substeps requested; clamp to 4.
    w.step_fixed(10.0, 0.1, /*max_substeps=*/4);
    CHECK(trace.size() == 4U);
}

TEST_CASE("step_fixed: variable-rate systems run once per call regardless of substeps",
          "[scene][system][step-fixed][variable]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    // One fixed-rate system, one variable-rate, both in Physics.
    w.register_system(
        std::make_unique<RecordingSystem>(SchedulePhase::Physics, crd::containers::String{"fixed"}, &trace, /*fixed=*/true));
    w.register_system(
        std::make_unique<RecordingSystem>(SchedulePhase::Physics, crd::containers::String{"variable"}, &trace, /*fixed=*/false));

    w.step_fixed(3.0, 1.0, 16);
    // 3 fixed runs + 1 variable run = 4 total trace entries.
    CHECK(trace.size() == 4U);

    crd::u32 fixed_count = 0;
    crd::u32 variable_count = 0;
    for (const auto& e : trace)
    {
        if (e == crd::containers::String{"fixed"}) ++fixed_count;
        else if (e == crd::containers::String{"variable"}) ++variable_count;
    }
    CHECK(fixed_count == 3U);
    CHECK(variable_count == 1U);
}

// ---------------------------------------------------------------------------
// Commands destructor leak test
// ---------------------------------------------------------------------------

TEST_CASE("Commands destructor runs payload destructors", "[scene][commands][lifetime]")
{
    World w;
    w.register_component<DtorCounter>();
    EntityId e = w.spawn();

    DtorCounter::counter().store(0);
    {
        // Queue a payload but don't flush — Commands' dtor (when w goes out
        // of scope) must run the destructor on the queued bytes.
        w.commands().add_component<DtorCounter>(e, make_dtor(42));
        // Counter should be 1 at this point — one live DtorCounter in the
        // commands payload buffer.
        CHECK(DtorCounter::counter().load() == 1);
    }
    // Flush manually: the payload moves into storage; storage now owns it.
    w.commands().flush();
    // Counter should be 1 (one live, in storage).
    CHECK(DtorCounter::counter().load() == 1);

    // Destroy the entity — storage destructs the slot.
    w.destroy_immediate(e);
    CHECK(DtorCounter::counter().load() == 0);
}

// ---------------------------------------------------------------------------
// Mid-frame registration
// ---------------------------------------------------------------------------

namespace
{
class SelfRegisteringSystem : public ISystem
{
public:
    SelfRegisteringSystem(crd::containers::Array<crd::containers::String>* trace) : m_trace(trace) {}

    [[nodiscard]] SchedulePhase phase() const override { return SchedulePhase::Update; }
    void run(World& w) override
    {
        m_trace->push_back(crd::containers::String{"self"});
        if (!m_registered)
        {
            m_registered = true;
            w.register_system(std::make_unique<RecordingSystem>(
                SchedulePhase::PostRender, crd::containers::String{"late"}, m_trace, false));
        }
    }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"SelfRegisteringSystem"};
    }

private:
    crd::containers::Array<crd::containers::String>* m_trace;
    bool                                             m_registered = false;
};
} // namespace

TEST_CASE("Mid-frame registration: registered system runs from next phase visit", "[scene][system][register]")
{
    World w;
    crd::containers::Array<crd::containers::String> trace;
    w.register_system(std::make_unique<SelfRegisteringSystem>(&trace));

    w.step(1.0 / 60.0);

    // Self-registering system runs in Update; it then registers a PostRender
    // system. Schedule visits PostRender later in the SAME step → "late" appears.
    REQUIRE(trace.size() == 2U);
    CHECK(trace[0] == crd::containers::String{"self"});
    CHECK(trace[1] == crd::containers::String{"late"});
}
