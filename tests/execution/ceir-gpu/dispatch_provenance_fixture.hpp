#pragma once

// DIAG.8a — the shared fixture of the dispatch-provenance tests (device-free tests/execution/ceir-gpu and the
// Vulkan leg tests/execution/ceir-gpu-vulkan): a two-dispatch CEIR program authored as text under a named file, run
// through CSE, serialized, loaded into a fresh Context and lowered. Expected positions come from scanning the text,
// never from the parser.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/pass_manager.hpp>
#include <crd/ceir/passes/cse.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>

#include <catch2/catch_test_macros.hpp>

namespace crd::ceir_gpu_test::dispatch_provenance
{
using namespace crd::ceir;      // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu; // NOLINT(google-build-using-namespace)
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

inline constexpr const char* kFile = "programs/diag/two_pass.ceir";

// Two dispatches; the second reads %3, which the first writes, so the lowering puts a RAW barrier between them.
inline constexpr const char* kTwoPass =
    "module {\n"
    "  ^bb0:\n"
    "    %0 = arith.const() {value = 1} : !index\n"
    "    %1 = resource.declare() : !buffer<plain,!f32>\n"
    "    %2 = resource.declare() : !buffer<plain,!f32>\n"
    "    %3 = resource.declare() : !buffer<plain,!f32>\n"
    "    %4 = resource.declare() : !buffer<plain,!f32>\n"
    "    compute.dispatch(%0, %0, %0, %1, %2, %3) {access = \"r,r,w\", kernel = @add_first}\n"
    "    compute.dispatch(%0, %0, %0, %3, %2, %4) {access = \"r,r,w\", kernel = @add_second}\n"
    "}\n";

inline void register_dialects(Context& ctx)
{
    (void)arith::register_arith_ops(ctx);
    (void)func::register_dialect(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)compute::register_compute_ops(ctx);
}

struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
// The 1-based line holding the first occurrence of `needle`, and the column of that line's first non-blank character.
inline TextPos find_op(StringView text, const char* needle)
{
    const usize nlen       = StringView(needle).size();
    u32         line       = 1U;
    usize       line_start = 0U;
    for (usize i = 0; i + nlen <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, nlen) == StringView(needle))
        {
            usize first = line_start;
            while (text[first] == ' ' || text[first] == '\t')
            {
                ++first;
            }
            return TextPos{line, static_cast<u32>(first - line_start + 1U)};
        }
    }
    return TextPos{};
}

inline bool contains(const String& s, StringView n)
{
    const StringView hay(s.data(), s.size());
    for (usize i = 0; i + n.size() <= hay.size(); ++i)
    {
        if (StringView(hay.data() + i, n.size()) == n)
        {
            return true;
        }
    }
    return false;
}

// The authored program after a text parse under kFile, CSE, serialization and a load into `loaded`, lowered.
struct Loaded
{
    Module*          module = nullptr; // set by author_and_load (DIAG.8b binds a session to it)
    Block*           block  = nullptr;
    const Value*     buffers[4]{}; // the four resource.declare results, in program order
    const Operation* first  = nullptr;
    const Operation* second = nullptr;
    TextPos          first_at, second_at;
};
// The four declared buffers and the two dispatches of `block` (a loaded two-pass program) into `out`, and its lowering
// into `commands`. Positions in `out` are left as they are.
inline void collect_and_lower(const Context& ctx, Block& block, Array<LoweredCommand>& commands, Loaded& out)
{
    out.block  = &block;
    usize nbuf = 0U;
    for (Operation* op = block.first_op(); op != nullptr; op = op->next_in_block())
    {
        const NativeBinding nb = native_binding(ctx, op);
        if (nb.op_name == StringView("resource.declare") && nbuf < 4U)
        {
            out.buffers[nbuf++] = op->result(0U);
        }
        else if (nb.op_name == StringView("compute.dispatch") && out.first == nullptr)
        {
            out.first = op;
        }
        else if (nb.op_name == StringView("compute.dispatch"))
        {
            out.second = op;
        }
    }
    REQUIRE(nbuf == 4U);
    REQUIRE(out.first != nullptr);
    REQUIRE(out.second != nullptr);
    lower_region(ctx, block, commands);
}

// The authored positions of the two dispatches in `src` (found by their kernel symbols).
inline void find_dispatches(StringView src, Loaded& out)
{
    out.first_at  = find_op(src, "@add_first");
    out.second_at = find_op(src, "@add_second");
    REQUIRE(out.first_at.line != 0U);
    REQUIRE(out.second_at.line > out.first_at.line);
}

// `src` defaults to kTwoPass; a variant must keep both kernel symbols, in order.
inline void author_and_load(Context& loaded, Array<LoweredCommand>& commands, Loaded& out, memory::IAllocator* alloc,
                            StringView src = StringView(kTwoPass))
{
    find_dispatches(src, out);

    Context ctx(alloc);
    register_dialects(ctx);
    const ParseResult pr = parse(ctx, src, ctx.register_file(kFile));
    REQUIRE(pr.ok);
    DiagnosticEngine diag(ctx, alloc);
    AnalysisManager  am(alloc);
    PassManager      pm(alloc);
    pm.add_pass(cse_pass());
    pm.run(ctx, *pr.module, am, diag);
    REQUIRE_FALSE(diag.has_errors());
    const Array<u8> blob = serialize(ctx, *pr.module, alloc);

    register_dialects(loaded);
    const ParseResult lr = deserialize(loaded, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(lr.ok);
    out.module = lr.module;
    collect_and_lower(loaded, *lr.module->body()->first_block(), commands, out);
}

// The authored position `op` resolves to in `ctx`; the provenance must name a source line.
inline TextPos authored_at(const Context& ctx, const Operation* op, memory::IAllocator* alloc)
{
    Array<Origin>    storage(alloc);
    const Provenance p = resolve_provenance(ctx, op, storage);
    REQUIRE(p.gap == ProvenanceGap::None);
    const Origin* const o = p.primary();
    REQUIRE(o != nullptr);
    return TextPos{o->loc.line, o->loc.col};
}

inline void append_decimal(String& s, u32 v)
{
    char digits[10];
    int  n = 0;
    do
    {
        digits[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U && n < 10);
    while (n > 0)
    {
        const char c[1] = {digits[--n]};
        s.append(static_cast<const char*>(c), 1U);
    }
}

// "<kFile>:<line>:<col>", the position render_op_site prints for an authored op.
inline String expected_site(const TextPos& at, memory::IAllocator* alloc)
{
    String s(alloc);
    s.append(kFile);
    s.append(":");
    append_decimal(s, at.line);
    s.append(":");
    append_decimal(s, at.col);
    return s;
}
} // namespace crd::ceir_gpu_test::dispatch_provenance
