#include <crd/renderasset/diagnostic.hpp>

#include <utility> // std::move

namespace crd::renderasset
{
StringView diag_code_name(DiagCode code) noexcept
{
    switch (code)
    {
    case DiagCode::Ok:
        return "ok";
    case DiagCode::MalformedPath:
        return "malformed-path";
    case DiagCode::UnknownScheme:
        return "unknown-scheme";
    case DiagCode::PathEscapesRoot:
        return "path-escapes-root";
    case DiagCode::EmptyPath:
        return "empty-path";
    case DiagCode::EmptySegment:
        return "empty-segment";
    case DiagCode::IdCollision:
        return "id-collision";
    case DiagCode::DuplicateRegistration:
        return "duplicate-registration";
    case DiagCode::MissingDependency:
        return "missing-dependency";
    case DiagCode::CyclicDependency:
        return "cyclic-dependency";
    case DiagCode::MalformedBlob:
        return "malformed-blob";
    case DiagCode::TruncatedBlob:
        return "truncated-blob";
    case DiagCode::SchemaMismatch:
        return "schema-mismatch";
    case DiagCode::TypeMismatch:
        return "type-mismatch";
    case DiagCode::DuplicateStage:
        return "duplicate-stage";
    case DiagCode::IllegalStageComposition:
        return "illegal-stage-composition";
    case DiagCode::StageIoMismatch:
        return "stage-io-mismatch";
    case DiagCode::AttachmentMismatch:
        return "attachment-mismatch";
    case DiagCode::BindingConflict:
        return "binding-conflict";
    case DiagCode::MaterialLightingAccess:
        return "material-lighting-access";
    case DiagCode::InvalidOverride:
        return "invalid-override";
    case DiagCode::MissingResource:
        return "missing-resource";
    case DiagCode::IncompatibleSurface:
        return "incompatible-surface";
    case DiagCode::UnsupportedPhase:
        return "unsupported-phase";
    case DiagCode::DuplicateExecutor:
        return "duplicate-executor";
    case DiagCode::UnknownExecutor:
        return "unknown-executor";
    case DiagCode::InvalidParam:
        return "invalid-param";
    case DiagCode::InvalidSlot:
        return "invalid-slot";
    case DiagCode::QueueMismatch:
        return "queue-mismatch";
    case DiagCode::ExecutionFailed:
        return "execution-failed";
    case DiagCode::UnsupportedPassKind:
        return "unsupported-pass-kind";
    case DiagCode::UnresolvedForEach:
        return "unresolved-for-each";
    case DiagCode::AssetNotFound:
        return "asset-not-found";
    case DiagCode::AssetCookFailed:
        return "asset-cook-failed";
    case DiagCode::InterfaceIncompatible:
        return "interface-incompatible";
    }
    return "unknown";
}

StringView severity_name(Severity sev) noexcept
{
    switch (sev)
    {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "unknown";
}

void DiagnosticList::emit(Severity severity, DiagCode code, StringView message, StringView asset, StringView field,
                          StringView expected, StringView actual, StringView capability)
{
    Diagnostic d{
        severity,
        code,
        String(message, m_alloc),
        String(asset, m_alloc),
        String(field, m_alloc),
        String(expected, m_alloc),
        String(actual, m_alloc),
        String(capability, m_alloc),
    };
    m_items.push_back(std::move(d));
}

bool DiagnosticList::has_errors() const noexcept
{
    for (usize i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].severity == Severity::Error)
        {
            return true;
        }
    }
    return false;
}

usize DiagnosticList::count(DiagCode code) const noexcept
{
    usize n = 0;
    for (usize i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].code == code)
        {
            ++n;
        }
    }
    return n;
}

bool DiagnosticList::contains(DiagCode code) const noexcept
{
    for (usize i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].code == code)
        {
            return true;
        }
    }
    return false;
}
} // namespace crd::renderasset
