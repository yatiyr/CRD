#pragma once

// crd-ceir — the DIAGNOSTIC ENGINE (CEIR-8g, ADR-0117, U-§102). ONE structured error surface for text/visual/agent/CLI
// — a stable `DiagnosticCode` + `Severity` + `SourceLoc` provenance + a message + notes + fix-its (`ParseResult`'s flat
// message cannot carry those). ⛔ SEPARATE class, not a Context member. ⛔ THE LIFETIME LANDMINE: message/note/fixit
// text arrives as StringViews from callers who may have built it in dying buffers — the engine COPIES all text into its
// OWN arena (alloc-outlives-borrowers), so a diagnostic outlives the scope that emitted it.

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/growable_linear_allocator.hpp>

namespace crd::ceir
{
// A typed diagnostic code's compile-time id (the make_interface_id shape, sharing id.hpp's fnv1a_ct).
[[nodiscard]] constexpr DiagnosticCode make_diagnostic_code(const char* name) noexcept
{
    return DiagnosticCode{fnv1a_ct(name)};
}

// Severity — a small CLOSED reasoning axis (widen-audit applies if extended). Fatal short-circuits a compilation.
// NOLINTNEXTLINE(performance-enum-size)
enum class Severity : u8
{
    Note = 0,
    Warning,
    Error,
    Fatal,
};

// A suggested edit: replace the text at `loc` with `replacement` (an arena copy).
struct FixIt
{
    SourceLoc              loc;
    containers::StringView replacement;
};

// One collected diagnostic. Every StringView points into the engine's arena (COPIED — never a borrowed caller buffer).
struct Diagnostic
{
    DiagnosticCode                                code;
    containers::StringView                        code_name; // for rendering (the reverse-lookup, arena copy)
    Severity                                      severity = Severity::Error;
    SourceLoc                                     loc;
    containers::StringView                        message;
    containers::ConstSpan<containers::StringView> notes;
    containers::ConstSpan<FixIt>                  fixits;
};

class DiagnosticEngine
{
public:
    DiagnosticEngine(Context& ctx, memory::IAllocator* alloc) : m_ctx(ctx), m_arena(kChunk, alloc), m_diags(alloc) {}

    // Emit a diagnostic. ⛔ ALL text (message, code_name, each note, each fixit replacement) is COPIED into the engine
    // arena, along with the note + fixit arrays — so the caller's buffers may die immediately after.
    void emit(Severity sev, DiagnosticCode code, containers::StringView code_name, SourceLoc loc,
              containers::StringView message, containers::ConstSpan<containers::StringView> notes = {},
              containers::ConstSpan<FixIt> fixits = {})
    {
        Diagnostic d;
        d.code      = code;
        d.code_name = copy_str(code_name);
        d.severity  = sev;
        d.loc       = loc;
        d.message   = copy_str(message);
        if (notes.size() > 0U)
        {
            auto* const arr = static_cast<containers::StringView*>(
                m_arena.allocate(notes.size() * sizeof(containers::StringView), alignof(containers::StringView)));
            for (usize i = 0; i < notes.size(); ++i) { arr[i] = copy_str(notes[i]); }
            d.notes = containers::ConstSpan<containers::StringView>(arr, notes.size());
        }
        if (fixits.size() > 0U)
        {
            auto* const arr = static_cast<FixIt*>(m_arena.allocate(fixits.size() * sizeof(FixIt), alignof(FixIt)));
            for (usize i = 0; i < fixits.size(); ++i) { arr[i] = FixIt{fixits[i].loc, copy_str(fixits[i].replacement)}; }
            d.fixits = containers::ConstSpan<FixIt>(arr, fixits.size());
        }
        m_diags.push_back(d);
    }

    [[nodiscard]] usize             count() const noexcept { return m_diags.size(); }
    [[nodiscard]] const Diagnostic& at(usize i) const noexcept { return m_diags[i]; }
    [[nodiscard]] bool has_errors() const noexcept
    {
        for (usize i = 0; i < m_diags.size(); ++i)
        {
            if (m_diags[i].severity == Severity::Error || m_diags[i].severity == Severity::Fatal) { return true; }
        }
        return false;
    }
    // A Fatal STOPS the pipeline (the PassManager breaks its loop on this) — the "short-circuit a compilation" semantic.
    [[nodiscard]] bool has_fatal() const noexcept
    {
        for (usize i = 0; i < m_diags.size(); ++i)
        {
            if (m_diags[i].severity == Severity::Fatal) { return true; }
        }
        return false;
    }

    // Render `d` as "<code_name> [<severity>] <file>:<line>:<col>: <message>" (notes appended). The file name comes from
    // the Context source map (SourceLoc.file_id); a 0 file_id renders "<unknown>".
    [[nodiscard]] containers::String render(const Diagnostic& d, memory::IAllocator* out) const
    {
        containers::String s(out);
        append_sv(s, d.code_name);
        s.append(" [");
        s.append(severity_name(d.severity));
        s.append("] ");
        const containers::StringView file = m_ctx.file_path(d.loc.file_id);
        append_sv(s, file.empty() ? containers::StringView("<unknown>") : file);
        s.push_back(':');
        append_u32(s, d.loc.line);
        s.push_back(':');
        append_u32(s, d.loc.col);
        s.append(": ");
        append_sv(s, d.message);
        for (usize i = 0; i < d.notes.size(); ++i)
        {
            s.append("\n  note: ");
            append_sv(s, d.notes[i]);
        }
        return s;
    }

private:
    static constexpr usize kChunk = 16U * 1024U;

    [[nodiscard]] containers::StringView copy_str(containers::StringView v)
    {
        if (v.empty()) { return {}; }
        char* const p = static_cast<char*>(m_arena.allocate(v.size(), 1U));
        for (usize i = 0; i < v.size(); ++i) { p[i] = v[i]; }
        return containers::StringView(p, v.size());
    }
    static void append_sv(containers::String& s, containers::StringView v) { s.append(v.data(), v.size()); }
    static void append_u32(containers::String& s, u32 n)
    {
        char  buf[10];
        usize k = 0;
        do {
            buf[k++] = static_cast<char>('0' + (n % 10U));
            n /= 10U;
        } while (n != 0U);
        while (k > 0U) { s.push_back(buf[--k]); }
    }
    [[nodiscard]] static containers::StringView severity_name(Severity sev) noexcept
    {
        switch (sev)
        {
        case Severity::Note: return containers::StringView("note");
        case Severity::Warning: return containers::StringView("warning");
        case Severity::Error: return containers::StringView("error");
        case Severity::Fatal: return containers::StringView("fatal");
        }
        return containers::StringView("error"); // unreachable (total switch)
    }

    Context&                        m_ctx;   // for file_path rendering
    memory::GrowableLinearAllocator m_arena; // ALL copied text + note/fixit arrays
    containers::Array<Diagnostic>   m_diags;
};
} // namespace crd::ceir
