// crd-shader-cook — the D-007 D2 cook CLI (ADR-0104).
//
//   crd-shader-cook cook <in.kgph> -o <out.crdr> [--backends all|spirv,dxil,cuda,msl,wgsl] [--cache <dir>] [--compress] [--name N]
//   crd-shader-cook info <bundle.crdr>
//
// `cook` deserializes a CKIR shader graph (D1 `.kgph`) and cooks it into a `.crdr` bundle (serialized IR + IR-reflection +
// per-backend blobs: SPIR-V/DXIL real bytecode, CUDA/MSL/WGSL source). `info` dumps a cooked bundle's chunks.
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_serialize.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/shadercook/cook.hpp>

#include <cstdio>
#include <cstring>

namespace
{
crd::memory::GrowableTlsfAllocator g_alloc; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  crd-shader-cook cook <in.kgph> -o <out.crdr> [--backends all|spirv,dxil,cuda,msl,wgsl]\n"
                 "                       [--cache <dir>] [--compress] [--name <n>]\n"
                 "  crd-shader-cook info <bundle.crdr>\n");
}

[[nodiscard]] crd::u32 parse_backends(const char* s)
{
    if (s == nullptr || std::strcmp(s, "all") == 0) { return static_cast<crd::u32>(crd::shadercook::CookBackend::All); }
    crd::u32   mask = 0U;
    const char* p   = s;
    while (*p != '\0')
    {
        const char* start = p;
        while (*p != '\0' && *p != ',') { ++p; }
        const auto n = static_cast<crd::usize>(p - start);
        const auto is = [&](const char* tok) { return std::strlen(tok) == n && std::strncmp(start, tok, n) == 0; };
        if (is("spirv")) { mask |= static_cast<crd::u32>(crd::shadercook::CookBackend::SpirV); }
        else if (is("dxil")) { mask |= static_cast<crd::u32>(crd::shadercook::CookBackend::Dxil); }
        else if (is("cuda")) { mask |= static_cast<crd::u32>(crd::shadercook::CookBackend::Cuda); }
        else if (is("msl")) { mask |= static_cast<crd::u32>(crd::shadercook::CookBackend::Msl); }
        else if (is("wgsl")) { mask |= static_cast<crd::u32>(crd::shadercook::CookBackend::Wgsl); }
        if (*p == ',') { ++p; }
    }
    return mask;
}

int cmd_cook(int argc, char** argv)
{
    const char* in       = (argc > 0) ? argv[0] : nullptr;
    const char* out      = nullptr;
    const char* cache    = nullptr;
    const char* name     = "shader";
    crd::u32    backends = static_cast<crd::u32>(crd::shadercook::CookBackend::All);
    bool        compress = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out = argv[++i]; }
        else if (std::strcmp(argv[i], "--backends") == 0 && i + 1 < argc) { backends = parse_backends(argv[++i]); }
        else if (std::strcmp(argv[i], "--cache") == 0 && i + 1 < argc) { cache = argv[++i]; }
        else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) { name = argv[++i]; }
        else if (std::strcmp(argv[i], "--compress") == 0) { compress = true; }
    }
    if (in == nullptr || out == nullptr) { usage(); return 2; }

    crd::containers::Array<crd::u8> ir(&g_alloc);
    if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path(in), ir))
    {
        std::fprintf(stderr, "crd-shader-cook: cannot read %s\n", in);
        return 1;
    }
    crd::kir::KGraph g(&g_alloc);
    crd::kir::KEntry e;
    if (!crd::kir::deserialize_graph(crd::containers::as_const_span(ir), g, e))
    {
        std::fprintf(stderr, "crd-shader-cook: %s is not a valid CKIR graph\n", in);
        return 1;
    }

    crd::shadercook::CookOptions opts;
    opts.backends  = backends;
    opts.cache_dir = cache;
    opts.compress  = compress;
    crd::shadercook::CookResult r = crd::shadercook::cook_compute_shader(g, e, crd::containers::StringView(name), opts, &g_alloc);
    if (!r.ok)
    {
        std::fprintf(stderr, "crd-shader-cook: cook failed: %s\n", r.error.c_str());
        return 1;
    }
    if (!crd::platform::fs::write_file_binary(crd::platform::fs::Path(out), crd::containers::as_const_span(r.crdr)))
    {
        std::fprintf(stderr, "crd-shader-cook: cannot write %s\n", out);
        return 1;
    }
    std::printf("cooked %s -> %s  (%zu B)  spirv=%u dxil=%u ptx=%u cuda=%u msl=%u wgsl=%u%s\n", in, out, static_cast<size_t>(r.crdr.size()),
                r.spirv_bytes, r.dxil_bytes, r.ptx_bytes, r.cuda_bytes, r.msl_bytes, r.wgsl_bytes, r.from_cache ? "  [cache]" : "");
    return 0;
}

int cmd_info(int argc, char** argv)
{
    if (argc < 1) { usage(); return 2; }
    crd::containers::Array<crd::u8> bytes(&g_alloc);
    if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path(argv[0]), bytes))
    {
        std::fprintf(stderr, "crd-shader-cook: cannot read %s\n", argv[0]);
        return 1;
    }
    crd::shadercook::ShaderBundle bundle(&g_alloc);
    if (!crd::shadercook::read_shader_bundle(crd::containers::as_const_span(bytes), bundle))
    {
        std::fprintf(stderr, "crd-shader-cook: %s is not a valid .crdr bundle\n", argv[0]);
        return 1;
    }
    std::printf("%s: %d chunks\n", argv[0], static_cast<int>(bundle.file.chunks.size()));
    for (crd::usize i = 0; i < bundle.file.chunks.size(); ++i)
    {
        const auto&    c  = bundle.file.chunks[i];
        const crd::u32 fc = c.fourcc;
        std::printf("  %c%c%c%c  %8zu B\n", static_cast<char>(fc & 0xFFU), static_cast<char>((fc >> 8U) & 0xFFU),
                    static_cast<char>((fc >> 16U) & 0xFFU), static_cast<char>((fc >> 24U) & 0xFFU),
                    static_cast<size_t>(c.payload.size()));
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 2; }
    if (std::strcmp(argv[1], "cook") == 0) { return cmd_cook(argc - 2, argv + 2); }
    if (std::strcmp(argv[1], "info") == 0) { return cmd_info(argc - 2, argv + 2); }
    usage();
    return 2;
}
