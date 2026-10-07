// DIAG.8a — a host-intrinsic refusal names its authored op and its native provider. The scene.resolve_* ops are
// ADR-0110 host intrinsics that crd-ceir-gpu evaluates through the RenderResolvers callbacks (evaluate_scene_resolve).
// An unwired callback, a callback that returns 0 and a mistyped chain each used to refuse with a bare error; each must
// now be blamed on the responsible resolve op, and that op must resolve to its authored line after a text parse under a
// named file and a binary round trip into a fresh Context. Expected positions come from scanning the printed text,
// never from the parser. Controls: a successful chain blames no op, including after a refusal with the same output
// struct. Device-free. ASCII test names.

#include <crd/ceir/binary.hpp>
#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/render_materialize.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/ceir/scene.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;            // NOLINT(google-build-using-namespace)
using namespace crd::ceir;      // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
constexpr const char* kFile = "programs/diag/scene_resolve.ceir";

void register_dialects(Context& ctx)
{
    (void)func::register_dialect(ctx);
    (void)scene::register_dialect(ctx);
}

// @main(%d: scene.draw) { %m = resolve_material(%d); %t = resolve_technique(%m) {phase}; %p = resolve_program(%t, %d);
//                         %g = resolve_geometry(%d); return }
// `mistyped` feeds the draw to resolve_technique instead of the material (find_scene_misuse: MaterialTypeMismatch).
Module* build_chain(Context& ctx, bool mistyped)
{
    Module* const m = ctx.create_module();
    m->body()->append(ctx.create_block(0U));
    Operation* const fn = func::create_func(ctx, *m, "main", Visibility::Public, 1U, scene::type_draw(ctx));
    m->body()->first_block()->append(fn);
    Block* const     b    = func::func_body_block(fn);
    Value* const     draw = b->arg(0U);
    Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
    b->append(mat);
    Operation* const tech = scene::build_resolve_technique(
        ctx, mistyped ? draw : mat->result(0U), ctx.attr_string(StringView("opaque")), scene::type_technique(ctx));
    b->append(tech);
    b->append(scene::build_resolve_program(ctx, tech->result(0U), draw, scene::type_program(ctx)));
    b->append(scene::build_resolve_geometry(ctx, draw, scene::type_geometry(ctx)));
    b->append(func::create_return(ctx, {}));
    return m;
}

struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
// The 1-based line holding the first occurrence of `needle`, and the column of that line's first non-blank character.
TextPos find_op(StringView text, const char* needle)
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

// The authored chain after a text parse under kFile and a binary round trip into `loaded`.
struct Loaded
{
    Module* module = nullptr;
    Value*  draw   = nullptr; // @main's scene.draw parameter (the chain's seed)
    TextPos material, technique, program;
};
Loaded author_and_reload(Context& loaded, bool mistyped, memory::IAllocator* alloc)
{
    Context builder(alloc);
    register_dialects(builder);
    const String     text = print(builder, *build_chain(builder, mistyped), alloc);
    const StringView src(text.data(), text.size());
    Loaded           out;
    out.material  = find_op(src, "scene.resolve_material");
    out.technique = find_op(src, "scene.resolve_technique");
    out.program   = find_op(src, "scene.resolve_program");
    REQUIRE(out.material.line != 0U);
    REQUIRE(out.technique.line > out.material.line);
    REQUIRE(out.program.line > out.technique.line);

    Context ctx(alloc);
    register_dialects(ctx);
    const ParseResult pr = parse(ctx, src, ctx.register_file(kFile));
    REQUIRE(pr.ok);
    const Array<u8> blob = serialize(ctx, *pr.module, alloc);
    register_dialects(loaded);
    const ParseResult lr = deserialize(loaded, ConstSpan<u8>(blob.data(), blob.size()));
    REQUIRE(lr.ok);
    out.module           = lr.module;
    Operation* const top = lr.module->body()->first_block()->first_op(); // @main
    REQUIRE(top != nullptr);
    out.draw = func::func_body_block(top)->arg(0U);
    return out;
}

// The blamed op is the named intrinsic at its authored position, and its rendering names the native provider.
void check_blamed(const Context& ctx, const Operation* op, const char* name, TextPos at, memory::IAllocator* alloc)
{
    REQUIRE(op != nullptr);
    const NativeBinding nb = native_binding(ctx, op);
    CHECK(nb.kind == NativeKind::Intrinsic);
    CHECK(nb.op_name == StringView(name));
    CHECK(nb.provider == StringView("host"));
    Array<Origin>    storage(alloc);
    const Provenance p = resolve_provenance(ctx, op, storage);
    REQUIRE(p.primary() != nullptr);
    CHECK(p.primary()->loc.line == at.line);
    CHECK(p.primary()->loc.col == at.col);
    CHECK(ctx.file_path(p.primary()->loc.file_id) == StringView(kFile));
    const String site = render_op_site(ctx, op, alloc);
    String       expect(alloc);
    expect.append(name);
    expect.append(" native host at ");
    expect.append(kFile);
    expect.push_back(':');
    CHECK(contains(site, StringView(expect.data(), expect.size())));
}

SceneResolveHandle ok_material(void* /*user*/, SceneResolveHandle draw)
{
    return draw * 10U + 1U;
}
SceneResolveHandle ok_technique(void* /*user*/, SceneResolveHandle material, StringView /*phase*/)
{
    return material * 10U + 2U;
}
SceneResolveHandle zero_technique(void* /*user*/, SceneResolveHandle /*material*/, StringView /*phase*/)
{
    return 0U;
}
SceneResolveHandle ok_program(void* /*user*/, SceneResolveHandle technique, SceneResolveHandle draw)
{
    return technique * 100U + draw * 10U + 3U;
}
SceneResolveHandle ok_geometry(void* /*user*/, SceneResolveHandle draw)
{
    return draw * 10U + 4U;
}

RenderResolvers all_resolvers()
{
    RenderResolvers r;
    r.resolve_material  = &ok_material;
    r.resolve_technique = &ok_technique;
    r.resolve_program   = &ok_program;
    r.resolve_geometry  = &ok_geometry;
    return r;
}
} // namespace

TEST_CASE("diag 8a: a scene resolve refusal names its authored intrinsic and native provider",
          "[ceir][ceir-gpu][scene][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Loaded                  l = author_and_reload(ctx, false, &root);

    SECTION("a callback that returns 0 is blamed on its resolve op")
    {
        RenderResolvers r   = all_resolvers();
        r.resolve_technique = &zero_technique;
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *l.module, r, l.draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
        CHECK(out.material == 71U); // the chain ran up to the refusal
        check_blamed(ctx, out.fault, "scene.resolve_technique", l.technique, &root);
    }

    SECTION("an unwired callback is blamed on the resolve op that needs it")
    {
        RenderResolvers r = all_resolvers();
        r.resolve_program = nullptr;
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *l.module, r, l.draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
        CHECK(out.technique == 712U);
        check_blamed(ctx, out.fault, "scene.resolve_program", l.program, &root);
    }

    SECTION("an unwired first stage is blamed on the first resolve op")
    {
        RenderResolvers      none;
        SceneResolvedHandles out;
        CHECK(evaluate_scene_resolve(ctx, *l.module, none, l.draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
        check_blamed(ctx, out.fault, "scene.resolve_material", l.material, &root);
    }

    SECTION("control: a resolved chain blames no op, even after a refusal through the same output")
    {
        RenderResolvers r   = all_resolvers();
        r.resolve_technique = &zero_technique;
        SceneResolvedHandles out;
        REQUIRE(evaluate_scene_resolve(ctx, *l.module, r, l.draw, 7U, out) == ExecuteError::UnresolvedSceneHandle);
        REQUIRE(out.fault != nullptr);
        CHECK(evaluate_scene_resolve(ctx, *l.module, all_resolvers(), l.draw, 7U, out) == ExecuteError::None);
        CHECK(out.fault == nullptr);
        CHECK(out.program == 712U * 100U + 73U);
        CHECK(out.geometry == 74U);
    }
}

TEST_CASE("diag 8a: a mistyped scene chain is blamed on the op the verifier names", "[ceir][ceir-gpu][scene][diag]")
{
    memory::GrowableTlsfAllocator root;
    Context                       ctx(&root);
    const Loaded                  l = author_and_reload(ctx, true, &root);
    SceneResolvedHandles          out;
    CHECK(evaluate_scene_resolve(ctx, *l.module, all_resolvers(), l.draw, 7U, out) == ExecuteError::SceneChainMisuse);
    CHECK(out.material == 0U); // refused before any callback ran
    check_blamed(ctx, out.fault, "scene.resolve_technique", l.technique, &root);
    CHECK(scene::find_scene_misuse(ctx, *l.module).op == out.fault);
}
