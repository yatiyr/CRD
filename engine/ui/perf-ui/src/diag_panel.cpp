// crd-perf-ui -- DiagCommandPanel: the GUI consumer of the typed diagnostic command service (see diag_panel.hpp).

#include <crd/perf/ui/diag_panel.hpp>

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <utility> // std::move

namespace crd::perf::ui
{
namespace cont = crd::containers;

namespace
{
constexpr crd::usize kBad = cont::StringView::npos;

[[nodiscard]] bool is_space(char c) noexcept
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

[[nodiscard]] crd::usize skip_space(cont::StringView s, crd::usize i) noexcept
{
    while (i < s.size() && is_space(s[i]))
    {
        ++i;
    }
    return i;
}

// `i` at an opening quote: the index after the closing quote, or kBad.
[[nodiscard]] crd::usize skip_string(cont::StringView s, crd::usize i) noexcept
{
    if (i >= s.size() || s[i] != '"')
    {
        return kBad;
    }
    ++i;
    while (i < s.size())
    {
        if (s[i] == '\\')
        {
            i += 2U;
            continue;
        }
        if (s[i] == '"')
        {
            return i + 1U;
        }
        ++i;
    }
    return kBad;
}

// `i` at a value's first byte: the index after the value, or kBad. Objects and arrays are skipped by depth, with their
// strings skipped whole, so no byte inside a string is read as structure.
[[nodiscard]] crd::usize skip_value(cont::StringView s, crd::usize i) noexcept
{
    if (i >= s.size())
    {
        return kBad;
    }
    if (s[i] == '"')
    {
        return skip_string(s, i);
    }
    if (s[i] == '{' || s[i] == '[')
    {
        crd::u32 depth = 0U;
        while (i < s.size())
        {
            const char c = s[i];
            if (c == '"')
            {
                i = skip_string(s, i);
                if (i == kBad)
                {
                    return kBad;
                }
                continue;
            }
            if (c == '{' || c == '[')
            {
                ++depth;
            }
            else if (c == '}' || c == ']')
            {
                --depth;
                if (depth == 0U)
                {
                    return i + 1U;
                }
            }
            ++i;
        }
        return kBad;
    }
    const crd::usize start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !is_space(s[i]))
    {
        ++i;
    }
    return i > start ? i : kBad;
}

// `arr` is one array value: one view per element.
[[nodiscard]] bool split_array(cont::StringView arr, cont::Array<cont::StringView>& out)
{
    crd::usize i = skip_space(arr, 1U);
    if (i < arr.size() && arr[i] == ']')
    {
        return skip_space(arr, i + 1U) == arr.size();
    }
    for (;;)
    {
        const crd::usize end = skip_value(arr, i);
        if (end == kBad)
        {
            return false;
        }
        out.push_back(arr.substr(i, end - i));
        i = skip_space(arr, end);
        if (i < arr.size() && arr[i] == ',')
        {
            i = skip_space(arr, i + 1U);
            continue;
        }
        return i < arr.size() && arr[i] == ']' && skip_space(arr, i + 1U) == arr.size();
    }
}

[[nodiscard]] bool fail(DiagDocumentView& out)
{
    out.summary = cont::StringView{};
    out.items.clear();
    return false;
}

// "name  [authority]" plus "  (not granted)", NUL-terminated for ImGui.
void command_label(const DiagCommandSpec& spec, bool granted, char* buf, crd::usize size)
{
    const cont::StringView authority = authority_name(spec.authority);
    (void)std::snprintf(buf, size, "%.*s  [%.*s]%s", static_cast<int>(spec.name.size()), spec.name.data(),
                        static_cast<int>(authority.size()), authority.data(), granted ? "" : "  (not granted)");
}

void wrapped(cont::StringView text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}
} // namespace

cont::StringView diag_panel_state_name(DiagPanelState s) noexcept
{
    switch (s)
    {
    case DiagPanelState::Idle:
        return "idle";
    case DiagPanelState::Running:
        return "running";
    case DiagPanelState::Done:
        return "done";
    }
    return "unknown";
}

cont::StringView diag_panel_submit_name(DiagPanelSubmit s) noexcept
{
    switch (s)
    {
    case DiagPanelSubmit::Started:
        return "started";
    case DiagPanelSubmit::Busy:
        return "busy";
    case DiagPanelSubmit::NoCommand:
        return "no-command";
    case DiagPanelSubmit::BadForm:
        return "bad-form";
    case DiagPanelSubmit::NoMorePages:
        return "no-more-pages";
    }
    return "unknown";
}

bool split_diag_document(cont::StringView json, DiagDocumentView& out)
{
    out.summary = cont::StringView{};
    out.items.clear();
    crd::usize i = skip_space(json, 0U);
    if (i >= json.size() || json[i] != '{')
    {
        return fail(out);
    }
    i = skip_space(json, i + 1U);
    if (i < json.size() && json[i] == '}')
    {
        return skip_space(json, i + 1U) == json.size() ? true : fail(out);
    }
    for (;;)
    {
        const crd::usize key_end = skip_string(json, i);
        if (key_end == kBad)
        {
            return fail(out);
        }
        const cont::StringView key = json.substr(i + 1U, key_end - i - 2U);
        i                          = skip_space(json, key_end);
        if (i >= json.size() || json[i] != ':')
        {
            return fail(out);
        }
        i                          = skip_space(json, i + 1U);
        const crd::usize value_end = skip_value(json, i);
        if (value_end == kBad)
        {
            return fail(out);
        }
        const cont::StringView value = json.substr(i, value_end - i);
        if (key == "summary" && value.front() == '{')
        {
            out.summary = value;
        }
        else if (key == "items" && value.front() == '[')
        {
            if (!split_array(value, out.items))
            {
                return fail(out);
            }
        }
        i = skip_space(json, value_end);
        if (i < json.size() && json[i] == ',')
        {
            i = skip_space(json, i + 1U);
            continue;
        }
        if (i < json.size() && json[i] == '}' && skip_space(json, i + 1U) == json.size())
        {
            return true;
        }
        return fail(out);
    }
}

DiagCommandPanel::DiagCommandPanel(DiagCommandService& service, memory::IAllocator* alloc)
    : m_service(&service), m_alloc(alloc), m_commands(alloc), m_grant_text(alloc), m_form_error(alloc),
      m_sent_command(alloc), m_sent_path(alloc), m_sent_args_text(alloc), m_sent_args(alloc), m_pending(alloc),
      m_result(alloc), m_document(alloc)
{
    append_authority_list(m_grant_text, service.grant());
    (void)refresh_commands();
}

DiagCommandPanel::~DiagCommandPanel()
{
    if (m_worker.joinable())
    {
        m_cancel.store(true, std::memory_order_release);
        m_worker.join();
    }
}

bool DiagCommandPanel::granted(crd::u32 index) const noexcept
{
    return index < m_commands.size() && grants(m_service->grant(), m_commands[index].authority);
}

bool DiagCommandPanel::refresh_commands()
{
    if (m_state == DiagPanelState::Running)
    {
        return false;
    }
    const cont::StringView selected = m_selected < m_commands.size() ? m_commands[m_selected].name : cont::StringView{};
    m_commands.clear();
    m_selected       = kNoCommand;
    const crd::u32 n = m_service->command_count();
    for (crd::u32 i = 0U; i < n; ++i)
    {
        const DiagCommandSpec* spec = m_service->command_at(i);
        if (spec == nullptr)
        {
            break;
        }
        m_commands.push_back(*spec);
        if (!selected.empty() && spec->name == selected)
        {
            m_selected = i;
        }
    }
    return true;
}

bool DiagCommandPanel::select(cont::StringView name)
{
    for (crd::u32 i = 0U; i < m_commands.size(); ++i)
    {
        if (m_commands[i].name == name)
        {
            m_selected = i;
            return true;
        }
    }
    return false;
}

bool DiagCommandPanel::set_path(cont::StringView path)
{
    if (path.size() >= kPathCapacity)
    {
        return false;
    }
    std::memcpy(m_path, path.data(), path.size());
    m_path[path.size()] = '\0';
    return true;
}

bool DiagCommandPanel::set_args(cont::StringView text)
{
    if (text.size() >= kArgsCapacity)
    {
        return false;
    }
    std::memcpy(m_args, text.data(), text.size());
    m_args[text.size()] = '\0';
    return true;
}

cont::StringView DiagCommandPanel::path() const noexcept
{
    return cont::StringView{m_path};
}

cont::StringView DiagCommandPanel::args() const noexcept
{
    return cont::StringView{m_args};
}

cont::StringView DiagCommandPanel::form_error() const noexcept
{
    return cont::StringView{m_form_error.data(), m_form_error.size()};
}

bool DiagCommandPanel::parse_args(cont::StringView text, cont::Array<DiagArg>* out)
{
    // With `out`, fills it with views into `text` (m_sent_args_text when the request is kept); without, only checks.
    if (out != nullptr)
    {
        out->clear();
    }
    crd::usize start = 0U;
    crd::u32   line  = 0U;
    while (start <= text.size())
    {
        crd::usize end = text.find('\n', start);
        if (end == kBad)
        {
            end = text.size();
        }
        ++line;
        cont::StringView entry = text.substr(start, end - start);
        if (!entry.empty() && entry.back() == '\r')
        {
            entry.remove_suffix(1U);
        }
        if (!entry.empty())
        {
            const crd::usize eq = entry.find('=');
            if (eq == kBad)
            {
                char buf[96];
                (void)std::snprintf(buf, sizeof(buf), "argument line %u has no '=' (one name=value per line)", line);
                m_form_error.clear();
                m_form_error.append(buf);
                return false;
            }
            if (out != nullptr)
            {
                out->push_back(DiagArg{entry.substr(0U, eq), entry.substr(eq + 1U)});
            }
        }
        start = end + 1U;
    }
    return true;
}

DiagPanelSubmit DiagCommandPanel::submit()
{
    if (m_state == DiagPanelState::Running)
    {
        m_last_submit = DiagPanelSubmit::Busy;
        return m_last_submit;
    }
    m_form_error.clear();
    if (m_selected >= m_commands.size())
    {
        m_last_submit = DiagPanelSubmit::NoCommand;
        return m_last_submit;
    }
    // Check the form's text first, so a bad form leaves the kept snapshot request (and its later pages) alone.
    if (!parse_args(args(), nullptr))
    {
        m_last_submit = DiagPanelSubmit::BadForm;
        return m_last_submit;
    }
    const cont::StringView name = m_commands[m_selected].name;
    m_sent_command.clear();
    m_sent_command.append(name.data(), name.size());
    m_sent_path.clear();
    m_sent_path.append(m_path, path().size());
    m_sent_args_text.clear();
    m_sent_args_text.append(m_args, args().size());
    (void)parse_args(cont::StringView{m_sent_args_text.data(), m_sent_args_text.size()}, &m_sent_args);
    m_request            = DiagRequest{};
    m_request.command    = cont::StringView{m_sent_command.data(), m_sent_command.size()};
    m_request.path       = cont::StringView{m_sent_path.data(), m_sent_path.size()};
    m_request.page_items = m_page_items;
    m_request.page_bytes = m_page_bytes;
    m_request.args       = cont::ConstSpan<DiagArg>(m_sent_args.data(), m_sent_args.size());
    m_sent_valid         = true;
    m_last_submit        = send(0U);
    return m_last_submit;
}

DiagPanelSubmit DiagCommandPanel::next_page()
{
    if (m_state == DiagPanelState::Running)
    {
        m_last_submit = DiagPanelSubmit::Busy;
        return m_last_submit;
    }
    if (!m_sent_valid || m_state != DiagPanelState::Done || m_result.next_cursor == 0U)
    {
        m_last_submit = DiagPanelSubmit::NoMorePages;
        return m_last_submit;
    }
    m_last_submit = send(m_result.next_cursor);
    return m_last_submit;
}

DiagPanelSubmit DiagCommandPanel::page_at(crd::u64 cursor)
{
    if (m_state == DiagPanelState::Running)
    {
        m_last_submit = DiagPanelSubmit::Busy;
        return m_last_submit;
    }
    if (!m_sent_valid)
    {
        m_last_submit = DiagPanelSubmit::NoMorePages;
        return m_last_submit;
    }
    m_last_submit = send(cursor);
    return m_last_submit;
}

DiagPanelSubmit DiagCommandPanel::send(crd::u64 cursor)
{
    join();
    m_request.cursor = cursor;
    m_cancel.store(false, std::memory_order_release);
    m_done.store(false, std::memory_order_release);
    m_state  = DiagPanelState::Running;
    m_worker = std::thread(
        [this]()
        {
            m_pending = m_service->execute(m_request, &m_cancel);
            m_done.store(true, std::memory_order_release);
        });
    return DiagPanelSubmit::Started;
}

bool DiagCommandPanel::cancel() noexcept
{
    if (m_state != DiagPanelState::Running)
    {
        return false;
    }
    m_cancel.store(true, std::memory_order_release);
    return true;
}

void DiagCommandPanel::join() noexcept
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

DiagPanelTick DiagCommandPanel::tick()
{
    if (m_state != DiagPanelState::Running || !m_done.load(std::memory_order_acquire))
    {
        return DiagPanelTick::None;
    }
    join(); // the worker stored its answer and is leaving: this join does not wait on the service
    m_result      = std::move(m_pending);
    m_pending     = DiagResult(m_alloc);
    m_document_ok = split_diag_document(cont::StringView{m_result.json.data(), m_result.json.size()}, m_document);
    m_state       = DiagPanelState::Done;
    ++m_completed;
    return DiagPanelTick::Completed;
}

void DiagCommandPanel::draw()
{
    if (!ImGui::Begin("Diagnostic commands"))
    {
        ImGui::End();
        return;
    }
    ImGui::Text("grant: %.*s (fixed by the host)", static_cast<int>(m_grant_text.size()), m_grant_text.data());

    char label[192];
    if (m_selected < m_commands.size())
    {
        command_label(m_commands[m_selected], granted(m_selected), label, sizeof(label));
    }
    else
    {
        (void)std::snprintf(label, sizeof(label), "%s", "(choose a command)");
    }
    if (ImGui::BeginCombo("command", label))
    {
        for (crd::u32 i = 0U; i < m_commands.size(); ++i)
        {
            command_label(m_commands[i], granted(i), label, sizeof(label));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label, i == m_selected))
            {
                m_selected = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (m_selected < m_commands.size())
    {
        const DiagCommandSpec& spec = m_commands[m_selected];
        ImGui::TextDisabled("%.*s (%.*s)%s", static_cast<int>(spec.summary.size()), spec.summary.data(),
                            static_cast<int>(spec.owner.size()), spec.owner.data(),
                            spec.takes_path ? "; takes a path" : "");
    }
    ImGui::InputText("path", m_path, kPathCapacity);
    ImGui::InputTextMultiline("arguments", m_args, kArgsCapacity, ImVec2(-1.0F, ImGui::GetTextLineHeight() * 4.0F));
    int page_items = static_cast<int>(m_page_items);
    if (ImGui::InputInt("page items (0: default)", &page_items))
    {
        m_page_items = page_items < 0 ? 0U : static_cast<crd::u32>(page_items);
    }
    int page_bytes = static_cast<int>(m_page_bytes);
    if (ImGui::InputInt("page bytes (0: default)", &page_bytes, 1024, 16384))
    {
        m_page_bytes = page_bytes < 0 ? 0U : static_cast<crd::u32>(page_bytes);
    }

    const bool running = m_state == DiagPanelState::Running;
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Run"))
    {
        (void)submit();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || m_state != DiagPanelState::Done || m_result.next_cursor == 0U);
    if (ImGui::Button("Next page"))
    {
        (void)next_page();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel"))
    {
        (void)cancel();
    }
    ImGui::EndDisabled();

    const cont::StringView state_text  = diag_panel_state_name(m_state);
    const cont::StringView submit_text = diag_panel_submit_name(m_last_submit);
    ImGui::Text("state: %.*s   last request: %.*s   answered: %llu", static_cast<int>(state_text.size()),
                state_text.data(), static_cast<int>(submit_text.size()), submit_text.data(),
                static_cast<unsigned long long>(m_completed));
    if (!m_form_error.empty())
    {
        wrapped(form_error());
    }
    if (m_state == DiagPanelState::Done)
    {
        const cont::StringView status = status_name(m_result.status);
        ImGui::Separator();
        ImGui::Text("%.*s: %.*s", static_cast<int>(status.size()), status.data(),
                    static_cast<int>(m_result.reason.size()), m_result.reason.data());
        ImGui::Text(
            "snapshot %llu  cursor %llu  next %llu  items %u of %u%s%s  dropped %u",
            static_cast<unsigned long long>(m_result.generation), static_cast<unsigned long long>(m_result.cursor),
            static_cast<unsigned long long>(m_result.next_cursor), m_result.items, m_result.total,
            m_result.complete ? "  complete" : "", m_result.byte_bounded ? "  byte-bounded" : "", m_result.dropped);
        if (!m_document_ok)
        {
            ImGui::TextUnformatted("the response document did not split; its text follows");
            wrapped(cont::StringView{m_result.json.data(), m_result.json.size()});
        }
        else
        {
            if (!m_document.summary.empty())
            {
                wrapped(m_document.summary);
            }
            if (ImGui::BeginChild("items", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders))
            {
                for (const cont::StringView item : m_document.items)
                {
                    wrapped(item);
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

} // namespace crd::perf::ui
