#include <crd/ceir/cook/device_replay.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/type.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp> // fnv1a_64
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>       // ckir_read
#include <crd/kir/ckir_kernel_eval.hpp> // eval_cpu_kernel: the CPU reference

#include <cstring> // memcpy
#include <initializer_list>
#include <utility>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;

constexpr crd::u32 kMaxDispatchBindings = 8U; // gpu::kMaxBindings, the command model's structural cap
constexpr crd::u32 kAccessRead          = 1U;
constexpr crd::u32 kAccessWrite         = 2U;

[[nodiscard]] bool same(const cont::String& a, cont::StringView b) noexcept
{
    return cont::StringView{a.data(), a.size()} == b;
}

void append_decimal(cont::String& out, crd::u64 v)
{
    char       buf[24];
    crd::usize n = 0U;
    do
    {
        buf[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U)
    {
        out.push_back(buf[--n]);
    }
}

// ---- the device program -------------------------------------------------------------------------------------------

// What a device program declares and dispatches, in block order.
struct DeviceProgram
{
    explicit DeviceProgram(memory::IAllocator* a)
        : buffers(a), elements(a), written(a), last_writer(a), declarations(a), dispatches(a)
    {
    }

    cont::Array<const Value*>     buffers;      // the resource.declare results
    cont::Array<DeviceElement>    elements;
    cont::Array<crd::u8>          written;      // 1: a dispatch writes it
    cont::Array<crd::u64>         last_writer;  // the stable id of the last dispatch writing it (0: none)
    cont::Array<crd::u64>         declarations; // the declarations' stable ids
    cont::Array<const Operation*> dispatches;
};

[[nodiscard]] bool element_of(const Context& ctx, TypeId t, DeviceElement& out) noexcept
{
    const Type bt = ctx.type_of(t);
    if (bt.kind != TypeKind::Buffer || bt.count != static_cast<crd::u32>(BufferMode::Plain) || bt.members.size() != 1U)
    {
        return false;
    }
    const Type e = ctx.type_of(bt.members[0]);
    if (e.kind == TypeKind::Float && e.fkind == FloatKind::F32)
    {
        out = DeviceElement::F32;
        return true;
    }
    if (e.kind == TypeKind::Int && e.count == 32U)
    {
        out = e.is_signed ? DeviceElement::I32 : DeviceElement::U32;
        return true;
    }
    return false;
}

// A grid operand's constant value (an arith.const integer), or false.
[[nodiscard]] bool const_grid(const Context& ctx, const Value* v, crd::u64& out) noexcept
{
    const Operation* const def = v->defining_op();
    if (def == nullptr || ctx.op_name(def->kind()) != cont::StringView{"arith.const"})
    {
        return false;
    }
    const AttrValue av = ctx.attr_value(def->attr(cont::StringView{"value"}));
    if (av.kind != AttrKind::Int || av.i <= 0 || av.i > 0xFFFFFFFFLL)
    {
        return false;
    }
    out = static_cast<crd::u64>(av.i);
    return true;
}

// Parse a dispatch's `access` into one r/w mask per binding; false unless it has exactly `n` valid tokens.
[[nodiscard]] bool access_masks(cont::StringView s, crd::u32 n, crd::u32 (&out)[kMaxDispatchBindings]) noexcept
{
    crd::u32   count = 0U;
    crd::usize start = 0U;
    if (s.empty())
    {
        return n == 0U;
    }
    for (crd::usize i = 0U; i <= s.size(); ++i)
    {
        if (i < s.size() && s[i] != ',')
        {
            continue;
        }
        const cont::StringView tok = s.substr(start, i - start);
        start                      = i + 1U;
        if (count >= n || count >= kMaxDispatchBindings)
        {
            return false;
        }
        if (tok == cont::StringView{"r"})
        {
            out[count] = kAccessRead;
        }
        else if (tok == cont::StringView{"w"})
        {
            out[count] = kAccessWrite;
        }
        else if (tok == cont::StringView{"rw"})
        {
            out[count] = kAccessRead | kAccessWrite;
        }
        else
        {
            return false;
        }
        ++count;
    }
    return count == n;
}

[[nodiscard]] cont::StringView kernel_symbol(const Context& ctx, const Operation* op) noexcept
{
    const AttrValue kv = ctx.attr_value(op->attr(cont::StringView{"kernel"}));
    return kv.kind == AttrKind::SymbolRef ? kv.s : cont::StringView{};
}

DeviceReplayStatus unsupported(cont::String& reason, cont::StringView what, cont::StringView op)
{
    reason.append(what);
    if (!op.empty())
    {
        reason.append(cont::StringView{": "});
        reason.append(op);
    }
    return DeviceReplayStatus::Unsupported;
}

// Check that `module` is a device program and describe it.
[[nodiscard]] DeviceReplayStatus describe(const Context& ctx, const Module& module, DeviceProgram& out,
                                          cont::String& reason)
{
    const Region* const body  = module.body();
    const Block* const  block = body != nullptr ? body->first_block() : nullptr;
    if (block == nullptr || block->next_in_region() != nullptr || block->num_args() != 0U)
    {
        return unsupported(reason, "a device program is one top-level block without arguments", {});
    }
    for (const Operation* op = block->first_op(); op != nullptr; op = op->next_in_block())
    {
        const cont::StringView name = ctx.op_name(op->kind());
        if (op->num_regions() != 0U)
        {
            return unsupported(reason, "a device program op has no regions", name);
        }
        if (name == cont::StringView{"arith.const"})
        {
            continue;
        }
        if (name == cont::StringView{"resource.declare"})
        {
            DeviceElement element = DeviceElement::F32;
            if (op->num_results() != 1U || !element_of(ctx, op->result(0U)->type(), element))
            {
                return unsupported(reason, "a device buffer is a plain f32, i32 or u32 buffer", name);
            }
            if (out.buffers.size() >= kReplayMaxDeviceBuffers)
            {
                reason.append("a device program declares at most 16 buffers");
                return DeviceReplayStatus::BadRequest;
            }
            out.buffers.push_back(op->result(0U));
            out.elements.push_back(element);
            out.written.push_back(0U);
            out.last_writer.push_back(0U);
            out.declarations.push_back(op->stable_id().value);
            continue;
        }
        if (name != cont::StringView{"compute.dispatch"})
        {
            return unsupported(reason, "a device program holds only arith.const, resource.declare and compute.dispatch",
                               name);
        }
        if (op->num_operands() < 3U || op->num_operands() - 3U > kMaxDispatchBindings)
        {
            return unsupported(reason, "a dispatch has a grid and at most 8 bindings", name);
        }
        for (crd::u32 g = 0U; g < 3U; ++g)
        {
            crd::u64 groups = 0U;
            if (!const_grid(ctx, op->operand(g), groups))
            {
                return unsupported(reason, "a dispatch grid is a positive constant", name);
            }
        }
        if (kernel_symbol(ctx, op).empty())
        {
            return unsupported(reason, "a dispatch names its kernel symbol", name);
        }
        const crd::u32  nbind = op->num_operands() - 3U;
        crd::u32        masks[kMaxDispatchBindings]{};
        const AttrValue av = ctx.attr_value(op->attr(cont::StringView{"access"}));
        if (av.kind != AttrKind::String || !access_masks(av.s, nbind, masks))
        {
            return unsupported(reason, "a dispatch's access has one r, w or rw per binding", name);
        }
        for (crd::u32 k = 0U; k < nbind; ++k)
        {
            const Value* const v     = op->operand(3U + k);
            crd::usize         index = out.buffers.size();
            for (crd::usize i = 0U; i < out.buffers.size(); ++i)
            {
                if (out.buffers[i] == v)
                {
                    index = i;
                    break;
                }
            }
            if (index == out.buffers.size())
            {
                return unsupported(reason, "a dispatch binds buffers declared before it in the block", name);
            }
            if ((masks[k] & kAccessWrite) != 0U)
            {
                out.written[index]     = 1U;
                out.last_writer[index] = op->stable_id().value;
            }
        }
        out.dispatches.push_back(op);
    }
    if (out.dispatches.empty())
    {
        return unsupported(reason, "a device program dispatches at least once", {});
    }
    return DeviceReplayStatus::Ok;
}

// Every dispatched symbol has exactly one kernel, every kernel is dispatched, and every kernel is within bounds.
[[nodiscard]] DeviceReplayStatus check_kernels(const Context& ctx, const DeviceProgram& program,
                                               cont::ConstSpan<DeviceKernelSource> kernels, cont::String& reason)
{
    if (kernels.size() > kReplayMaxDeviceKernels)
    {
        reason.append("a device record holds at most 16 kernels");
        return DeviceReplayStatus::BadRequest;
    }
    for (crd::usize i = 0U; i < kernels.size(); ++i)
    {
        const DeviceKernelSource& k = kernels[i];
        if (k.symbol.empty() || k.symbol.size() > kReplayMaxStringBytes || k.ckir.size() > kReplayMaxKernelBytes)
        {
            reason.append("a kernel needs a symbol of at most 256 bytes and CKIR text of at most 1 MiB");
            return DeviceReplayStatus::BadRequest;
        }
        bool dispatched = false;
        for (const Operation* const op : program.dispatches)
        {
            dispatched = dispatched || kernel_symbol(ctx, op) == k.symbol;
        }
        for (crd::usize j = 0U; j < i; ++j)
        {
            if (kernels[j].symbol == k.symbol)
            {
                dispatched = false;
            }
        }
        if (!dispatched)
        {
            reason.append("kernel @");
            reason.append(k.symbol);
            reason.append(" is given twice or not dispatched");
            return DeviceReplayStatus::BadRequest;
        }
    }
    for (const Operation* const op : program.dispatches)
    {
        bool found = false;
        for (const DeviceKernelSource& k : kernels)
        {
            found = found || k.symbol == kernel_symbol(ctx, op);
        }
        if (!found)
        {
            reason.append("no kernel is given for @");
            reason.append(kernel_symbol(ctx, op));
            return DeviceReplayStatus::BadRequest;
        }
    }
    return DeviceReplayStatus::Ok;
}

// Read `blob` into the fresh `ctx` with the host's dialects and assign stable ids. Null when it did not read.
[[nodiscard]] Module* load(Context& ctx, cont::ConstSpan<crd::u8> blob, Registrar registrar, void* user,
                           crd::u64& content_hash)
{
    if (registrar != nullptr)
    {
        registrar(ctx, user);
    }
    const ReadResult rr = read_program(ctx, blob, ctx.allocator());
    if (!rr.ok() || rr.module == nullptr)
    {
        return nullptr;
    }
    ctx.assign_stable_ids(*rr.module);
    content_hash = rr.content_hash;
    return rr.module;
}

void site_of(const Context& ctx, const Module& module, crd::u64 op, OwnedReplaySite& out)
{
    const ReplaySite s = replay_site_in_module(ctx, module, op, out.file.allocator());
    out.op             = op;
    out.file.clear();
    out.file.append(s.file);
    out.line = s.line;
    out.col  = s.col;
}

[[nodiscard]] bool adapter_fits(const DeviceAdapterInfo& a) noexcept
{
    return !a.backend.empty() && a.backend.size() <= kReplayMaxStringBytes && a.name.size() <= kReplayMaxStringBytes;
}

// The adapter fields of `record` that differ from `a`, comma-separated into `out`. True when none does.
[[nodiscard]] bool same_adapter(const DeviceAdapterRecord& record, const DeviceAdapterInfo& a, cont::String& out)
{
    const crd::usize before = out.size();
    const auto       note   = [&out](bool equal, cont::StringView name)
    {
        if (equal)
        {
            return;
        }
        if (!out.empty())
        {
            out.push_back(',');
        }
        out.append(name);
    };
    note(same(record.backend, a.backend), "backend");
    note(same(record.name, a.name), "name");
    note(record.vendor == a.vendor, "vendor");
    note(record.device == a.device, "device");
    note(record.driver == a.driver, "driver");
    note(record.api == a.api, "api");
    return out.size() == before;
}

// ---- the CPU reference executor -------------------------------------------------------------------------------------

[[nodiscard]] bool reference_refuses(cont::String& reason, cont::StringView symbol, cont::StringView why)
{
    reason.append("the CPU reference cannot run @");
    reason.append(symbol);
    reason.append(cont::StringView{": "});
    reason.append(why);
    return false;
}

// Whether the reference runs `g`/`e` faithfully: a scalar compute kernel with a one-dimensional workgroup and no
// ray-tracing node or statement (eval_cpu_kernel would skip them).
[[nodiscard]] bool reference_runs(const kir::KGraph& g, const kir::KEntry& e, cont::StringView symbol,
                                  cont::String& reason)
{
    if (e.stage != kir::KStage::Compute || e.kernel_body_count <= 0)
    {
        return reference_refuses(reason, symbol, "it is not a compute kernel with a statement body");
    }
    if (e.local_size[0] == 0U || e.local_size[1] != 1U || e.local_size[2] != 1U)
    {
        return reference_refuses(reason, symbol, "its workgroup is not one-dimensional");
    }
    for (int i = 0; i < g.size(); ++i)
    {
        const kir::KNode& n = g.node(i);
        if (n.type.kind != kir::TKind::Scalar || n.op == kir::KOp::AccelStructDecl || n.op == kir::KOp::RayHitResult ||
            n.op == kir::KOp::RayPayloadDecl || n.op == kir::KOp::PayloadLoad || n.op == kir::KOp::CallableDataDecl)
        {
            return reference_refuses(reason, symbol, "it has a vector, texture or ray-tracing node");
        }
        // The evaluator models two builtins and reads every other one as 0.
        if (n.op == kir::KOp::Builtin && static_cast<kir::KBuiltin>(n.iidx) != kir::KBuiltin::LocalInvocationIndex &&
            static_cast<kir::KBuiltin>(n.iidx) != kir::KBuiltin::WorkgroupIndex)
        {
            return reference_refuses(reason, symbol,
                                     "it reads a builtin other than LocalInvocationIndex and WorkgroupIndex");
        }
    }
    for (int i = 0; i < g.stmt_count(); ++i)
    {
        switch (g.stmt(i).kind)
        {
        case kir::KStmtKind::TraceRayClosest:
        case kir::KStmtKind::TraceRayHit:
        case kir::KStmtKind::TraceRayCurves:
        case kir::KStmtKind::TraceRayPipeline:
        case kir::KStmtKind::PayloadStore:
        case kir::KStmtKind::ReorderThread:
        case kir::KStmtKind::IgnoreHitIf:
        case kir::KStmtKind::ReportHit:
        case kir::KStmtKind::ExecuteCallable:
            return reference_refuses(reason, symbol, "it has a ray-tracing statement");
        default: break;
        }
    }
    return true;
}

[[nodiscard]] kir::DType kir_dtype(DeviceElement e) noexcept
{
    switch (e) // no default (-Werror=switch)
    {
    case DeviceElement::F32: return kir::DType::F32;
    case DeviceElement::I32: return kir::DType::I32;
    case DeviceElement::U32: return kir::DType::U32;
    }
    return kir::DType::F32;
}

[[nodiscard]] crd::f64 widen(DeviceElement e, crd::u32 w) noexcept
{
    switch (e) // no default (-Werror=switch)
    {
    case DeviceElement::F32:
    {
        float f = 0.0F;
        std::memcpy(&f, &w, sizeof(f));
        return static_cast<crd::f64>(f);
    }
    case DeviceElement::I32: return static_cast<crd::f64>(static_cast<crd::i32>(w));
    case DeviceElement::U32: return static_cast<crd::f64>(w);
    }
    return 0.0;
}

[[nodiscard]] crd::u32 narrow(DeviceElement e, crd::f64 v) noexcept
{
    if (e == DeviceElement::F32)
    {
        const float f = static_cast<float>(v);
        crd::u32    w = 0U;
        std::memcpy(&w, &f, sizeof(w));
        return w;
    }
    // Integers: eval_cpu_kernel rounds every store to its dtype, so `v` is an integer in range; anything else is 0.
    constexpr crd::f64 limit = 9223372036854775808.0; // 2^63
    if (!(v >= -limit && v < limit))
    {
        return 0U;
    }
    return static_cast<crd::u32>(static_cast<crd::u64>(static_cast<crd::i64>(v)));
}

struct ReferenceKernel
{
    explicit ReferenceKernel(memory::IAllocator* a) : graph(a) {}

    kir::KGraph graph;
    kir::KEntry entry;
};

bool reference_run(Context& ctx, const Module& module, cont::ConstSpan<DeviceKernelSource> kernels,
                   cont::Span<DeviceBufferView> buffers, DeviceRunOutcome& out, cont::String& reason, void* user)
{
    memory::IAllocator* const scratch = static_cast<memory::IAllocator*>(user);
    out                               = DeviceRunOutcome{};
    DeviceProgram program(scratch);
    if (describe(ctx, module, program, reason) != DeviceReplayStatus::Ok || program.buffers.size() != buffers.size())
    {
        reason.append(cont::StringView{"; the CPU reference runs device programs only"});
        return false;
    }

    // Every buffer widened once to the f64 the evaluator works in; written ones narrowed after each dispatch, as a
    // device stores its element type.
    cont::Array<cont::Array<crd::f64>> shadow(scratch);
    for (const DeviceBufferView& b : buffers)
    {
        cont::Array<crd::f64> values(scratch);
        values.reserve(b.words.size());
        for (const crd::u32 w : b.words)
        {
            values.push_back(widen(b.element, w));
        }
        shadow.push_back(std::move(values));
    }

    for (const Operation* const op : program.dispatches)
    {
        const cont::StringView symbol = kernel_symbol(ctx, op);
        cont::StringView       text;
        for (const DeviceKernelSource& k : kernels)
        {
            if (k.symbol == symbol)
            {
                text = k.ckir;
            }
        }
        ReferenceKernel kernel(scratch);
        if (!kir::ckir_read(text, kernel.graph, kernel.entry).ok)
        {
            return reference_refuses(reason, symbol, "its CKIR text does not read");
        }
        if (!reference_runs(kernel.graph, kernel.entry, symbol, reason))
        {
            return false;
        }
        crd::u64 grid[3]{};
        for (crd::u32 g = 0U; g < 3U; ++g)
        {
            (void)const_grid(ctx, op->operand(g), grid[g]); // describe() checked every grid
        }
        if (grid[1] != 1U || grid[2] != 1U)
        {
            return reference_refuses(reason, symbol, "its grid is not one-dimensional");
        }

        const crd::u32   nbind = op->num_operands() - 3U;
        kir::KernelBuffer bound[kMaxDispatchBindings];
        crd::usize        index[kMaxDispatchBindings]{};
        for (crd::u32 k = 0U; k < nbind; ++k)
        {
            for (crd::usize i = 0U; i < buffers.size(); ++i)
            {
                if (buffers[i].resource == op->operand(3U + k))
                {
                    index[k] = i;
                }
            }
            // The kernel's binding k (set 0) is the dispatch's binding operand k: its element type must agree.
            for (int n = 0; n < kernel.graph.size(); ++n)
            {
                const kir::KNode& node = kernel.graph.node(n);
                if (node.op == kir::KOp::BufferDecl && node.dset == 0U && node.iidx == static_cast<crd::i32>(k) &&
                    node.dtype() != kir_dtype(buffers[index[k]].element))
                {
                    return reference_refuses(reason, symbol, "a binding's element type is not its buffer's");
                }
            }
            cont::Array<crd::f64>& values = shadow[index[k]];
            bound[k] =
                kir::KernelBuffer{values.data(), static_cast<crd::i32>(values.size()), 0U, static_cast<crd::u8>(k)};
        }
        kir::eval_cpu_kernel(kernel.graph, kernel.entry, bound, static_cast<int>(nbind), kernel.entry.local_size[0],
                             scratch, static_cast<crd::u32>(grid[0]));
        for (crd::u32 k = 0U; k < nbind; ++k)
        {
            const DeviceElement e = buffers[index[k]].element;
            for (crd::f64& v : shadow[index[k]])
            {
                v = widen(e, narrow(e, v));
            }
        }
    }

    for (crd::usize i = 0U; i < buffers.size(); ++i)
    {
        if (!buffers[i].written)
        {
            continue;
        }
        for (crd::usize j = 0U; j < buffers[i].words.size(); ++j)
        {
            buffers[i].words[j] = narrow(buffers[i].element, shadow[i][j]);
        }
    }
    return true;
}
} // namespace

// ---- distances ------------------------------------------------------------------------------------------------------

crd::u64 f32_ulp_distance(crd::u32 a, crd::u32 b) noexcept
{
    const auto is_nan = [](crd::u32 w)
    {
        return (w & 0x7F800000U) == 0x7F800000U && (w & 0x007FFFFFU) != 0U;
    };
    const bool an = is_nan(a);
    const bool bn = is_nan(b);
    if (an || bn)
    {
        return an && bn ? 0U : kDeviceIncomparable;
    }
    // Map the sign-magnitude patterns onto one ordered integer line; both zeros map to 0.
    const auto ordered = [](crd::u32 w)
    {
        const crd::i64 magnitude = static_cast<crd::i64>(w & 0x7FFFFFFFU);
        return (w & 0x80000000U) != 0U ? -magnitude : magnitude;
    };
    const crd::i64 d = ordered(a) - ordered(b);
    return static_cast<crd::u64>(d < 0 ? -d : d);
}

crd::u64 device_distance(DeviceElement element, DeviceEnvelope envelope, crd::u32 a, crd::u32 b) noexcept
{
    crd::u64 d = 0U;
    switch (element) // no default (-Werror=switch)
    {
    case DeviceElement::F32: d = f32_ulp_distance(a, b); break;
    case DeviceElement::I32:
    {
        const crd::i64 x = static_cast<crd::i64>(static_cast<crd::i32>(a)) - static_cast<crd::i32>(b);
        d                = static_cast<crd::u64>(x < 0 ? -x : x);
        break;
    }
    case DeviceElement::U32: d = a > b ? crd::u64{a - b} : crd::u64{b - a}; break;
    }
    if (envelope.kind == DeviceEnvelopeKind::Exact && a != b && d == 0U)
    {
        d = 1U; // a bit pattern the value measure cannot see (a zero's sign, a NaN's payload) still differs
    }
    return d;
}

bool within_envelope(DeviceElement element, DeviceEnvelope envelope, crd::u64 distance) noexcept
{
    if (envelope.kind == DeviceEnvelopeKind::Exact || element != DeviceElement::F32)
    {
        return distance == 0U;
    }
    return distance <= envelope.ulps;
}

// ---- names ----------------------------------------------------------------------------------------------------------

cont::StringView device_replay_status_name(DeviceReplayStatus s) noexcept
{
    switch (s) // no default (-Werror=switch)
    {
    case DeviceReplayStatus::Ok: return cont::StringView{"ok"};
    case DeviceReplayStatus::BadRequest: return cont::StringView{"bad-request"};
    case DeviceReplayStatus::NotLoaded: return cont::StringView{"not-loaded"};
    case DeviceReplayStatus::Unsupported: return cont::StringView{"unsupported"};
    case DeviceReplayStatus::WrongExecutor: return cont::StringView{"wrong-executor"};
    case DeviceReplayStatus::OtherBuild: return cont::StringView{"other-build"};
    case DeviceReplayStatus::OtherAdapter: return cont::StringView{"other-adapter"};
    case DeviceReplayStatus::MissingInputs: return cont::StringView{"missing-inputs"};
    case DeviceReplayStatus::ContentMismatch: return cont::StringView{"content-mismatch"};
    case DeviceReplayStatus::DeviceFailed: return cont::StringView{"device-failed"};
    }
    return cont::StringView{"?"};
}

cont::StringView device_divergence_name(DeviceDivergenceKind k) noexcept
{
    switch (k) // no default (-Werror=switch)
    {
    case DeviceDivergenceKind::None: return cont::StringView{"none"};
    case DeviceDivergenceKind::Outcome: return cont::StringView{"outcome"};
    case DeviceDivergenceKind::Element: return cont::StringView{"element"};
    }
    return cont::StringView{"?"};
}

// ---- the reference executor -----------------------------------------------------------------------------------------

DeviceExecutor reference_device_executor(memory::IAllocator* scratch) noexcept
{
    DeviceExecutor e;
    e.run             = &reference_run;
    e.user            = scratch;
    e.adapter.backend = cont::StringView{"cpu-reference"};
    e.adapter.name    = cont::StringView{"crd-kir eval_cpu_kernel"};
    e.adapter.driver  = kReferenceDeviceVersion;
    return e;
}

// ---- the shape a host supplies --------------------------------------------------------------------------------------

DeviceReplayStatus describe_device_program(cont::ConstSpan<crd::u8> blob, Registrar registrar, void* user,
                                           DeviceProgramShape& out, cont::String& reason)
{
    reason.clear();
    out.kernels.clear();
    out.buffers                     = 0U;
    memory::IAllocator* const alloc = out.kernels.allocator();
    if (blob.size() > kReplayMaxProgramBytes)
    {
        reason.append("the program is past a record's bounds");
        return DeviceReplayStatus::BadRequest;
    }
    Context             ctx(alloc);
    crd::u64            content_hash = 0U;
    const Module* const module       = load(ctx, blob, registrar, user, content_hash);
    if (module == nullptr)
    {
        return DeviceReplayStatus::NotLoaded;
    }
    DeviceProgram program(alloc);
    if (const DeviceReplayStatus s = describe(ctx, *module, program, reason); s != DeviceReplayStatus::Ok)
    {
        return s;
    }
    for (const Operation* const op : program.dispatches)
    {
        const cont::StringView symbol = kernel_symbol(ctx, op);
        bool                   seen   = false;
        for (const cont::String& k : out.kernels)
        {
            seen = seen || same(k, symbol);
        }
        if (seen)
        {
            continue;
        }
        if (out.kernels.size() >= kReplayMaxDeviceKernels)
        {
            reason.append("a device record holds at most 16 kernels");
            return DeviceReplayStatus::BadRequest;
        }
        cont::String k(alloc);
        k.append(symbol);
        out.kernels.push_back(std::move(k));
    }
    out.buffers = static_cast<crd::u32>(program.buffers.size());
    return DeviceReplayStatus::Ok;
}

// ---- record ---------------------------------------------------------------------------------------------------------

DeviceReplayStatus record_device_run(const DeviceRecordRequest& request, const DeviceExecutor& executor,
                                     Registrar registrar, void* user, ReplayRecord& out, cont::String& reason,
                                     OwnedReplaySite* fault)
{
    reason.clear();
    memory::IAllocator* const alloc = out.program.allocator();
    const DeviceEnvelope      env   = request.envelope;
    if (request.blob.size() > kReplayMaxProgramBytes || request.path.size() > kReplayMaxStringBytes ||
        (env.kind == DeviceEnvelopeKind::Exact && env.ulps != 0U) || env.ulps > kReplayMaxUlps)
    {
        reason.append("the program, its path or the envelope is past a record's bounds");
        return DeviceReplayStatus::BadRequest;
    }
    if (executor.run == nullptr || !adapter_fits(executor.adapter))
    {
        reason.append("the executor has no run function or no adapter backend");
        return DeviceReplayStatus::BadRequest;
    }

    Context        ctx(alloc);
    crd::u64       content_hash = 0U;
    const Module*  module       = load(ctx, request.blob, registrar, user, content_hash);
    if (module == nullptr)
    {
        return DeviceReplayStatus::NotLoaded;
    }
    DeviceProgram program(alloc);
    if (const DeviceReplayStatus s = describe(ctx, *module, program, reason); s != DeviceReplayStatus::Ok)
    {
        return s;
    }
    if (const DeviceReplayStatus s = check_kernels(ctx, program, request.kernels, reason); s != DeviceReplayStatus::Ok)
    {
        return s;
    }
    if (request.buffers.size() != program.buffers.size())
    {
        reason.append("the program declares ");
        append_decimal(reason, program.buffers.size());
        reason.append(" buffers and the request gives ");
        append_decimal(reason, request.buffers.size());
        return DeviceReplayStatus::BadRequest;
    }
    crd::u64 words = 0U;
    for (const cont::ConstSpan<crd::u32> b : request.buffers)
    {
        words += b.size();
        if (b.empty())
        {
            reason.append("every buffer holds at least one element");
            return DeviceReplayStatus::BadRequest;
        }
    }
    if (words > kReplayMaxDeviceWords)
    {
        reason.append("the buffers hold more than 2^22 elements");
        return DeviceReplayStatus::BadRequest;
    }

    // The run: working copies of the initial contents, which the executor overwrites with the written buffers' results.
    cont::Array<cont::Array<crd::u32>> contents(alloc);
    cont::Array<DeviceBufferView>      views(alloc);
    for (crd::usize i = 0U; i < request.buffers.size(); ++i)
    {
        cont::Array<crd::u32> c(alloc);
        c.reserve(request.buffers[i].size());
        for (const crd::u32 w : request.buffers[i])
        {
            c.push_back(w);
        }
        contents.push_back(std::move(c));
    }
    for (crd::usize i = 0U; i < contents.size(); ++i)
    {
        views.push_back(DeviceBufferView{program.buffers[i], program.elements[i],
                                         cont::Span<crd::u32>{contents[i].data(), contents[i].size()},
                                         program.written[i] != 0U});
    }
    DeviceRunOutcome outcome;
    if (!executor.run(ctx, *module, request.kernels, cont::Span<DeviceBufferView>{views.data(), views.size()}, outcome,
                      reason, executor.user))
    {
        return DeviceReplayStatus::DeviceFailed;
    }

    out.schema        = kReplayRecordSchema;
    out.build         = current_build(alloc);
    out.executor      = ReplayExecutorKind::Device;
    out.host_jobs     = 0U;
    out.host_sub_fuel = 0U;
    out.program_path.clear();
    out.program_path.append(request.path);
    out.content_hash = content_hash;
    out.asset        = 0U;
    out.generation   = 0U;
    out.program.clear();
    out.program.reserve(request.blob.size());
    for (const crd::u8 b : request.blob)
    {
        out.program.push_back(b);
    }
    out.entry.clear();
    out.args.clear();
    (void)classify_replay_inputs(ctx, *module, ReplayExecutorKind::Device, /*host_inputs_held=*/true, alloc, nullptr,
                                 out.inputs, nullptr);
    out.max_events   = 1U;
    out.events_total = 0U;
    out.events.clear();
    out.error      = plan::RunError::None;
    out.host_error = exec::ExecError::None;
    out.fault_op   = outcome.fault_op;
    out.results.clear();
    out.cells.clear();
    out.input_reads_total = 0U;
    out.input_reads.clear();

    out.device_adapter.backend.clear();
    out.device_adapter.backend.append(executor.adapter.backend);
    out.device_adapter.name.clear();
    out.device_adapter.name.append(executor.adapter.name);
    out.device_adapter.vendor = executor.adapter.vendor;
    out.device_adapter.device = executor.adapter.device;
    out.device_adapter.driver = executor.adapter.driver;
    out.device_adapter.api    = executor.adapter.api;
    out.device_envelope       = env;
    out.device_error          = outcome.error;
    out.device_kernels.clear();
    for (const DeviceKernelSource& k : request.kernels)
    {
        DeviceKernelRecord rk(alloc);
        rk.symbol.append(k.symbol);
        rk.ckir.append(k.ckir);
        rk.hash = cont::fnv1a_64(k.ckir.data(), k.ckir.size());
        out.device_kernels.push_back(std::move(rk));
    }
    out.device_buffers.clear();
    for (crd::usize i = 0U; i < contents.size(); ++i)
    {
        DeviceBufferRecord rb(alloc);
        rb.element = program.elements[i];
        rb.written = program.written[i] != 0U;
        for (const crd::u32 w : request.buffers[i])
        {
            rb.initial.push_back(w);
        }
        if (rb.written)
        {
            rb.output = std::move(contents[i]);
        }
        out.device_buffers.push_back(std::move(rb));
    }
    if (fault != nullptr)
    {
        site_of(ctx, *module, out.fault_op, *fault);
    }
    return DeviceReplayStatus::Ok;
}

// ---- replay ---------------------------------------------------------------------------------------------------------

DeviceReplayStatus replay_device_record(const ReplayRecord& record, const DeviceExecutor& executor,
                                        Registrar registrar, void* user, const DeviceReplayOptions& options,
                                        DeviceReplay& out)
{
    memory::IAllocator* const alloc = out.reason.allocator();
    out.reason.clear();
    out.divergence      = DeviceDivergence{};
    out.max_distance    = 0U;
    out.compared        = 0U;
    out.replayed_hash   = 0U;
    out.build_differs   = false;
    out.adapter_differs = false;
    out.program_differs = false;
    out.kernels_differ  = false;
    for (OwnedReplaySite* const site : {&out.dispatch, &out.declaration, &out.fault, &out.recorded_fault})
    {
        site->op = 0U;
        site->file.clear();
        site->line = 0U;
        site->col  = 0U;
    }
    if (record.executor != ReplayExecutorKind::Device)
    {
        out.reason.append(replay_executor_name(record.executor));
        return DeviceReplayStatus::WrongExecutor;
    }
    if (executor.run == nullptr || !adapter_fits(executor.adapter))
    {
        out.reason.append("the executor has no run function or no adapter backend");
        return DeviceReplayStatus::BadRequest;
    }

    // Compatibility, before anything runs: the build, the adapter (an exact envelope names its own), the inputs.
    const bool   exact = record.device_envelope.kind == DeviceEnvelopeKind::Exact;
    cont::String differing(alloc);
    out.build_differs = !same_build(record.build, current_build(alloc), differing);
    if (out.build_differs && (exact || !options.any_build))
    {
        out.reason.append(exact ? cont::StringView{"an exact envelope is claimed only on the recording build; "}
                                : cont::StringView{});
        out.reason.append(cont::StringView{"the build differs in "});
        out.reason.append(cont::StringView{differing.data(), differing.size()});
        return DeviceReplayStatus::OtherBuild;
    }
    differing.clear();
    out.adapter_differs = !same_adapter(record.device_adapter, executor.adapter, differing);
    if (out.adapter_differs && exact)
    {
        out.reason.append("an exact envelope is claimed only on the recording adapter; the adapter differs in ");
        out.reason.append(cont::StringView{differing.data(), differing.size()});
        return DeviceReplayStatus::OtherAdapter;
    }
    if (!record_missing_inputs(record, out.reason))
    {
        return DeviceReplayStatus::MissingInputs;
    }

    // The record's own artifacts, never the checkout's: its blob and kernel texts must be the content it recorded.
    Context       rctx(alloc);
    crd::u64      header_hash = 0U;
    Module* const recorded =
        load(rctx, {record.program.data(), record.program.size()}, registrar, user, header_hash);
    if (recorded == nullptr)
    {
        return DeviceReplayStatus::NotLoaded;
    }
    if (header_hash != record.content_hash || stable_hash(rctx, *recorded, alloc) != record.content_hash)
    {
        out.reason.append("the program blob is not the content it was recorded with");
        return DeviceReplayStatus::ContentMismatch;
    }
    for (const DeviceKernelRecord& k : record.device_kernels)
    {
        if (cont::fnv1a_64(k.ckir.data(), k.ckir.size()) != k.hash)
        {
            out.reason.append("the text of kernel @");
            out.reason.append(cont::StringView{k.symbol.data(), k.symbol.size()});
            out.reason.append(" is not the content it was recorded with");
            return DeviceReplayStatus::ContentMismatch;
        }
    }

    // An explicit other program, or other kernel text, replays the same inputs.
    Context  actx(alloc);
    Context* ctx      = &rctx;
    Module*  module   = recorded;
    out.replayed_hash = header_hash;
    if (!options.against.empty())
    {
        module = load(actx, options.against, registrar, user, out.replayed_hash);
        if (module == nullptr)
        {
            return DeviceReplayStatus::NotLoaded;
        }
        ctx = &actx;
    }
    out.program_differs = out.replayed_hash != record.content_hash;
    cont::Array<DeviceKernelSource> kernels(alloc);
    if (options.kernels_against.empty())
    {
        for (const DeviceKernelRecord& k : record.device_kernels)
        {
            kernels.push_back(DeviceKernelSource{cont::StringView{k.symbol.data(), k.symbol.size()},
                                                 cont::StringView{k.ckir.data(), k.ckir.size()}});
        }
    }
    else
    {
        for (const DeviceKernelSource& k : options.kernels_against)
        {
            kernels.push_back(k);
            bool recorded_text = false;
            for (const DeviceKernelRecord& rk : record.device_kernels)
            {
                recorded_text = recorded_text || (same(rk.symbol, k.symbol) &&
                                                  rk.hash == cont::fnv1a_64(k.ckir.data(), k.ckir.size()));
            }
            out.kernels_differ = out.kernels_differ || !recorded_text;
        }
        out.kernels_differ = out.kernels_differ || options.kernels_against.size() != record.device_kernels.size();
    }

    DeviceProgram program(alloc);
    if (const DeviceReplayStatus s = describe(*ctx, *module, program, out.reason); s != DeviceReplayStatus::Ok)
    {
        return s;
    }
    if (const DeviceReplayStatus s = check_kernels(*ctx, program, cont::as_const_span(kernels), out.reason);
        s != DeviceReplayStatus::Ok)
    {
        return s;
    }
    if (program.buffers.size() != record.device_buffers.size())
    {
        out.reason.append("the replayed program declares ");
        append_decimal(out.reason, program.buffers.size());
        out.reason.append(" buffers and the record holds ");
        append_decimal(out.reason, record.device_buffers.size());
        return DeviceReplayStatus::BadRequest;
    }
    for (crd::usize i = 0U; i < program.buffers.size(); ++i)
    {
        if (program.elements[i] != record.device_buffers[i].element)
        {
            out.reason.append("a replayed buffer's element type is not the recorded one");
            return DeviceReplayStatus::BadRequest;
        }
    }

    // The run on the recorded initial contents.
    cont::Array<cont::Array<crd::u32>> contents(alloc);
    cont::Array<DeviceBufferView>      views(alloc);
    for (const DeviceBufferRecord& b : record.device_buffers)
    {
        contents.push_back(cont::Array<crd::u32>(b.initial, alloc));
    }
    for (crd::usize i = 0U; i < contents.size(); ++i)
    {
        views.push_back(DeviceBufferView{program.buffers[i], program.elements[i],
                                         cont::Span<crd::u32>{contents[i].data(), contents[i].size()},
                                         program.written[i] != 0U});
    }
    DeviceRunOutcome outcome;
    if (!executor.run(*ctx, *module, cont::as_const_span(kernels),
                      cont::Span<DeviceBufferView>{views.data(), views.size()}, outcome, out.reason, executor.user))
    {
        return DeviceReplayStatus::DeviceFailed;
    }

    // Compare: the outcome, then every recorded output element in declaration and element order.
    DeviceDivergence& d = out.divergence;
    if (outcome.error != record.device_error || outcome.fault_op != record.fault_op)
    {
        d.kind           = DeviceDivergenceKind::Outcome;
        d.recorded       = record.device_error;
        d.replayed       = outcome.error;
        d.recorded_fault = record.fault_op;
        d.replayed_fault = outcome.fault_op;
    }
    for (crd::usize b = 0U; b < record.device_buffers.size(); ++b)
    {
        const DeviceBufferRecord& rb = record.device_buffers[b];
        if (!rb.written)
        {
            continue;
        }
        for (crd::usize i = 0U; i < rb.output.size(); ++i)
        {
            const crd::u32 was = rb.output[i];
            const crd::u32 now = contents[b][i];
            const crd::u64 dist = device_distance(rb.element, record.device_envelope, was, now);
            ++out.compared;
            if (rb.element == DeviceElement::F32)
            {
                const crd::u64 ulps = f32_ulp_distance(was, now);
                out.max_distance    = ulps > out.max_distance ? ulps : out.max_distance;
            }
            if (d.kind == DeviceDivergenceKind::None && !within_envelope(rb.element, record.device_envelope, dist))
            {
                d.kind     = DeviceDivergenceKind::Element;
                d.buffer   = static_cast<crd::u32>(b);
                d.element  = i;
                d.type     = rb.element;
                d.recorded = was;
                d.replayed = now;
                d.distance = dist;
                d.bound    = exact || rb.element != DeviceElement::F32 ? 0U : record.device_envelope.ulps;
            }
        }
    }
    if (d.kind == DeviceDivergenceKind::Element)
    {
        site_of(*ctx, *module, program.last_writer[d.buffer], out.dispatch);
        site_of(*ctx, *module, program.declarations[d.buffer], out.declaration);
    }
    site_of(*ctx, *module, outcome.fault_op, out.fault);
    site_of(rctx, *recorded, record.fault_op, out.recorded_fault);
    return DeviceReplayStatus::Ok;
}
} // namespace crd::ceir::cook
