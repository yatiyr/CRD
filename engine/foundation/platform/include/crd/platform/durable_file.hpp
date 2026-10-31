#pragma once

// crd-platform -- durable saves (HW-RESILIENCE.1): the atomic replace, the generational checksummed save, the journal
// and the autosave cadence every saving writer builds on. DOC.2 applies them to every document format; the cooker's
// database, artifacts, pack and journal use them today, and crd-perf's bundle publish moves onto them under OWN.1.
//
// - write_file_atomic: a reader sees the old file or the new one, never a mix, and the new one survives a power loss
//   once the call returns (stable-storage flush before the rename, the directory synced after it on POSIX).
// - save_generation / load_generation: each save is one file with a 40-byte envelope (magic "CRDS", version, the
//   save's generation, the payload's size and XXH3-64, the CRC-32C of the header). The newest save is `path`; the
//   `kept` previous generations rotate through `path.1` .. `path.<kept>`; a save in flight is `path.tmp`. A load
//   delivers the newest generation that verifies, reading a file once more when a check fails (a fault in transfer
//   is not a fault in storage), and reports why it fell back to an older one.
// - Journal / replay_journal: append-only records, each with its sequence number, size, XXH3-64 and a CRC-32C of its
//   header, after a header naming the snapshot generation the records apply to. Replay delivers the records up to
//   the first one that fails (a crash during an append, or damage) and says how many bytes were lost; opening the
//   journal again cuts that tail before appending.
// - Autosave: when to commit the journal (the most work a crash can cost) and when to fold it into a new snapshot.
//
// Every read goes through the I/O fault seam (crd/core/fault_seam.hpp), so tests drive the re-read and recovery
// paths with injected flips. Contract: docs/design/runtime-diagnostics.md#hardware-fault-resilience.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/platform/filesystem.hpp>

namespace crd::platform::fs
{

// Replace `path` with `contents` atomically and durably through `<path>.tmp`. False leaves `path` as it was.
[[nodiscard]] bool write_file_atomic(const Path& path, containers::ConstSpan<u8> contents) noexcept;

// ---- generational saves ----------------------------------------------------------------------------------------

inline constexpr usize kSaveHeaderBytes = 40U;

enum class SaveStatus : u8
{
    Ok = 0U,        // `path` (or a complete save interrupted before its rename) verified
    Recovered = 1U, // `path` is missing or failed its checks (`fault`); an older generation verified
    NotFound = 2U,  // no file of the save exists
    Corrupt = 3U,   // files of the save exist and none verifies
};

enum class SaveFault : u8
{
    None = 0U,
    Missing = 1U,        // `path` does not exist while older generations do
    Unreadable = 2U,     // the file exists and cannot be read
    Truncated = 3U,      // shorter than its header, or than the payload its header declares
    HeaderCorrupt = 4U,  // wrong magic, version or reserved bytes, or the header's CRC-32C does not match
    PayloadCorrupt = 5U, // the payload's XXH3-64 does not match its header
};

struct SaveLoad
{
    SaveStatus status = SaveStatus::NotFound;
    u64 generation = 0U;               // of the delivered payload; 0 when none is delivered
    u32 rereads = 0U;                  // files read once more after a failed check
    SaveFault fault = SaveFault::None; // why `path` was not delivered (Recovered and Corrupt)
    Path source{};                     // the file delivered
};

struct SaveWrite
{
    bool ok = false;
    u64 generation = 0U; // of the save written (one past the newest generation found)
};

// Save `payload` as the newest generation of `path`, keeping `kept` previous generations. A save interrupted after
// its bytes were durable is completed first. False leaves every existing generation readable.
[[nodiscard]] SaveWrite save_generation(const Path& path, containers::ConstSpan<u8> payload, u32 kept) noexcept;

// Load the newest generation of `path` that verifies into `payload` (cleared when none does).
[[nodiscard]] SaveLoad load_generation(const Path& path, u32 kept, containers::Array<u8>& payload) noexcept;

[[nodiscard]] const char* save_fault_name(SaveFault fault) noexcept;

// A sentence for the user: what failed, where, and what was loaded instead (empty for a plain Ok).
[[nodiscard]] containers::String save_load_message(const Path& path, const SaveLoad& load);

// ---- journal ---------------------------------------------------------------------------------------------------

inline constexpr usize kJournalHeaderBytes = 32U;
inline constexpr usize kJournalRecordHeaderBytes = 24U;

enum class JournalStatus : u8
{
    Ok = 0U,            // every byte after the header is a valid record
    TornTail = 1U,      // the records before the first failing one are delivered; `lost_bytes` from it on are not
    NotFound = 2U,      // no journal
    OtherBase = 3U,     // the journal applies to another snapshot generation: nothing delivered
    HeaderCorrupt = 4U, // the journal's header fails its checks: nothing delivered
    Unreadable = 5U,    // the journal exists and cannot be read
};

struct JournalRecord
{
    u64 sequence = 0U; // 1 for the first record after the header, then one more per record
    usize offset = 0U; // of the payload in JournalReplay::bytes
    usize size = 0U;
};

struct JournalReplay
{
    JournalStatus status = JournalStatus::NotFound;
    u64 base_generation = 0U; // the header's (0 when unreadable)
    u64 valid_bytes = 0U;     // the header and every delivered record
    u64 lost_bytes = 0U;      // after them (TornTail; all of them: HeaderCorrupt)
    u32 rereads = 0U;
    containers::Array<u8> bytes{}; // the journal as read
    containers::Array<JournalRecord> records{};
};

// Read the records of the journal at `path` that apply to the snapshot of generation `base_generation`.
[[nodiscard]] JournalReplay replay_journal(const Path& path, u64 base_generation) noexcept;

class Journal
{
public:
    Journal() noexcept = default;
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&& other) noexcept;
    Journal& operator=(Journal&& other) noexcept;

    // Append after the snapshot of generation `base_generation`: the valid records of a journal of that base are
    // kept and its torn tail is cut; any other journal is replaced by an empty one (replay it first: its records are
    // in a newer snapshot already, or cannot apply to this one).
    [[nodiscard]] bool open(const Path& path, u64 base_generation) noexcept;

    // The same with the replay of `path` the caller already read (replay_journal(path, base_generation)), so the
    // journal is read once.
    [[nodiscard]] bool open(const Path& path, u64 base_generation, const JournalReplay& existing) noexcept;

    // Write one record. It is durable after the next commit(). A failed write cuts its partial record off; a journal
    // that cannot cut it closes, since an append after those bytes would follow where replay stops.
    [[nodiscard]] bool append(containers::ConstSpan<u8> record) noexcept;

    // Flush every appended record to stable storage.
    [[nodiscard]] bool commit() noexcept;

    // After a snapshot of generation `base_generation` holds every record: atomically start an empty journal for it.
    [[nodiscard]] bool reset(u64 base_generation) noexcept;

    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return m_file != kNoFile; }
    [[nodiscard]] u64 bytes() const noexcept { return m_bytes; }
    [[nodiscard]] u64 next_sequence() const noexcept { return m_next_sequence; }

private:
    static constexpr i64 kNoFile = -1;

    Path m_path{};
    i64 m_file = kNoFile; // the OS handle (Windows) or descriptor (POSIX)
    u64 m_bytes = 0U;
    u64 m_next_sequence = 1U;
};

// ---- autosave --------------------------------------------------------------------------------------------------

struct AutosavePolicy
{
    u64 commit_interval_ns = 2'000'000'000ULL;     // the most unsaved work a crash can cost
    u64 snapshot_interval_ns = 300'000'000'000ULL; // fold the journal into a snapshot at least this often
    u64 snapshot_journal_bytes = 64ULL << 20U;     // or once the journal reaches this size
};

enum class AutosaveStep : u8
{
    None = 0U,
    Commit = 1U,   // Journal::commit
    Snapshot = 2U, // save_generation, then Journal::reset
};

// The cadence of one document's saves on the caller's clock (TickClock nanoseconds).
class Autosave
{
public:
    Autosave(const AutosavePolicy& policy, u64 now_ns) noexcept;

    // A change was appended to the journal.
    void changed(u64 now_ns) noexcept;

    // What is due now; a snapshot also covers a pending commit.
    [[nodiscard]] AutosaveStep due(u64 now_ns, u64 journal_bytes) const noexcept;

    // `step` was performed at `now_ns`.
    void done(AutosaveStep step, u64 now_ns) noexcept;

private:
    AutosavePolicy m_policy{};
    u64 m_first_uncommitted_ns = 0U;
    u64 m_last_snapshot_ns = 0U;
    bool m_uncommitted = false;
    bool m_unsnapshotted = false;
};

} // namespace crd::platform::fs
