#include "program_load.hpp"

#include "bounded_file.hpp"

#include <crd/ceir/binary.hpp> // kBinaryMagic / deserialize
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp> // read_program
#include <crd/ceir/parse.hpp>
#include <crd/resources/crdr.hpp> // kFourCC_CRDR

namespace crd::ceir::cook::detail
{
namespace
{
namespace cont = crd::containers;

using crd::perf::DiagStatus;

[[nodiscard]] crd::u32 le32(const cont::Array<crd::u8>& b) noexcept
{
    return static_cast<crd::u32>(b[0]) | (static_cast<crd::u32>(b[1]) << 8U) | (static_cast<crd::u32>(b[2]) << 16U) |
           (static_cast<crd::u32>(b[3]) << 24U);
}
} // namespace

cont::StringView program_form_name(ProgramForm f) noexcept
{
    switch (f) // no default: every form is named
    {
    case ProgramForm::Text: return cont::StringView{"text"};
    case ProgramForm::Binary: return cont::StringView{"binary"};
    case ProgramForm::Cooked: return cont::StringView{"cooked"};
    }
    return cont::StringView{"?"};
}

DiagStatus load_program(const perf::DiagCall& call, crd::u64 max_bytes, Registrar registrar, void* user, Context& ctx,
                        cont::Array<crd::u8>& bytes, LoadedProgram& out, cont::String& reason,
                        std::atomic<crd::u64>& bytes_read)
{
    return load_program_file(call.file, call.request->path, call.cancel, max_bytes, registrar, user, ctx, bytes, out,
                             reason, bytes_read);
}

DiagStatus load_program_file(cont::StringView file, cont::StringView path, const std::atomic<bool>* cancel,
                             crd::u64 max_bytes, Registrar registrar, void* user, Context& ctx,
                             cont::Array<crd::u8>& bytes, LoadedProgram& out, cont::String& reason,
                             std::atomic<crd::u64>& bytes_read)
{
    const DiagStatus read = read_bounded_file(file, max_bytes, bytes, reason, bytes_read);
    if (read != DiagStatus::Ok)
    {
        return read;
    }
    return load_program_bytes(bytes, path, cancel, registrar, user, ctx, out, reason);
}

DiagStatus load_program_bytes(const cont::Array<crd::u8>& bytes, cont::StringView path, const std::atomic<bool>* cancel,
                              Registrar registrar, void* user, Context& ctx, LoadedProgram& out, cont::String& reason)
{
    if (cancel != nullptr && cancel->load(std::memory_order_acquire))
    {
        reason.append("cancelled after the program was read");
        return DiagStatus::Cancelled;
    }

    if (registrar != nullptr)
    {
        registrar(ctx, user);
    }

    crd::memory::IAllocator* const alloc = ctx.allocator();
    const crd::u32                 magic = bytes.size() >= 4U ? le32(bytes) : 0U;
    if (magic == crd::resources::kFourCC_CRDR)
    {
        out.form             = ProgramForm::Cooked;
        const ReadResult res = read_program(ctx, {bytes.data(), bytes.size()}, alloc);
        if (!res.ok())
        {
            reason.append("the cooked program did not load: ");
            reason.append(read_error_name(res.error));
            return DiagStatus::Failed;
        }
        out.module        = res.module;
        out.recorded_hash = res.content_hash;
    }
    else if (magic == kBinaryMagic)
    {
        out.form              = ProgramForm::Binary;
        const ParseResult res = deserialize(ctx, {bytes.data(), bytes.size()});
        if (!res.ok)
        {
            reason.append("the binary program did not load at byte ");
            append_decimal(reason, res.error_offset);
            reason.append(": ");
            reason.append(cont::StringView{res.error});
            return DiagStatus::Failed;
        }
        out.module = res.module;
    }
    else
    {
        // Parsed under the request's own relative path, so the host's root never reaches the answer.
        out.form = ProgramForm::Text;
        const crd::u32         file_id = ctx.register_file(path);
        const cont::StringView text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const ParseResult      res     = parse(ctx, text, file_id);
        if (!res.ok)
        {
            reason.append("the program text did not parse at ");
            reason.append(path);
            reason.push_back(':');
            append_decimal(reason, res.error_line);
            reason.push_back(':');
            append_decimal(reason, res.error_col);
            reason.append(": ");
            reason.append(cont::StringView{res.error});
            return DiagStatus::Failed;
        }
        out.module = res.module;
    }
    if (out.module == nullptr || out.module->body() == nullptr)
    {
        reason.append("the program has no body");
        return DiagStatus::Failed;
    }
    ctx.assign_stable_ids(*out.module);
    return DiagStatus::Ok;
}
} // namespace crd::ceir::cook::detail
