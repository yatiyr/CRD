#include <crd/ceir/cook/replay_diag.hpp>

#include "program_load.hpp"

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp> // OpInfo
#include <crd/ceir/effect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/semantics.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;

using crd::perf::DiagCall;
using crd::perf::DiagFields;
using crd::perf::DiagSnapshot;
using crd::perf::DiagStatus;

// The walk checks the caller's cancel flag once per this many ops.
constexpr crd::u32 kCancelStride = 256U;

[[nodiscard]] constexpr crd::u64 bits(EffectFamily f) noexcept
{
    return effect_family_bit(f);
}

// One replay input. `families` empty: the input is needed by every run, whatever its ops do.
struct InputSpec
{
    cont::StringView name;
    cont::StringView guarantee;
    crd::u64         families = 0U;
    cont::StringView what;    // the evidence, for the reasons
    cont::StringView missing; // why it is missing when needed
};

constexpr crd::u32 kInputCount = 9U;

constexpr InputSpec kInputs[kInputCount] = {
    {"program", "identity", 0U, "the program's content hash", ""},
    {"build", "identity", 0U, "the build and configuration a run used",
     "a program file names no build, and nothing records the build and configuration a run used"},
    {"entry-arguments", "event", 0U, "the entry arguments and initial state",
     "nothing records the entry arguments and initial state a run was given"},
    {"random", "event", bits(EffectFamily::RandomRead), "random streams", "nothing records the random streams drawn"},
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

constexpr crd::u32 kProgramInput = 0U;

// NOLINTNEXTLINE(performance-enum-size)
enum class Need : crd::u8
{
    No,
    Yes,
    Unknown,
};

[[nodiscard]] cont::StringView need_name(Need n) noexcept
{
    switch (n) // no default: every need is named
    {
    case Need::No: return cont::StringView{"no"};
    case Need::Yes: return cont::StringView{"yes"};
    case Need::Unknown: return cont::StringView{"unknown"};
    }
    return cont::StringView{"?"};
}

[[nodiscard]] cont::StringView claim_name(DeterminismClass c) noexcept
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

// What the walk found for one input.
struct InputTally
{
    crd::u64         ops   = 0U;      // ops whose effective effects need it
    const Operation* first = nullptr; // the first of them in pre-order
};

// What the walk found for the program.
struct Tally
{
    crd::u64         ops          = 0U;
    crd::u64         unregistered = 0U;
    crd::u64         opaque       = 0U; // unregistered, or effective effects including ExternalCall
    const Operation* first_opaque = nullptr;
    crd::u64         unclaimed    = 0U; // registered ops that make no determinism claim
    DeterminismClass weakest      = DeterminismClass::Unspecified; // weakest claim made (Unspecified: none made)
    InputTally       inputs[kInputCount];
};

void tally_op(const Context& ctx, const Operation& op, const EffectQuery& query,
              cont::HashMap<const Operation*, crd::u8>& visited, Tally& tally)
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
    if ((mask & bits(EffectFamily::ExternalCall)) != 0U)
    {
        ++tally.opaque;
        if (tally.first_opaque == nullptr)
        {
            tally.first_opaque = &op;
        }
    }
    for (crd::u32 i = 0U; i < kInputCount; ++i)
    {
        if ((mask & kInputs[i].families) != 0U)
        {
            InputTally& t = tally.inputs[i];
            ++t.ops;
            if (t.first == nullptr)
            {
                t.first = &op;
            }
        }
    }
}

// The op an item blames, at its closest-known authored position.
void blame(const Context& ctx, const Operation* op, cont::Array<Origin>& storage, DiagFields& item)
{
    const Provenance p  = resolve_provenance(ctx, op, storage);
    const Origin*    at = op != nullptr ? p.primary() : nullptr;
    item.u64("op", op != nullptr ? p.op.value : 0U)
        .str("op_name", op != nullptr ? ctx.op_name(op->kind()) : cont::StringView{})
        .str("file", at != nullptr ? ctx.file_path(at->loc.file_id) : cont::StringView{})
        .u64("line", at != nullptr ? at->loc.line : 0U)
        .u64("col", at != nullptr ? at->loc.col : 0U);
}

DiagStatus run_replay_prepare(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    command = static_cast<ReplayPrepareCommand*>(context);
    crd::memory::IAllocator* const alloc   = out.allocator();
    command->runs.fetch_add(1U, std::memory_order_relaxed);

    cont::Array<crd::u8>  bytes(alloc);
    Context               ctx(alloc);
    detail::LoadedProgram loaded;
    const DiagStatus load = detail::load_program(call, command->max_program_bytes, command->registrar, command->user,
                                                 ctx, bytes, loaded, out.reason, command->bytes_read);
    if (load != DiagStatus::Ok)
    {
        return load;
    }
    const Module* const module = loaded.module;

    // Callees resolve through the module's own symbol table, so a call is charged with what its callee does.
    cont::HashMap<const Operation*, crd::u8> visited(alloc);
    const EffectQuery                        query{module->symbols(), &visited};

    // Pre-order over every region, iteratively, so the first op needing an input is the first a reader meets.
    cont::Array<const Operation*> stack(alloc);
    cont::Array<const Operation*> children(alloc);
    Tally                         tally;
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
    push_children(module->body());
    while (!stack.empty())
    {
        const Operation* const op = stack.back();
        stack.pop_back();
        if (tally.ops % kCancelStride == 0U && call.cancelled())
        {
            out.reason.append("cancelled while classifying the program's ops");
            return DiagStatus::Cancelled;
        }
        tally_op(ctx, *op, query, visited, tally);
        for (crd::u32 r = op->num_regions(); r > 0U; --r)
        {
            const Region* const region = op->region(r - 1U);
            if (region != nullptr)
            {
                push_children(region);
            }
        }
    }

    cont::Array<Origin> storage(alloc);
    DiagFields          item(alloc);
    cont::String        reason(alloc);
    cont::String        missing_names(alloc);
    crd::u64            needed  = 0U;
    crd::u64            unknown = 0U;
    crd::u64            missing = 0U;
    for (crd::u32 i = 0U; i < kInputCount; ++i)
    {
        const InputSpec&  spec = kInputs[i];
        const InputTally& t    = tally.inputs[i];

        Need             need  = Need::Yes;
        const Operation* cause = t.first;
        reason.clear();
        if (spec.families != 0U && t.ops == 0U)
        {
            need  = tally.opaque != 0U ? Need::Unknown : Need::No;
            cause = tally.first_opaque;
        }
        const bool available = i == kProgramInput;
        cont::StringView state{"missing"};
        if (need == Need::No)
        {
            state = cont::StringView{"not-needed"};
            reason.append("no op needs ");
            reason.append(spec.what);
        }
        else if (available)
        {
            state = cont::StringView{"available"};
            reason.append(spec.what);
            reason.append(" is computed from the file");
        }
        else
        {
            if (need == Need::Unknown)
            {
                reason.append("an opaque op may need ");
                reason.append(spec.what);
                reason.append("; ");
            }
            reason.append(spec.missing);
            ++missing;
            if (!missing_names.empty())
            {
                missing_names.push_back(',');
            }
            missing_names.append(spec.name);
        }
        needed += need == Need::Yes ? 1U : 0U;
        unknown += need == Need::Unknown ? 1U : 0U;

        item.clear();
        item.str("kind", "input")
            .str("input", spec.name)
            .str("guarantee", spec.guarantee)
            .str("needed", need_name(need))
            .str("state", state)
            .u64("ops", t.ops);
        blame(ctx, need == Need::No ? nullptr : cause, storage, item);
        item.str("reason", cont::StringView{reason.data(), reason.size()});
        (void)out.add_item(item);
    }

    out.summary.str("path", call.request->path)
        .str("form", detail::program_form_name(loaded.form))
        .u64("bytes", bytes.size())
        .u64("content_hash", stable_hash(ctx, *module, alloc))
        .u64("recorded_hash", loaded.recorded_hash)
        .u64("ops", tally.ops)
        .u64("unregistered", tally.unregistered)
        .u64("opaque", tally.opaque)
        .str("weakest_claim", claim_name(tally.weakest))
        .u64("unclaimed", tally.unclaimed)
        .u64("needed", needed)
        .u64("unknown", unknown)
        .u64("missing", missing)
        .str("replay", missing == 0U ? cont::StringView{"prepared"} : cont::StringView{"unavailable"})
        .str("missing_inputs", cont::StringView{missing_names.data(), missing_names.size()});
    return DiagStatus::Ok;
}
} // namespace

bool register_replay_prepare(perf::DiagCommandService& service, ReplayPrepareCommand& command)
{
    perf::DiagCommandSpec spec;
    spec.name       = kReplayPrepareCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"what replaying an authored program needs, and why each input is missing"};
    spec.authority  = perf::DiagAuthority::Read;
    spec.takes_path = true;
    return service.register_command(spec, &run_replay_prepare, &command);
}
} // namespace crd::ceir::cook
