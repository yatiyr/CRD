// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — Cooker. Phase 3.1.7 v9e-d (2026-05-19).
//
// Writes the GLSL/HLSL helpers prelude + per-IR SDF functions to disk so
// downstream renderers (DFAO, soft-shadows, font MTSDF, editor preview) can
// consume cooked shader text directly without depending on the cooker library
// at runtime.
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/cooker.hpp>

#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/geometry/shader_helpers/glsl_emitter.hpp>
#include <crd/geometry/shader_helpers/hlsl_emitter.hpp>
#include <crd/platform/filesystem.hpp>

namespace crd::geometry::shader_helpers
{
namespace
{

namespace fs = crd::platform::fs;

[[nodiscard]] crd::containers::StringView as_view(const crd::containers::String& s) noexcept
{
    return crd::containers::StringView{s.data(), s.size()};
}

[[nodiscard]] crd::containers::String join_path(
    crd::containers::StringView dir,
    crd::containers::StringView file_name,
    crd::memory::IAllocator*    alloc) noexcept
{
    crd::containers::String out(dir, alloc);
    if (out.size() > 0U)
    {
        const char last = out.data()[out.size() - 1U];
        if (last != '/' && last != '\\') { out.append("/"); }
    }
    out.append(file_name);
    return out;
}

[[nodiscard]] bool ensure_dir(crd::containers::StringView dir) noexcept
{
    if (dir.empty()) { return true; }
    return fs::create_directories(fs::Path(dir));
}

void record_emission(CookResult& result, const crd::containers::String& path) noexcept
{
    result.emitted_paths.emplace_back(as_view(path), result.error_message.allocator());
}

void fail(CookResult& result, crd::containers::StringView message) noexcept
{
    result.ok = false;
    result.error_message = crd::containers::String(message, result.error_message.allocator());
}

} // namespace

// ---------------------------------------------------------------------------
// cook_helpers_prelude
// ---------------------------------------------------------------------------

CookResult cook_helpers_prelude(crd::containers::StringView output_dir,
                                 crd::memory::IAllocator*    alloc) noexcept
{
    CookResult result(alloc);

    if (!ensure_dir(output_dir))
    {
        fail(result, "cook_helpers_prelude: create_directories failed");
        return result;
    }

    {
        const crd::containers::StringView body = glsl_helpers_prelude();
        const crd::containers::String     path = join_path(output_dir, "sdf_helpers.glsl", alloc);
        if (!fs::write_file_text(fs::Path(as_view(path)), body))
        {
            fail(result, "cook_helpers_prelude: write_file_text failed for sdf_helpers.glsl");
            return result;
        }
        record_emission(result, path);
    }

    {
        const crd::containers::StringView body = hlsl_helpers_prelude();
        const crd::containers::String     path = join_path(output_dir, "sdf_helpers.hlsl", alloc);
        if (!fs::write_file_text(fs::Path(as_view(path)), body))
        {
            fail(result, "cook_helpers_prelude: write_file_text failed for sdf_helpers.hlsl");
            return result;
        }
        record_emission(result, path);
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// cook_ir
// ---------------------------------------------------------------------------

CookResult cook_ir(const FormulaIr&            ir,
                   crd::containers::StringView name,
                   crd::containers::StringView output_dir,
                   crd::memory::IAllocator*    alloc) noexcept
{
    CookResult result(alloc);

    if (name.empty())
    {
        fail(result, "cook_ir: name must not be empty");
        return result;
    }

    const IrValidationResult val = validate(ir);
    if (val.status != IrValidationStatus::Ok)
    {
        fail(result, "cook_ir: IR validation failed");
        return result;
    }

    if (!ensure_dir(output_dir))
    {
        fail(result, "cook_ir: create_directories failed");
        return result;
    }

    {
        const crd::containers::String body = emit_glsl_sdf_function(ir, name, alloc);

        crd::containers::String file_name(name, alloc);
        file_name.append(".glsl");
        const crd::containers::String path = join_path(output_dir, as_view(file_name), alloc);

        if (!fs::write_file_text(fs::Path(as_view(path)), as_view(body)))
        {
            fail(result, "cook_ir: write_file_text failed for GLSL");
            return result;
        }
        record_emission(result, path);
    }

    {
        const crd::containers::String body = emit_hlsl_sdf_function(ir, name, alloc);

        crd::containers::String file_name(name, alloc);
        file_name.append(".hlsl");
        const crd::containers::String path = join_path(output_dir, as_view(file_name), alloc);

        if (!fs::write_file_text(fs::Path(as_view(path)), as_view(body)))
        {
            fail(result, "cook_ir: write_file_text failed for HLSL");
            return result;
        }
        record_emission(result, path);
    }

    result.ok = true;
    return result;
}

} // namespace crd::geometry::shader_helpers
