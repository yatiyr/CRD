// DIAG.5a (d): a hang-triggered NON-FATAL live dump, end to end. When the progress-sensitive watchdog fires (here:
// work queued with no executor -> ExecutorStarved), a host-installed hang handler calls crd::crash::capture_dump(
// {Hang, evidence}) from the watchdog thread. MiniDumpWriteDump suspends the other threads and walks their stacks
// safely -- so the cooperative snapshot names WHAT is stuck and this dump shows WHERE, with the hang evidence riding
// along as a user stream. This is never a default action: jobs does not dump on hang; the host (this test) does. The
// process keeps running afterwards.
//
// The crash handler is already installed process-wide by main_diag.cpp; this test uses that ambient handler rather
// than install/uninstall of its own, which would fight Catch2's per-test SEH filter. The dump lands in the ambient
// output dir; the one file this test creates is deleted at the end.
#include <crd/core/crash.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

using crd::crash::WriteResult;

namespace
{
// A compact POD snapshot of the HangReport scalars, embedded as the dump's evidence stream (never the report's
// span, whose pointer would dangle outside the handler call).
struct HangEvidence
{
    crd::u32 stale_windows;
    crd::u32 executing;
    crd::u32 outstanding;
    crd::u32 parked_total;
    crd::i32 kind;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool>     g_captured{false};   // capture on the FIRST fire only -> exactly one dump
std::atomic<bool>     g_dump_done{false};  // main waits on this (no allocation while it spins)
std::atomic<int>      g_dump_result{-1};   // the WriteResult the handler observed
std::atomic<unsigned> g_evidence_bytes{0}; // what the handler embedded
std::wstring          g_dump_path;         // assigned after capture_dump returns (threads already resumed)
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void tick_job(void* /*data*/) noexcept {}

// Runs on the watchdog thread when the hang fires.
void hang_dumps(const crd::jobs::HangReport& report, void* /*user*/) noexcept
{
    if (g_captured.exchange(true, std::memory_order_acq_rel))
        return; // one dump per episode

    HangEvidence ev{};
    ev.stale_windows = report.stale_windows;
    ev.executing     = report.executing;
    ev.outstanding   = report.outstanding;
    ev.parked_total  = static_cast<crd::u32>(report.parked_total);
    ev.kind          = static_cast<crd::i32>(report.kind);

    crd::crash::DumpNote note{};
    note.kind           = crd::crash::DumpKind::Hang;
    note.evidence       = &ev;
    note.evidence_bytes = static_cast<std::uint32_t>(sizeof(ev));

    const wchar_t*    path = nullptr;
    const WriteResult wr   = crd::crash::capture_dump(note, &path); // suspends the other threads, walks their stacks
    g_dump_result.store(static_cast<int>(wr), std::memory_order_relaxed);
    g_evidence_bytes.store(note.evidence_bytes, std::memory_order_relaxed);
    if (wr == WriteResult::Ok && path != nullptr)
        g_dump_path.assign(path); // safe: capture_dump has returned, all threads resumed
    g_dump_done.store(true, std::memory_order_release);
}
} // namespace

TEST_CASE("hang dump: a watchdog-fired hang writes one non-fatal dump with evidence", "[jobs][diag][hang-dump]")
{
    g_captured.store(false, std::memory_order_relaxed);
    g_dump_done.store(false, std::memory_order_relaxed);
    g_dump_result.store(-1, std::memory_order_relaxed);
    g_evidence_bytes.store(0, std::memory_order_relaxed);
    g_dump_path.clear();

    crd::jobs::set_hang_handler(&hang_dumps, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 1U;  // main is the only executor and will not pump -> the queued job is starved
    cfg.hang_watchdog_period_ms = 20U; // fire quickly
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn                       = &tick_job;
    crd::jobs::Counter* handle = crd::jobs::run(j); // queued, no executor -> ExecutorStarved hang

    // Wait for the handler to finish its dump. A plain atomic spin (no allocation) so a suspend-all mid-dump can
    // never catch this thread holding the CRT heap lock.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_dump_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    REQUIRE(g_dump_done.load(std::memory_order_acquire));
    CHECK(g_dump_result.load(std::memory_order_relaxed) == static_cast<int>(WriteResult::Ok));
    CHECK(g_evidence_bytes.load(std::memory_order_relaxed) == static_cast<unsigned>(sizeof(HangEvidence)));

    REQUIRE_FALSE(g_dump_path.empty());
    const fs::path dump{g_dump_path};
    CHECK(dump.filename().wstring().rfind(L"hang_", 0) == 0); // the Hang prefix
    CHECK(fs::exists(dump));
    CHECK(fs::file_size(dump) > 0U);

    // The evidence stream round-trips out of the live dump.
    HangEvidence      back{};
    const std::size_t n =
        crd::crash::read_dump_stream(g_dump_path.c_str(), crd::crash::kEvidenceStreamType, &back, sizeof(back));
    CHECK(n == sizeof(HangEvidence));
    CHECK(back.kind == static_cast<crd::i32>(crd::jobs::HangKind::ExecutorStarved));
    CHECK(back.executing == 0U); // nobody executing in an executor-starved hang

    (void)crd::jobs::pump_main_thread_until_idle(); // run the starved job so shutdown is clean
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);

    std::error_code ec;
    fs::remove(dump, ec); // delete the one dump this test created (ambient dir is otherwise left as found)
}
