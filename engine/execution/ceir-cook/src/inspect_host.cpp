#include <crd/ceir/cook/inspect_host.hpp>

#include <crd/ceir/provenance.hpp> // Origin / Provenance (a stop's authored position)

#include <chrono>
#include <utility> // std::move

namespace crd::ceir::cook
{
namespace
{
// Each owned allocator grows from a small first chunk: a debug run of an authored program is small.
constexpr crd::usize kHostChunkBytes = crd::usize{1} << 20U;
} // namespace

containers::StringView host_load_name(HostLoad s) noexcept
{
    switch (s) // ⛔ no default (-Werror=switch)
    {
    case HostLoad::Ok: return containers::StringView("ok");
    case HostLoad::Busy: return containers::StringView("busy");
    case HostLoad::CookFailed: return containers::StringView("cook-failed");
    case HostLoad::LoadFailed: return containers::StringView("load-failed");
    case HostLoad::Rejected: return containers::StringView("rejected");
    case HostLoad::CompileFailed: return containers::StringView("compile-failed");
    }
    return containers::StringView("?");
}

InspectHost::InspectHost(memory::IAllocator* alloc, Registrar reg, void* user, inspect::PauseScope scope)
    : m_alloc(alloc), m_session_alloc(kHostChunkBytes, nullptr, "ceir-inspect-session"),
      m_exec_alloc(kHostChunkBytes, nullptr, "ceir-inspect-exec"), m_session(&m_session_alloc, scope),
      m_set(alloc, reg, user), m_compiled(alloc), m_binds(alloc), m_args(alloc), m_result(&m_exec_alloc)
{
    m_session.connect_controller();
}

InspectHost::~InspectHost()
{
    stop_execution();
}

void InspectHost::stop_execution()
{
    if (!m_thread.joinable())
    {
        return;
    }
    while (!m_done.load(std::memory_order_acquire))
    {
        // NotRunning until the executing thread attaches; the next pass raises the cancel once it has.
        (void)m_session.cancel(m_generation);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_thread.join();
}

bool InspectHost::running() const noexcept
{
    return m_thread.joinable() && !m_done.load(std::memory_order_acquire);
}

HostLoadResult InspectHost::load(AssetId id, containers::StringView source, containers::StringView file,
                                 containers::StringView entry)
{
    HostLoadResult out;
    out.generation = m_generation;
    if (running())
    {
        out.status = HostLoad::Busy;
        return out;
    }
    if (m_thread.joinable()) // a finished execution: its thread ends with the run
    {
        m_thread.join();
    }
    if (!m_loaded)
    {
        const AddResult a = m_set.add_source(id, source, file);
        if (!a.ok())
        {
            out.add_error  = a.error;
            out.cook_error = a.cook_error;
            out.cook_site  = a.cook_site;
            out.status     = (a.error == AddError::CookFailed) ? HostLoad::CookFailed : HostLoad::LoadFailed;
            return out;
        }
        m_asset  = id;
        m_loaded = true;
    }
    else if (id.value != m_asset.value)
    {
        out.add_error = AddError::InvalidAssetId;
        out.status    = HostLoad::LoadFailed;
        return out;
    }
    else
    {
        const ReloadResult r = m_set.reload_source(id, source, file);
        out.decision         = r.decision;
        if (r.cook_error != CookError::Ok)
        {
            out.cook_error = r.cook_error;
            out.cook_site  = r.cook_site;
            out.status     = HostLoad::CookFailed;
            return out;
        }
        if (!r.load_ok)
        {
            out.status = HostLoad::LoadFailed;
            return out;
        }
        if (!r.installed && r.decision != ReloadDecision::NoChange)
        {
            out.status = HostLoad::Rejected; // the last good generation and its plan stay installed
            return out;
        }
    }
    // Installed (added, hot-swapped, or unchanged with refreshed positions): compile the entry in the generation the
    // set holds now and bind the session to it, so requests naming the previous generation are refused from here on.
    Generation* const g = m_set.generation(m_asset);
    m_generation        = m_set.handle(m_asset).generation.value;
    out.generation      = m_generation;
    m_compiled          = plan::compile(*g->ctx, *g->program.module, entry, m_alloc);
    if (!m_compiled.ok())
    {
        out.compile_error = m_compiled.error;
        out.status        = HostLoad::CompileFailed;
        return out;
    }
    (void)m_session.bind(m_compiled.plan, *g->ctx, m_generation, m_binds); // not Busy: no execution is attached
    return out;
}

inspect::Refusal InspectHost::add_line_breakpoint(containers::StringView file, crd::u32 line, crd::u32& out_index)
{
    return m_session.add_line_breakpoint(file, line, out_index);
}

inspect::Refusal InspectHost::start(containers::ConstSpan<crd::i64> args)
{
    if (!m_loaded || !m_compiled.ok())
    {
        return inspect::Refusal::NotBound;
    }
    if (running())
    {
        return inspect::Refusal::Busy;
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    // Rebinding resolves breakpoints added since the last bind and resets a finished session to idle, so the
    // controller's next wait sees this execution rather than the previous one's end.
    const Generation* const g = m_set.generation(m_asset);
    if (const inspect::Refusal r = m_session.bind(m_compiled.plan, *g->ctx, m_generation, m_binds);
        r != inspect::Refusal::None)
    {
        return r;
    }
    m_args.clear();
    for (crd::usize i = 0; i < args.size(); ++i)
    {
        m_args.push_back(args[i]);
    }
    m_done.store(false, std::memory_order_release);
    m_thread = std::thread(
        [this]
        {
            m_result = m_session.run(m_compiled.plan, containers::ConstSpan<crd::i64>(m_args.data(), m_args.size()),
                                     &m_exec_alloc);
            m_done.store(true, std::memory_order_release);
        });
    return inspect::Refusal::None;
}

bool InspectHost::wait_finished(crd::u32 timeout_ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!m_done.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= until)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    return true;
}

StableId InspectHost::op_at_line(containers::StringView file, crd::u32 line) const noexcept
{
    if (!m_loaded || !m_compiled.ok())
    {
        return StableId{};
    }
    return inspect::op_at_line(m_compiled.plan, *m_set.generation(m_asset)->ctx, file, line);
}

const Origin* InspectHost::stop_origin(const inspect::StopRecord& stop) const noexcept
{
    if (!m_loaded || !m_compiled.ok() || !stop.at.valid())
    {
        return nullptr;
    }
    return plan::instr_provenance(m_compiled.plan, stop.at).primary();
}

containers::StringView InspectHost::file_path(crd::u32 file_id) const noexcept
{
    if (!m_loaded || file_id == 0U)
    {
        return {};
    }
    return m_set.generation(m_asset)->ctx->file_path(file_id);
}
} // namespace crd::ceir::cook
