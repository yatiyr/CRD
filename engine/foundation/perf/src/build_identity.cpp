// crd-perf -- the build identity of a diagnostic bundle (DIAG.9b). See build_identity.hpp.

#include <crd/perf/build_identity.hpp>

#include <crd/core/build_config.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/platform.hpp>
#include <crd/perf/diagnostics.hpp>

#include "json_writer.hpp"
#include "payload_codec.hpp"

#include <cstring>
#include <initializer_list>

namespace crd::perf
{
namespace
{
namespace cont = crd::containers;
using detail::append_json_escaped;
using detail::append_u64;
using detail::Cursor;
using detail::put_u32;

[[nodiscard]] constexpr bool built_with_asan() noexcept
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

[[nodiscard]] constexpr bool built_with_tsan() noexcept
{
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

void append_compiler_version(cont::String& out)
{
#if defined(__clang__)
    out.append("clang ");
    append_u64(out, static_cast<crd::u64>(__clang_major__));
    out.push_back('.');
    append_u64(out, static_cast<crd::u64>(__clang_minor__));
    out.push_back('.');
    append_u64(out, static_cast<crd::u64>(__clang_patchlevel__));
#elif defined(_MSC_FULL_VER)
    out.append("msvc ");
    append_u64(out, static_cast<crd::u64>(_MSC_FULL_VER));
#elif defined(__GNUC__)
    out.append("gcc ");
    append_u64(out, static_cast<crd::u64>(__GNUC__));
    out.push_back('.');
    append_u64(out, static_cast<crd::u64>(__GNUC_MINOR__));
    out.push_back('.');
    append_u64(out, static_cast<crd::u64>(__GNUC_PATCHLEVEL__));
#else
    out.append("unknown");
#endif
}

[[nodiscard]] cont::StringView view(const cont::String& s) noexcept
{
    return cont::StringView{s.data(), s.size()};
}

BuildIdentityRead refuse(BuildIdentityRead& r, const char* why) noexcept
{
    r.ok    = false;
    r.error = cont::StringView{why};
    return static_cast<BuildIdentityRead&&>(r);
}
} // namespace

BuildIdentity current_build_identity(crd::memory::IAllocator* alloc)
{
    BuildIdentity b(alloc);
    b.diagnostic_schema = kDiagnosticSchemaVersion;
    b.version.append(cont::StringView{CRD_VERSION_STRING});
    b.platform.append(cont::StringView{crd::platform_name()});
    b.compiler.append(cont::StringView{crd::compiler_name()});
    append_compiler_version(b.compiler_version);
    b.arch.append(cont::StringView{crd::arch_name()});
#if defined(CRD_DEBUG)
    b.config.append(cont::StringView{"debug"});
#else
    b.config.append(cont::StringView{"release"});
#endif
    b.flags = 0U;
    if (CRD_ENABLE_ASSERTS != 0)
    {
        b.flags |= kBuildFlagAsserts;
    }
    if (CRD_ENABLE_PROFILING != 0)
    {
        b.flags |= kBuildFlagProfiling;
    }
    if (crd::fault_seams_compiled())
    {
        b.flags |= kBuildFlagFaultSeams;
    }
    if (built_with_asan())
    {
        b.flags |= kBuildFlagAsan;
    }
    if (built_with_tsan())
    {
        b.flags |= kBuildFlagTsan;
    }
    return b;
}

bool same_build_identity(const BuildIdentity& a, const BuildIdentity& b, cont::String& differing)
{
    const crd::usize before = differing.size();
    const auto       note   = [&differing](bool same, const char* name)
    {
        if (same)
        {
            return;
        }
        if (!differing.empty())
        {
            differing.push_back(',');
        }
        differing.append(name);
    };
    note(a.diagnostic_schema == b.diagnostic_schema, "diagnostic_schema");
    note(view(a.version) == view(b.version), "version");
    note(view(a.platform) == view(b.platform), "platform");
    note(view(a.compiler) == view(b.compiler), "compiler");
    note(view(a.compiler_version) == view(b.compiler_version), "compiler_version");
    note(view(a.arch) == view(b.arch), "arch");
    note(view(a.config) == view(b.config), "config");
    const crd::u32 flag_diff = a.flags ^ b.flags;
    note((flag_diff & kBuildFlagAsserts) == 0U, "asserts");
    note((flag_diff & kBuildFlagProfiling) == 0U, "profiling");
    note((flag_diff & kBuildFlagFaultSeams) == 0U, "fault_seams");
    note((flag_diff & kBuildFlagAsan) == 0U, "asan");
    note((flag_diff & kBuildFlagTsan) == 0U, "tsan");
    note((flag_diff & ~kBuildKnownFlags) == 0U, "unknown_flags");
    return differing.size() == before;
}

cont::Array<crd::u8> encode_build_identity(const BuildIdentity& b, crd::memory::IAllocator* alloc)
{
    cont::Array<crd::u8> out(alloc);
    put_u32(out, kBuildIdentityMagic);
    put_u32(out, kBuildIdentityVersion);
    put_u32(out, b.diagnostic_schema);
    put_u32(out, b.flags & kBuildKnownFlags);
    for (const cont::String* field : {&b.version, &b.platform, &b.compiler, &b.compiler_version, &b.arch, &b.config})
    {
        detail::put_field(out, view(*field), kBuildFieldBytes);
    }
    return out;
}

BuildIdentityRead decode_build_identity(cont::ConstSpan<crd::u8> payload, crd::memory::IAllocator* alloc)
{
    BuildIdentityRead r;
    r.identity = BuildIdentity(alloc);
    Cursor in{payload};
    if (in.u32() != kBuildIdentityMagic || !in.ok)
    {
        return refuse(r, "not a build identity payload (magic)");
    }
    if (in.u32() != kBuildIdentityVersion || !in.ok)
    {
        return refuse(r, "unsupported build identity version");
    }
    r.identity.diagnostic_schema = in.u32();
    r.identity.flags = in.u32();
    if (!in.ok)
    {
        return refuse(r, "build identity header is short");
    }
    if ((r.identity.flags & ~kBuildKnownFlags) != 0U)
    {
        return refuse(r, "unknown build identity flags");
    }
    for (cont::String* field : {&r.identity.version, &r.identity.platform, &r.identity.compiler,
                                &r.identity.compiler_version, &r.identity.arch, &r.identity.config})
    {
        if (!in.field(*field, kBuildFieldBytes))
        {
            return refuse(r, "a build identity field is over its bound or past the payload");
        }
    }
    if (!in.done())
    {
        return refuse(r, "trailing bytes after the build identity");
    }
    r.ok = true;
    return r;
}

void build_identity_json(const BuildIdentity& b, cont::String& out)
{
    const auto str = [&out](const char* key, const cont::String& v, bool comma)
    {
        if (comma)
        {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(key);
        out.append("\":\"");
        append_json_escaped(out, view(v));
        out.push_back('"');
    };
    const auto flag = [&out, &b](const char* key, crd::u32 bit)
    {
        out.append(",\"");
        out.append(key);
        out.append((b.flags & bit) != 0U ? "\":true" : "\":false");
    };
    out.append("{\"diagnostic_schema\":");
    append_u64(out, b.diagnostic_schema);
    str("version", b.version, true);
    str("platform", b.platform, true);
    str("compiler", b.compiler, true);
    str("compiler_version", b.compiler_version, true);
    str("arch", b.arch, true);
    str("config", b.config, true);
    flag("asserts", kBuildFlagAsserts);
    flag("profiling", kBuildFlagProfiling);
    flag("fault_seams", kBuildFlagFaultSeams);
    flag("asan", kBuildFlagAsan);
    flag("tsan", kBuildFlagTsan);
    out.push_back('}');
}

} // namespace crd::perf
