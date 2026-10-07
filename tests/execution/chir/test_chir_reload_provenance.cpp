// DIAG.8a — a CHIR-lowered program keeps its CHIR node origins through the hot-reload supervisor. The committed CHIR
// text is lowered under its authored file name, cooked to a blob and installed in a ReloadSet. Failed reloads (a
// corrupt blob, a candidate whose caller contract changed, a source that does not parse) install nothing, and the
// installed generation's compile refusal still names the authored `parallel_for update` node: its CHIR id, its
// line:col in the authored file, and the generation that holds it. A reformat-only reload of the same CHIR program
// from another file is NoChange: the handle stays current and the positions move to the new text, CHIR ids unchanged.
// Expected positions come from scanning the CHIR text, never from the parser. ASCII test names (ctest by-name).

#include <crd/chir/lower.hpp>
#include <crd/chir/node.hpp>
#include <crd/chir/text.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/provenance.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <initializer_list>
#include <utility>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::Module;
using crd::ceir::Origin;
using crd::ceir::OriginSpace;
using crd::ceir::Provenance;
using crd::ceir::cook::AssetId;
using crd::ceir::cook::GenerationSite;
using crd::ceir::cook::GenerationState;
using crd::ceir::cook::ProgramHandle;
using crd::ceir::cook::ReloadDecision;
using crd::ceir::cook::ReloadResult;
using crd::ceir::cook::ReloadSet;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

constexpr const char* kTextPath = CRD_REPO_DIR "/assets/chir/event_handler.chir";
constexpr const char* kFile     = "assets/chir/event_handler.chir";
constexpr const char* kMoved    = "assets/chir/event_handler_moved.chir";

Array<char> slurp(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    Array<char> buf(a);
    buf.resize(static_cast<usize>(sz), '\0');
    f.read(buf.data(), sz);
    return buf;
}

StringView sv(const Array<char>& b)
{
    return StringView(b.data(), b.size());
}

bool contains(const String& s, StringView n)
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

// The independent oracle: the 1-based line holding the first occurrence of `needle`, and the 1-based column of that
// line's first non-blank character (where the CHIR node's keyword starts).
struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
TextPos find_node(StringView text, const char* needle)
{
    const StringView n(needle);
    u32              line       = 1U;
    usize            line_start = 0U;
    for (usize i = 0; i + n.size() <= text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            line_start = i + 1U;
            continue;
        }
        if (StringView(text.data() + i, n.size()) == n)
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

u64 chir_id(const crd::chir::SourceModel& m, const char* name)
{
    const StringView want(name);
    for (u32 i = 0; i < m.node_count(); ++i)
    {
        if (m.str(m.node(i).name) == want)
        {
            return m.node(i).id.value;
        }
    }
    return 0U;
}

void register_lowered_dialects(Context& ctx)
{
    (void)crd::ceir::func::register_dialect(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::task::register_task_ops(ctx);
    (void)crd::ceir::async::register_async_ops(ctx);
}
void registrar(Context& ctx, void* /*user*/)
{
    register_lowered_dialects(ctx);
}

// Lower `text` under `file` and cook it; the lowering Context is gone once this returns, so only the blob carries the
// CHIR origins.
Array<u8> lower_and_cook(StringView text, const char* file, u64 asset, crd::memory::IAllocator* root)
{
    crd::chir::SourceModel model(root);
    REQUIRE(crd::chir::parse_chir(text, 0U, model).ok);
    Context ctx(root);
    register_lowered_dialects(ctx);
    Module* const mod = crd::chir::lower_chir(model, ctx, StringView(file));
    REQUIRE(mod != nullptr);
    crd::ceir::cook::CookResult cr = crd::ceir::cook::cook_program(ctx, *mod, asset, root, root);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}

// `text` with the first occurrence of `from` replaced by `to`.
Array<char> renamed(const Array<char>& text, const char* from, const char* to, crd::memory::IAllocator* a)
{
    const StringView f(from);
    const StringView t(to);
    Array<char>      out(a);
    bool             done = false;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (!done && i + f.size() <= text.size() && StringView(text.data() + i, f.size()) == f)
        {
            for (usize k = 0; k < t.size(); ++k)
            {
                out.push_back(t[k]);
            }
            i += f.size() - 1U;
            done = true;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// Every line break doubled and every line indented two more columns: the same CHIR program, every node moved.
Array<char> reformat(const Array<char>& text, crd::memory::IAllocator* a)
{
    Array<char> out(a);
    for (usize i = 0; i < text.size(); ++i)
    {
        out.push_back(text[i]);
        if (text[i] == '\n')
        {
            out.push_back('\n');
            out.push_back(' ');
            out.push_back(' ');
        }
    }
    return out;
}

// The installed generation's compile refusal (the committed body reads an outer value) must name the authored
// `parallel_for update` node: CHIR id `id`, position `at` in `file`, located in generation `gen` of `set`.
void check_refusal(const ReloadSet& set, AssetId asset, u64 gen, u64 id, const char* file, TextPos at,
                   crd::memory::IAllocator* root)
{
    crd::ceir::cook::Generation* const g = set.generation(asset);
    REQUIRE(g != nullptr);
    const crd::ceir::plan::CompileResult cr = crd::ceir::plan::compile(*g->ctx, *g->program.module, "on_tick", root);
    CHECK(cr.error == crd::ceir::plan::CompileError::CapturedValue);
    REQUIRE(cr.op != nullptr);
    Array<Origin>    storage(root);
    const Provenance p = crd::ceir::resolve_provenance(*g->ctx, cr.op, storage);
    REQUIRE(p.origins.size() == 1U);
    const Origin& o = p.origins[0];
    CHECK(o.space == OriginSpace::ChirNode);
    CHECK(o.node.value == id);
    CHECK(o.loc.line == at.line);
    CHECK(o.loc.col == at.col);
    CHECK(g->ctx->file_path(o.loc.file_id) == StringView(file));

    const GenerationSite site = set.locate(set.handle(asset), cr.op, root);
    CHECK(site.state == GenerationState::Current);
    CHECK(site.generation.value == gen);
    String flc(root);
    flc.append(file);
    for (const u32 v : {at.line, at.col})
    {
        flc.push_back(':');
        char  buf[10];
        usize k = 0U;
        u32   n = v;
        do
        {
            buf[k++] = static_cast<char>('0' + (n % 10U));
            n /= 10U;
        } while (n != 0U);
        while (k > 0U)
        {
            flc.push_back(buf[--k]);
        }
    }
    CHECK(contains(site.where, StringView(flc.data(), flc.size())));
}
} // namespace

TEST_CASE("diag 8a: a CHIR-lowered generation keeps its CHIR node origins through failed reloads", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text = slurp(kTextPath, &root);
    crd::chir::SourceModel             model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), 0U, model).ok);
    const u64     pfor_id = chir_id(model, "update");
    const TextPos at      = find_node(sv(text), "parallel_for update");
    REQUIRE(pfor_id != 0U);
    REQUIRE(at.line != 0U);

    ReloadSet     set(&root, &registrar, nullptr);
    const AssetId asset{5100U};
    {
        const Array<u8> blob = lower_and_cook(sv(text), kFile, asset.value, &root);
        REQUIRE(set.add(asset, ConstSpan<u8>(blob.data(), blob.size())).ok());
    }
    const ProgramHandle                h = set.handle(asset);
    crd::ceir::cook::Generation* const g = set.generation(asset);
    check_refusal(set, asset, 1U, pfor_id, kFile, at, &root);

    SECTION("a truncated blob does not load")
    {
        const Array<u8>    blob = lower_and_cook(sv(text), kFile, asset.value, &root);
        const ReloadResult r    = set.reload(asset, ConstSpan<u8>(blob.data(), blob.size() / 2U));
        CHECK_FALSE(r.load_ok);
        CHECK(r.load_error == crd::ceir::cook::LoadError::ReadFailed);
        CHECK_FALSE(r.installed);
    }
    SECTION("a candidate whose caller contract changed is rejected")
    {
        // The handler is renamed: its exported entry symbol, part of the caller contract, changes.
        const Array<char>  edited = renamed(text, "on_tick", "on_tock", &root);
        const Array<u8>    blob   = lower_and_cook(sv(edited), kMoved, asset.value, &root);
        const ReloadResult r      = set.reload(asset, ConstSpan<u8>(blob.data(), blob.size()));
        CHECK(r.load_ok);
        CHECK(r.decision == ReloadDecision::ContractChange);
        CHECK_FALSE(r.installed);
    }
    SECTION("a source that does not parse is not cooked")
    {
        const ReloadResult r = set.reload_source(asset, StringView("module {\n  )(\n}\n"), StringView(kMoved));
        CHECK(r.cook_error == crd::ceir::cook::CookError::ParseFailed);
        CHECK(r.cook_site.line == 2U);
        CHECK_FALSE(r.installed);
    }

    // Whatever failed, generation 1 is still installed and still names the authored CHIR node in its own file.
    CHECK(set.generation(asset) == g);
    CHECK(set.is_current(asset, h));
    check_refusal(set, asset, 1U, pfor_id, kFile, at, &root);
}

TEST_CASE("diag 8a: a reformat-only CHIR reload moves the CHIR node positions and keeps the generation", "[chir][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const Array<char>                  text  = slurp(kTextPath, &root);
    const Array<char>                  moved = reformat(text, &root);
    crd::chir::SourceModel             model(&root);
    crd::chir::SourceModel             moved_model(&root);
    REQUIRE(crd::chir::parse_chir(sv(text), 0U, model).ok);
    REQUIRE(crd::chir::parse_chir(sv(moved), 0U, moved_model).ok);
    const u64     pfor_id  = chir_id(model, "update");
    const TextPos at       = find_node(sv(text), "parallel_for update");
    const TextPos at_moved = find_node(sv(moved), "parallel_for update");
    REQUIRE(chir_id(moved_model, "update") == pfor_id); // CHIR ids follow names, not positions
    REQUIRE(at_moved.line != at.line);
    REQUIRE(at_moved.col != at.col);

    ReloadSet     set(&root, &registrar, nullptr);
    const AssetId asset{5200U};
    {
        const Array<u8> blob = lower_and_cook(sv(text), kFile, asset.value, &root);
        REQUIRE(set.add(asset, ConstSpan<u8>(blob.data(), blob.size())).ok());
    }
    const ProgramHandle h = set.handle(asset);
    check_refusal(set, asset, 1U, pfor_id, kFile, at, &root);

    const Array<u8>    blob = lower_and_cook(sv(moved), kMoved, asset.value, &root);
    const ReloadResult r    = set.reload(asset, ConstSpan<u8>(blob.data(), blob.size()));
    CHECK(r.decision == ReloadDecision::NoChange);
    CHECK_FALSE(r.installed);
    CHECK(set.is_current(asset, h));
    check_refusal(set, asset, 1U, pfor_id, kMoved, at_moved, &root);
}
