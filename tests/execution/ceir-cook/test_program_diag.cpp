// DIAG.8c -- the program-provenance diagnostic command. `program.provenance` is registered into crd-perf's command
// service and answers, for an authored program file under the host's root, every op in pre-order with its authored
// file:line:col, its CHIR origin and its native binding. The committed authored program assets/ceir/inspect_demo.ceir
// is read as text, as a raw CEIR binary and as a cooked program, and the three answers name the same positions in the
// same file. Refusals (authority, an unsafe path, no root) run nothing; an oversized file is refused unread; a text
// that does not parse is refused with its file, line and column. Expected positions come from scanning the text, never
// from the parser. ASCII test names (ctest by-name).

#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/program_diag.hpp>

#include <crd/ceir/binary.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/parse.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::ceir::Context;
using crd::ceir::cook::ProgramProvenanceCommand;
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;

constexpr const char* kFile   = "ceir/inspect_demo.ceir";
constexpr const char* kAssets = CRD_REPO_DIR "/assets";
constexpr const char* kAsset  = CRD_REPO_DIR "/assets/ceir/inspect_demo.ceir";

// Each case owns its scratch files in the working directory (the file root ".").
constexpr const char* kBinaryFile = "diag8c_program_demo.ceirb";
constexpr const char* kCookedFile = "diag8c_program_demo.crdr";
constexpr const char* kBrokenFile = "diag8c_program_broken.ceir";

void register_dialects(Context& ctx)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}
void registrar(Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
}

String slurp(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    String buf(a);
    buf.resize(static_cast<usize>(sz));
    f.read(buf.data(), sz);
    return buf;
}

void spill(const char* path, const void* data, usize size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    REQUIRE(f.good());
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

// The independent oracle: one expected op per line of the printed text that holds an op ("%N = dialect.op(" or
// "dialect.op("), with its name, the 1-based line, the column of the line's first non-blank character and the
// nesting depth the printer's four-column indent per level gives.
struct Expected
{
    String name;
    u32    line  = 0U;
    u32    col   = 0U;
    u32    depth = 0U;
};
Array<Expected> scan_ops(StringView text, crd::memory::IAllocator* a)
{
    Array<Expected> out(a);
    u32             line  = 1U;
    usize           start = 0U;
    while (start < text.size())
    {
        usize end = text.find('\n', start);
        if (end == StringView::npos)
        {
            end = text.size();
        }
        StringView l = text.substr(start, end - start);
        usize      indent = 0U;
        while (indent < l.size() && l[indent] == ' ')
        {
            ++indent;
        }
        StringView body = l.substr(indent);
        if (!body.empty() && body[0] == '%')
        {
            const usize eq = body.find(" = ");
            body           = eq == StringView::npos ? StringView{} : body.substr(eq + 3U);
        }
        const usize paren = body.find('(');
        const usize dot   = body.find('.');
        if (!body.empty() && body[0] >= 'a' && body[0] <= 'z' && paren != StringView::npos && dot < paren)
        {
            Expected e{String(a)};
            e.name.append(body.substr(0U, paren));
            e.line  = line;
            e.col   = static_cast<u32>(indent + 1U);
            e.depth = static_cast<u32>((indent - 4U) / 4U);
            out.push_back(std::move(e));
        }
        ++line;
        start = end + 1U;
    }
    return out;
}

// The page's items, in order: each object of the "items" array.
Array<String> items_of(const DiagResult& r, crd::memory::IAllocator* a)
{
    Array<String>    out(a);
    const StringView json = view(r.json);
    const usize      at   = json.find("\"items\":[");
    REQUIRE(at != StringView::npos);
    usize pos = json.find("{\"kind\":", at);
    while (pos != StringView::npos)
    {
        usize next = json.find("{\"kind\":", pos + 1U);
        usize stop = next == StringView::npos ? json.size() - 2U : next - 1U; // drop "]}" or the separating comma
        String item(a);
        item.append(json.substr(pos, stop - pos));
        out.push_back(std::move(item));
        pos = next;
    }
    return out;
}

void append_u64(String& s, u64 v)
{
    char buf[24];
    (void)std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    s.append(buf);
}

// The fields every text-authored op must carry, in the command's order.
String expected_fields(const Expected& e, StringView native, crd::memory::IAllocator* a)
{
    String s(a);
    s.append(R"("name":")");
    s.append(view(e.name));
    s.append(R"(","depth":)");
    append_u64(s, e.depth);
    s.append(R"(,"file":")");
    s.append(kFile);
    s.append(R"(","line":)");
    append_u64(s, e.line);
    s.append(R"(,"col":)");
    append_u64(s, e.col);
    s.append(R"(,"gap":"none","origins":1,"chir":0,"chir_file":"","chir_line":0,"chir_col":0,"native":")");
    s.append(native);
    s.append("\"");
    return s;
}

DiagResult run(DiagCommandService& svc, const char* path, u32 page_items = 256U)
{
    DiagRequest r;
    r.command    = crd::ceir::cook::kProgramProvenanceCommand;
    r.path       = StringView{path};
    r.page_items = page_items;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return svc.execute(r);
}

DiagServiceConfig rooted(const char* root)
{
    DiagServiceConfig c;
    c.root = StringView{root};
    return c;
}
} // namespace

TEST_CASE("diag 8c: program.provenance answers each authored op at its file, line and column", "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const String                       text = slurp(kAsset, &root);
    const Array<Expected>              ops  = scan_ops(view(text), &root);
    REQUIRE(ops.size() == 15U); // the oracle itself: two functions, their bodies and the loop body

    const crd::perf::DiagAuthoritySet read = crd::perf::authority_bit(DiagAuthority::Read);

    SECTION("the committed text, read under the request's relative path")
    {
        ProgramProvenanceCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(read, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(svc, cmd));
        CHECK_FALSE(crd::ceir::cook::register_program_provenance(svc, cmd)); // one name, one command

        const DiagResult r = run(svc, kFile);
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(r.total == ops.size());
        CHECK(r.complete);
        CHECK(has(view(r.json), R"("form":"text")"));
        CHECK(has(view(r.json), R"("ops":15,"positioned":15,"unpositioned":0,"chir_origins":0)"));
        CHECK(has(view(r.json), R"("intrinsic":0,"unregistered":0,"origin_items":0)"));
        CHECK_FALSE(has(view(r.json), StringView{CRD_REPO_DIR})); // the host's root never reaches the answer

        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == ops.size());
        for (usize i = 0; i < ops.size(); ++i)
        {
            INFO(items[i].c_str());
            const String want = expected_fields(ops[i], "not-intrinsic", &root);
            CHECK(has(view(items[i]), view(want)));
            CHECK(has(view(items[i]), R"({"kind":"op",)"));
            CHECK_FALSE(has(view(items[i]), R"("op":0,)")); // every op has its stable id
            CHECK_FALSE(has(view(items[i]), R"("clipped":true)"));
        }
        CHECK(cmd.runs.load() == 1U);
        CHECK(cmd.bytes_read.load() == text.size());

        // Pages are cut from the one snapshot: four items a page walk the same items in the same order.
        u64        cursor = 0U;
        usize      seen   = 0U;
        DiagRequest next;
        next.command    = crd::ceir::cook::kProgramProvenanceCommand;
        next.path       = StringView{kFile};
        next.page_items = 4U;
        do
        {
            next.cursor          = cursor;
            const DiagResult page = svc.execute(next);
            REQUIRE(page.status == DiagStatus::Ok);
            const Array<String> got = items_of(page, &root);
            for (usize k = 0; k < got.size(); ++k)
            {
                CHECK(view(got[k]) == view(items[seen + k]));
            }
            seen += got.size();
            cursor = page.next_cursor;
        } while (cursor != 0U);
        CHECK(seen == ops.size());
        CHECK(cmd.runs.load() == 2U); // the walk took one more snapshot, then paged it
    }

    SECTION("without the host's dialects every op's native binding is unknown, never non-intrinsic")
    {
        ProgramProvenanceCommand bare; // no registrar
        DiagCommandService       svc(read, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(svc, bare));
        const DiagResult r = run(svc, kFile);
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(has(view(r.json), R"("intrinsic":0,"unregistered":15)"));
        const Array<String> items = items_of(r, &root);
        REQUIRE(items.size() == ops.size());
        for (usize i = 0; i < ops.size(); ++i)
        {
            CHECK(has(view(items[i]), view(expected_fields(ops[i], "unregistered", &root))));
        }
    }

    SECTION("the binary and cooked forms name the same authored file and positions")
    {
        Context ctx(&root);
        register_dialects(ctx);
        const crd::ceir::ParseResult pr = crd::ceir::parse(ctx, view(text), ctx.register_file(StringView{kFile}));
        REQUIRE(pr.ok);
        const Array<u8> blob = crd::ceir::serialize(ctx, *pr.module, &root);
        spill(kBinaryFile, blob.data(), blob.size());

        Context                           cook_ctx(&root);
        register_dialects(cook_ctx);
        const crd::ceir::cook::CookResult cr =
            crd::ceir::cook::cook_program_text(cook_ctx, view(text), StringView{kFile}, 4801U, &root, &root);
        REQUIRE(cr.ok());
        spill(kCookedFile, cr.blob.data(), cr.blob.size());

        ProgramProvenanceCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService text_svc(read, rooted(kAssets), &root);
        DiagCommandService file_svc(read, rooted("."), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(text_svc, cmd));
        REQUIRE(crd::ceir::cook::register_program_provenance(file_svc, cmd));

        const DiagResult from_text = run(text_svc, kFile);
        const DiagResult binary    = run(file_svc, kBinaryFile);
        const DiagResult cooked    = run(file_svc, kCookedFile);
        INFO(binary.json.c_str());
        INFO(cooked.json.c_str());
        REQUIRE(from_text.status == DiagStatus::Ok);
        REQUIRE(binary.status == DiagStatus::Ok);
        REQUIRE(cooked.status == DiagStatus::Ok);
        CHECK(has(view(binary.json), R"("form":"binary")"));
        CHECK(has(view(cooked.json), R"("form":"cooked")"));

        // The recorded and recomputed content hashes agree, and match the text form's.
        String hash(&root);
        hash.append("\"content_hash\":");
        append_u64(hash, cr.content_hash);
        hash.append(",\"recorded_hash\":");
        String recorded = hash;
        append_u64(recorded, cr.content_hash);
        CHECK(has(view(cooked.json), view(recorded)));
        CHECK(has(view(binary.json), view(hash)));
        CHECK(has(view(from_text.json), view(hash)));

        const Array<String> t = items_of(from_text, &root);
        const Array<String> b = items_of(binary, &root);
        const Array<String> c = items_of(cooked, &root);
        REQUIRE(t.size() == ops.size());
        REQUIRE(b.size() == ops.size());
        REQUIRE(c.size() == ops.size());
        for (usize i = 0; i < ops.size(); ++i)
        {
            INFO(b[i].c_str());
            CHECK(view(b[i]) == view(t[i]));
            CHECK(view(c[i]) == view(t[i]));
        }
        (void)std::remove(kBinaryFile);
        (void)std::remove(kCookedFile);
    }
}

TEST_CASE("diag 8c: program.provenance refuses before reading, and names where a text stopped parsing",
          "[ceir][cook][diag]")
{
    crd::memory::GrowableTlsfAllocator root;
    const crd::perf::DiagAuthoritySet  read = crd::perf::authority_bit(DiagAuthority::Read);

    SECTION("service refusals never reach the command")
    {
        ProgramProvenanceCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService record_only(crd::perf::authority_bit(DiagAuthority::Record), rooted(kAssets), &root);
        DiagCommandService no_root(read, DiagServiceConfig{}, &root);
        DiagCommandService svc(read, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(record_only, cmd));
        REQUIRE(crd::ceir::cook::register_program_provenance(no_root, cmd));
        REQUIRE(crd::ceir::cook::register_program_provenance(svc, cmd));

        CHECK(run(record_only, kFile).status == DiagStatus::Unauthorized);
        CHECK(run(no_root, kFile).status == DiagStatus::Unavailable);
        CHECK(run(svc, "../assets/ceir/inspect_demo.ceir").status == DiagStatus::BadArgument);
        std::atomic<bool> cancel{true};
        DiagRequest       r;
        r.command = crd::ceir::cook::kProgramProvenanceCommand;
        r.path    = StringView{kFile};
        CHECK(svc.execute(r, &cancel).status == DiagStatus::Cancelled);
        CHECK(cmd.runs.load() == 0U);
        CHECK(cmd.bytes_read.load() == 0U);
    }

    SECTION("a program over the host's limit is refused without reading a byte")
    {
        ProgramProvenanceCommand cmd;
        cmd.registrar         = &registrar;
        cmd.max_program_bytes = 64U;
        DiagCommandService svc(read, rooted(kAssets), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(svc, cmd));
        const DiagResult r = run(svc, kFile);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Oversized);
        CHECK(has(view(r.json), "the host's limit is 64"));
        CHECK(cmd.runs.load() == 1U);
        CHECK(cmd.bytes_read.load() == 0U);
    }

    SECTION("a text that does not parse is refused with its file, line and column")
    {
        // A stray ")" on its own line, four columns in, before the first arith.addi.
        const String text = slurp(kAsset, &root);
        const usize  at   = view(text).find("        %7 = arith.addi");
        REQUIRE(at != StringView::npos);
        u32 broken = 1U;
        for (usize i = 0; i < at; ++i)
        {
            broken += text.data()[i] == '\n' ? 1U : 0U;
        }
        String bad(&root);
        bad.append(view(text).substr(0U, at));
        bad.append("    )\n");
        bad.append(view(text).substr(at));
        spill(kBrokenFile, bad.data(), bad.size());

        ProgramProvenanceCommand cmd;
        cmd.registrar = &registrar;
        DiagCommandService svc(read, rooted("."), &root);
        REQUIRE(crd::ceir::cook::register_program_provenance(svc, cmd));
        const DiagResult r = run(svc, kBrokenFile);
        INFO(r.json.c_str());
        CHECK(r.status == DiagStatus::Failed);
        String where(&root);
        where.append("the program text did not parse at ");
        where.append(kBrokenFile);
        where.push_back(':');
        append_u64(where, broken);
        where.append(":5:"); // the ")" after the four-space indent
        CHECK(has(view(r.json), view(where)));
        CHECK(r.items == 0U);
        CHECK(cmd.bytes_read.load() == bad.size());
        (void)std::remove(kBrokenFile);
    }
}
