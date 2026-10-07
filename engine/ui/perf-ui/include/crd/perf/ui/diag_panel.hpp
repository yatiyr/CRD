#pragma once

// crd-perf-ui -- DiagCommandPanel: the GUI consumer of the typed diagnostic command service.
//
// A host builds one DiagCommandService (crd/perf/diag_commands.hpp), grants its authority, registers the commands it
// serves and hands the service to this panel. The panel is a form over the service's own request (command, path,
// named arguments, page bounds) and a view of its own response document: it sends DiagCommandService::execute the
// request and keeps the returned DiagResult unchanged, so the bytes it shows are the bytes a native caller, the ceridc
// verb and its MCP tool receive for the same request. It adds no command, no check and no authority. The grant is
// the host's, fixed when it built the service; nothing the panel's form holds can raise it.
//
// THE FRAME NEVER WAITS. A request runs on the panel's own worker thread, because a command may hold the service for
// its bounded run (program.inspect runs an authored program). `tick` polls for the answer without waiting and every
// accessor reads panel state only: the command listing is copied from the service when the panel is built and on
// `refresh_commands` while idle, so a frame never takes the service's lock either. `cancel` raises the cancel flag the
// running request was given; the service and the command's handler poll it. Destroying the panel cancels a running
// request and joins its thread.
//
// One request at a time: a submit while one runs is refused Busy before anything is sent. A host whose command reads
// state that only one thread may touch (gpu.resources over a frame graph) must not register that state with a service
// it gives this panel, since the worker thread calls the service.
//
// Paging: `submit` takes a new snapshot (cursor 0) from the form; `next_page` sends the snapshot's own request with the
// last page's next cursor, whatever the form holds now; `page_at` sends it with any cursor, so a cursor from a replaced
// snapshot is answered by the service's stale-cursor refusal. Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <atomic>
#include <thread>

namespace crd::perf::ui
{

// NOLINTNEXTLINE(performance-enum-size)
enum class DiagPanelState : crd::u8
{
    Idle = 0, // nothing sent yet
    Running,  // a request runs on the worker thread
    Done,     // the last request's answer is `result()`
};

// NOLINTNEXTLINE(performance-enum-size)
enum class DiagPanelSubmit : crd::u8
{
    Started = 0, // the request was sent; `tick` reports its answer
    Busy,        // a request is still running; nothing was sent
    NoCommand,   // no command is selected; nothing was sent
    BadForm,     // the argument text is not `name=value` lines (`form_error()` says where); nothing was sent
    NoMorePages, // no answered snapshot has a next page (or none was taken); nothing was sent
};

// NOLINTNEXTLINE(performance-enum-size)
enum class DiagPanelTick : crd::u8
{
    None = 0,  // still running, or nothing running
    Completed, // a request's answer arrived this tick (its thread is joined)
};

// "idle", "running", "done".
[[nodiscard]] containers::StringView diag_panel_state_name(DiagPanelState s) noexcept;
// "started", "busy", "no-command", "bad-form", "no-more-pages".
[[nodiscard]] containers::StringView diag_panel_submit_name(DiagPanelSubmit s) noexcept;

// The parts of one response document the panel shows: its summary object and one view per item of its "items" array,
// each the item's own object text. A refusal's summary is `{}` and it has no items. The views point into `json`.
struct DiagDocumentView
{
    explicit DiagDocumentView(memory::IAllocator* alloc = memory::default_allocator()) : items(alloc) {}

    containers::StringView                   summary;
    containers::Array<containers::StringView> items;
};

// Split a response document. Strings are skipped with their escapes, so an item value holding braces or the text
// `"items":[` never splits an item. Returns false, leaving `out` empty, when `json` is not one object of key/value
// pairs (nesting is tracked by depth only: the service writes the document, the splitter only cuts it).
[[nodiscard]] bool split_diag_document(containers::StringView json, DiagDocumentView& out);

class DiagCommandPanel
{
public:
    // Form limits of the panel's own text fields. They are larger than the service's bounds on purpose: a path or an
    // argument the service would refuse still reaches it and is refused there, with its reason.
    static constexpr crd::u32 kPathCapacity = 512U;
    static constexpr crd::u32 kArgsCapacity = 8192U;
    static constexpr crd::u32 kNoCommand    = 0xFFFFFFFFU;

    // `service` must outlive the panel. The command listing is copied here, so register every command first.
    DiagCommandPanel(DiagCommandService& service, memory::IAllocator* alloc = memory::default_allocator());
    ~DiagCommandPanel();

    DiagCommandPanel(const DiagCommandPanel&)            = delete;
    DiagCommandPanel& operator=(const DiagCommandPanel&) = delete;
    DiagCommandPanel(DiagCommandPanel&&)                 = delete;
    DiagCommandPanel& operator=(DiagCommandPanel&&)      = delete;

    // The listing as it was last copied from the service, in its order.
    [[nodiscard]] crd::u32 command_count() const noexcept { return static_cast<crd::u32>(m_commands.size()); }
    [[nodiscard]] const DiagCommandSpec& command(crd::u32 index) const noexcept { return m_commands[index]; }
    // Whether the host's grant covers the command (the service still checks it on every request).
    [[nodiscard]] bool granted(crd::u32 index) const noexcept;
    // Copy the listing again. Returns false (keeping the old copy) while a request runs.
    bool refresh_commands();

    // The form. Returns false, changing nothing, for an unknown command or text longer than the panel's field.
    bool select(containers::StringView name);
    bool set_path(containers::StringView path);
    // One `name=value` per line; empty lines are skipped. Parsed when a request is sent.
    bool set_args(containers::StringView text);
    void set_page_items(crd::u32 items) noexcept { m_page_items = items; }
    void set_page_bytes(crd::u32 bytes) noexcept { m_page_bytes = bytes; }

    [[nodiscard]] crd::u32               selected() const noexcept { return m_selected; }
    [[nodiscard]] containers::StringView path() const noexcept;
    [[nodiscard]] containers::StringView args() const noexcept;

    // Send a request (see the header). Never waits.
    [[nodiscard]] DiagPanelSubmit submit();
    [[nodiscard]] DiagPanelSubmit next_page();
    [[nodiscard]] DiagPanelSubmit page_at(crd::u64 cursor);

    // Raise the running request's cancel flag. Returns false when nothing runs.
    bool cancel() noexcept;

    // Once per frame. Never waits on the running request.
    DiagPanelTick tick();

    [[nodiscard]] DiagPanelState          state() const noexcept { return m_state; }
    [[nodiscard]] const DiagResult&       result() const noexcept { return m_result; }
    [[nodiscard]] const DiagDocumentView& document() const noexcept { return m_document; }
    [[nodiscard]] bool                    document_ok() const noexcept { return m_document_ok; }
    [[nodiscard]] containers::StringView  form_error() const noexcept;
    [[nodiscard]] DiagPanelSubmit         last_submit() const noexcept { return m_last_submit; }
    [[nodiscard]] crd::u64                completed() const noexcept { return m_completed; }
    // The command of the snapshot the shown answer belongs to (empty before any request).
    [[nodiscard]] containers::StringView sent_command() const noexcept
    {
        return containers::StringView{m_sent_command.data(), m_sent_command.size()};
    }

    // Draw the panel's window. Call inside an ImGui frame; the host ticks the panel separately.
    void draw();

private:
    [[nodiscard]] DiagPanelSubmit send(crd::u64 cursor);
    [[nodiscard]] bool            parse_args(containers::StringView text, containers::Array<DiagArg>* out);
    void                          join() noexcept;

    DiagCommandService*                  m_service;
    memory::IAllocator*                  m_alloc;
    containers::Array<DiagCommandSpec>   m_commands;
    containers::String                   m_grant_text;
    crd::u32                             m_selected = kNoCommand;
    char                                 m_path[kPathCapacity]{};
    char                                 m_args[kArgsCapacity]{};
    crd::u32                             m_page_items = 0U;
    crd::u32                             m_page_bytes = 0U;
    containers::String                   m_form_error;
    DiagPanelSubmit                      m_last_submit = DiagPanelSubmit::NoCommand;

    // The snapshot's request: what `submit` sent, kept for its later pages. The worker reads it while it runs.
    containers::String                   m_sent_command;
    containers::String                   m_sent_path;
    containers::String                   m_sent_args_text;
    containers::Array<DiagArg>           m_sent_args;
    bool                                 m_sent_valid = false;
    DiagRequest                          m_request;

    DiagPanelState                       m_state = DiagPanelState::Idle;
    DiagResult                           m_pending; // written by the worker only
    DiagResult                           m_result;
    DiagDocumentView                     m_document;
    bool                                 m_document_ok = false;
    crd::u64                             m_completed   = 0U;
    std::atomic<bool>                    m_cancel{false};
    std::atomic<bool>                    m_done{false};
    std::thread                          m_worker; // last: it runs while the members above are alive
};

} // namespace crd::perf::ui
