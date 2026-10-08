#include "replay_needs.hpp"

#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp> // OpInfo
#include <crd/ceir/effect.hpp>
#include <crd/ceir/input.hpp> // reads_input
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>

namespace crd::ceir::cook::detail
{
namespace
{
namespace cont = crd::containers;

// The walk checks the caller's cancel flag once per this many ops.
constexpr crd::u32 kCancelStride = 256U;

[[nodiscard]] constexpr crd::u64 bits(EffectFamily f) noexcept
{
    return effect_family_bit(f);
}

constexpr ReplayInputSpec kInputs[kReplayInputCount] = {
    {"program", "identity", 0U, "the program's content hash", ""},
    {"build", "identity", 0U, "the build and configuration a run used",
     "a program file names no build; replay.record writes the build a run used into a run record"},
    {"entry-arguments", "event", 0U, "the entry arguments and initial state",
     "a program file holds no run's entry arguments; replay.record writes them into a run record"},
    {"random", "event", bits(EffectFamily::RandomRead), "random streams",
     "the random draws are not held; replay.record keeps every input.random draw"},
    {"clock", "event", bits(EffectFamily::TimeRead), "clock and time-step inputs",
     "nothing records the clock and time-step inputs read"},
    {"host-state", "event",
     bits(EffectFamily::HostStateRead) | bits(EffectFamily::SceneRead) | bits(EffectFamily::EcsRead) |
         bits(EffectFamily::PhysicsRead) | bits(EffectFamily::AudioRead) | bits(EffectFamily::DocumentRead) |
         bits(EffectFamily::ConstraintRead) | bits(EffectFamily::UIRead),
     "host and world state reads", "nothing records the host and world state read"},
    {"external-results", "event",
     bits(EffectFamily::FileIO) | bits(EffectFamily::NetworkIO) | bits(EffectFamily::DeviceIO) |
         bits(EffectFamily::ExternalCall) | bits(EffectFamily::AgentAction),
     "external I/O completions and opaque call results",
     "nothing records the external I/O completions and opaque call results"},
    {"schedule", "schedule", bits(EffectFamily::Synchronization) | bits(EffectFamily::Nondeterministic),
     "schedule choices", "nothing records the schedule choices a run made"},
    {"device-tolerance", "numeric", bits(EffectFamily::GPUCommand), "a tolerance or oracle for device numerics",
     "nothing declares a tolerance or oracle for backend-specific device numerics"},
};

void tally_op(const Context& ctx, const Operation& op, const EffectQuery& query,
              cont::HashMap<const Operation*, crd::u8>& visited, ProgramNeeds& tally)
{
    // Each op's own effective effects. The cycle guard is per op, so a second call to one callee counts again.
    visited.clear();
    crd::u64 mask = 0U;
    ctx.collect_effective_mask(op, query, mask);

    const OpInfo* const info = ctx.op_info(op.kind());
    ++tally.ops;
    if (info == nullptr)
    {
        ++tally.unregistered;
    }
    else
    {
        const DeterminismClass claim = ctx.op_determinism(op.kind());
        if (claim == DeterminismClass::Unspecified)
        {
            ++tally.unclaimed;
        }
        else if (tally.weakest == DeterminismClass::Unspecified ||
                 determinism_rank(claim) < determinism_rank(tally.weakest))
        {
            tally.weakest = claim;
        }
    }
    // DIAG.9a: what the op declares itself (a call's callee effects are not its own), for the reads no seam sees.
    crd::u64 own = 0U;
    for (const EffectRecord& e : ctx.op_effects(op.kind()))
    {
        own |= bits(e.family);
    }
    const bool through_seam = input::reads_input(ctx, op.kind());
    if ((mask & bits(EffectFamily::ExternalCall)) != 0U)
    {
        ++tally.opaque;
        if (tally.first_opaque == nullptr)
        {
            tally.first_opaque = &op;
        }
    }
    for (crd::u32 i = 0U; i < kReplayInputCount; ++i)
    {
        if ((mask & kInputs[i].families) != 0U)
        {
            InputNeed& t = tally.inputs[i];
            ++t.ops;
            if (t.cause == nullptr)
            {
                t.cause = &op;
            }
            if (!through_seam && (own & kInputs[i].families) != 0U)
            {
                ++t.uncaptured;
            }
        }
    }
}
} // namespace

const ReplayInputSpec& replay_input(crd::u32 index) noexcept
{
    return kInputs[index < kReplayInputCount ? index : 0U];
}

cont::StringView need_name(Need n) noexcept
{
    switch (n) // no default: every need is named
    {
    case Need::No: return cont::StringView{"no"};
    case Need::Yes: return cont::StringView{"yes"};
    case Need::Unknown: return cont::StringView{"unknown"};
    }
    return cont::StringView{"?"};
}

cont::StringView claim_name(DeterminismClass c) noexcept
{
    switch (c) // no default: every class is named
    {
    case DeterminismClass::Unspecified: return cont::StringView{"none"};
    case DeterminismClass::BitExact: return cont::StringView{"bit-exact"};
    case DeterminismClass::DeterministicWithinTarget: return cont::StringView{"within-target"};
    case DeterminismClass::DeterministicWithinBackend: return cont::StringView{"within-backend"};
    case DeterminismClass::Nondeterministic: return cont::StringView{"nondeterministic"};
    case DeterminismClass::ExternalNondeterminism: return cont::StringView{"external-nondeterminism"};
    }
    return cont::StringView{"?"};
}

bool analyze_needs(const Context& ctx, const Module& module, memory::IAllocator* alloc,
                   const std::atomic<bool>* cancel, ProgramNeeds& out)
{
    out = ProgramNeeds{};

    // Callees resolve through the module's own symbol table, so a call is charged with what its callee does.
    cont::HashMap<const Operation*, crd::u8> visited(alloc);
    const EffectQuery                        query{module.symbols(), &visited};

    // Pre-order over every region, iteratively, so the first op needing an input is the first a reader meets.
    cont::Array<const Operation*> stack(alloc);
    cont::Array<const Operation*> children(alloc);
    const auto                    push_children = [&](const Region* region)
    {
        children.clear();
        for (const Block* b = region->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
            {
                children.push_back(op);
            }
        }
        for (crd::usize i = children.size(); i > 0U; --i)
        {
            stack.push_back(children[i - 1U]);
        }
    };
    if (module.body() != nullptr)
    {
        push_children(module.body());
    }
    while (!stack.empty())
    {
        const Operation* const op = stack.back();
        stack.pop_back();
        if (out.ops % kCancelStride == 0U && cancel != nullptr && cancel->load(std::memory_order_acquire))
        {
            return false;
        }
        tally_op(ctx, *op, query, visited, out);
        for (crd::u32 r = op->num_regions(); r > 0U; --r)
        {
            const Region* const region = op->region(r - 1U);
            if (region != nullptr)
            {
                push_children(region);
            }
        }
    }

    // Resolve each input's need: an effect-derived input no registered op needs is unknown when an opaque op exists.
    for (crd::u32 i = 0U; i < kReplayInputCount; ++i)
    {
        InputNeed& t = out.inputs[i];
        t.need       = Need::Yes;
        if (kInputs[i].families != 0U && t.ops == 0U)
        {
            t.need  = out.opaque != 0U ? Need::Unknown : Need::No;
            t.cause = out.first_opaque;
        }
    }
    return true;
}

void record_inputs(const ProgramNeeds& needs, ReplayExecutorKind executor, bool host_inputs_held,
                   ReplayInput (&inputs)[kReplayInputCount], containers::String* missing)
{
    const bool host = executor == ReplayExecutorKind::Host;
    for (crd::u32 i = 0U; i < kReplayInputCount; ++i)
    {
        ReplayInput& in = inputs[i];
        in.need         = needs.inputs[i].need;
        if (in.need == ReplayNeed::No)
        {
            in.state = ReplayInputState::NotNeeded;
        }
        else if (i == kReplayProgramInput || i == kReplayBuildInput || i == kReplayArgumentsInput ||
                 (host && i == kReplayScheduleInput) ||
                 (i == kReplayRandomInput && in.need == ReplayNeed::Yes && host_inputs_held &&
                  needs.inputs[i].uncaptured == 0U))
        {
            in.state = ReplayInputState::Recorded;
        }
        else
        {
            // Nothing captures this input at the executor's boundary: stored missing, never assumed.
            in.state = ReplayInputState::Missing;
            if (missing != nullptr)
            {
                if (!missing->empty())
                {
                    missing->push_back(',');
                }
                missing->append(replay_input_name(i));
            }
        }
    }
}
} // namespace crd::ceir::cook::detail

namespace crd::ceir::cook
{
bool classify_replay_inputs(const Context& ctx, const Module& module, ReplayExecutorKind executor,
                            bool host_inputs_held, memory::IAllocator* alloc, const std::atomic<bool>* cancel,
                            ReplayInput (&inputs)[kReplayInputs], containers::String* missing)
{
    detail::ProgramNeeds needs;
    if (!detail::analyze_needs(ctx, module, alloc, cancel, needs))
    {
        return false;
    }
    detail::record_inputs(needs, executor, host_inputs_held, inputs, missing);
    return true;
}
} // namespace crd::ceir::cook
