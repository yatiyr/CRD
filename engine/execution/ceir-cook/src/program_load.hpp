#pragma once

// crd-ceir-cook (private) -- the program load the program diagnostic commands share. A program file under the host's
// root is read through the bounded read (its size is checked before a byte is read), its form is chosen by its
// leading bytes (a cooked CRDR program, a raw CEIR binary, else CEIR text parsed under the request's relative path, so
// the host's root never reaches an answer), and it is loaded into the caller's fresh Context after the host's dialects
// were registered there. Stable ids are assigned before the module is returned.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diag_commands.hpp>

#include <atomic>

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceir::cook::detail
{
// NOLINTNEXTLINE(performance-enum-size)
enum class ProgramForm : crd::u8
{
    Text,
    Binary,
    Cooked,
};

// "text", "binary", "cooked".
[[nodiscard]] containers::StringView program_form_name(ProgramForm f) noexcept;

struct LoadedProgram
{
    Module*     module        = nullptr;
    ProgramForm form          = ProgramForm::Text;
    crd::u64    recorded_hash = 0U; // the content hash a cooked program recorded (0 for the other forms)
};

// Read the request's file into `bytes` and load it into `ctx` (which must be fresh). Returns Ok with `out` filled, or
// Oversized / Failed / Cancelled with `reason` set. `bytes` must outlive the module (a text parse may borrow it).
[[nodiscard]] perf::DiagStatus load_program(const perf::DiagCall& call, crd::u64 max_bytes, Registrar registrar,
                                            void* user, Context& ctx, containers::Array<crd::u8>& bytes,
                                            LoadedProgram& out, containers::String& reason,
                                            std::atomic<crd::u64>& bytes_read);
} // namespace crd::ceir::cook::detail
