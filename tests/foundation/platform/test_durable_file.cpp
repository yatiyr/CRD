// Durable saves (HW-RESILIENCE.1): the atomic replace, generational saves, the journal and the autosave cadence,
// with every bit of a save and a journal flipped on disk and in transfer, and every torn length.

#include <crd/core/fault_seam.hpp>
#include <crd/platform/durable_file.hpp>
#include <crd/platform/threading.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <utility>

namespace fs = crd::platform::fs;
namespace cont = crd::containers;

namespace
{
struct TempRoot
{
    fs::Path path;

    TempRoot()
    {
        const auto stamp = static_cast<crd::u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        char leaf[64];
        (void)std::snprintf(leaf, sizeof(leaf), "crd_durable_%llu_%llu", static_cast<unsigned long long>(stamp),
                            static_cast<unsigned long long>(crd::platform::threading::current_thread_id()));
        path = fs::temp_directory() / leaf;
        REQUIRE(fs::create_directories(path));
    }
    ~TempRoot() { (void)fs::remove_all(path); }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
};

[[nodiscard]] cont::Array<crd::u8> bytes_of(std::string_view text)
{
    cont::Array<crd::u8> out;
    for (const char c : text)
    {
        out.push_back(static_cast<crd::u8>(c));
    }
    return out;
}

[[nodiscard]] cont::ConstSpan<crd::u8> span_of(const cont::Array<crd::u8>& bytes)
{
    return cont::ConstSpan<crd::u8>{bytes.data(), bytes.size()};
}

[[nodiscard]] bool same(const cont::Array<crd::u8>& a, std::string_view b)
{
    return a.size() == b.size() &&
           (b.empty() || std::string_view(reinterpret_cast<const char*>(a.data()), a.size()) == b);
}

[[nodiscard]] fs::Path suffixed(const fs::Path& path, const char* suffix)
{
    cont::String name(path.generic());
    name.append(suffix);
    return fs::Path{std::move(name)};
}

[[nodiscard]] cont::Array<crd::u8> read_all(const fs::Path& path)
{
    cont::Array<crd::u8> out;
    REQUIRE(fs::read_file_binary(path, out));
    return out;
}

void write_all(const fs::Path& path, const cont::Array<crd::u8>& bytes, crd::usize count)
{
    REQUIRE(fs::write_file_binary(path, cont::ConstSpan<crd::u8>{bytes.data(), count}));
}

constexpr std::string_view kSaves[] = {"first save", "second save, longer", "third", "fourth save", "fifth save!"};

// Saves kSaves[0..count) as generations 1..count of `path`, keeping two older generations.
void save_all(const fs::Path& path, crd::usize count)
{
    for (crd::usize i = 0U; i < count; ++i)
    {
        const fs::SaveWrite w = fs::save_generation(path, span_of(bytes_of(kSaves[i])), 2U);
        REQUIRE(w.ok);
        REQUIRE(w.generation == i + 1U);
    }
}

// Flips bit `bit` of reads of one file through the I/O fault seam, `remaining` times.
struct FlipReads
{
    cont::String target;
    crd::u64 bit = 0U;
    crd::u32 remaining = 0U;
    crd::u32 flipped = 0U;
    bool installed = false;

    FlipReads(const fs::Path& path, crd::u64 flip_bit, crd::u32 times)
        : target(path.generic()), bit(flip_bit), remaining(times),
          installed(crd::install_io_fault_hook(&FlipReads::hook, this))
    {
    }
    ~FlipReads()
    {
        if (installed)
        {
            crd::uninstall_io_fault_hook();
        }
    }
    FlipReads(const FlipReads&) = delete;
    FlipReads& operator=(const FlipReads&) = delete;

    static crd::IoFault hook(void* user, crd::IoFaultOp, const char* path, crd::usize path_bytes, crd::u64,
                             crd::u64) noexcept
    {
        auto* self = static_cast<FlipReads*>(user);
        if (self->remaining == 0U ||
            std::string_view(path, path_bytes) != std::string_view(self->target.data(), self->target.size()))
        {
            return crd::IoFault{};
        }
        --self->remaining;
        ++self->flipped;
        return crd::IoFault{crd::IoFaultAction::Flip, self->bit};
    }
};
} // namespace

TEST_CASE("durable: write_file_atomic replaces a file and leaves no temporary", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path file = root.path / "settings.bin";
    REQUIRE(fs::write_file_atomic(file, span_of(bytes_of("old"))));
    REQUIRE(fs::write_file_atomic(file, span_of(bytes_of("new contents"))));
    CHECK(same(read_all(file), "new contents"));
    CHECK_FALSE(fs::exists(suffixed(file, ".tmp")));

    // A directory that does not exist: nothing is written and nothing is left behind.
    const fs::Path missing = root.path / "absent" / "settings.bin";
    CHECK_FALSE(fs::write_file_atomic(missing, span_of(bytes_of("x"))));
    CHECK_FALSE(fs::exists(missing));
}

TEST_CASE("durable: generations rotate and the newest loads", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";

    cont::Array<crd::u8> payload;
    fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::NotFound);
    CHECK(payload.empty());

    save_all(doc, 5U);
    load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Ok);
    CHECK(load.generation == 5U);
    CHECK(load.rereads == 0U);
    CHECK(same(payload, kSaves[4]));
    CHECK(fs::save_load_message(doc, load).empty());

    // Generation g sits in slot g % 2 + 1: 4 in .1, 3 in .2; nothing older survives.
    CHECK_FALSE(fs::exists(suffixed(doc, ".tmp")));
    REQUIRE(fs::remove_file(doc));
    load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Recovered);
    CHECK(load.fault == fs::SaveFault::Missing);
    CHECK(load.generation == 4U);
    CHECK(load.source == suffixed(doc, ".1"));
    CHECK(same(payload, kSaves[3]));
    REQUIRE(fs::remove_file(suffixed(doc, ".1")));
    load = fs::load_generation(doc, 2U, payload);
    CHECK(load.generation == 3U);
    CHECK(same(payload, kSaves[2]));
}

TEST_CASE("durable: every flipped bit of the newest save falls back to the previous one", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 5U);
    const cont::Array<crd::u8> original = read_all(doc);

    cont::Array<crd::u8> payload;
    for (crd::usize bit = 0U; bit < original.size() * 8U; ++bit)
    {
        cont::Array<crd::u8> damaged(original);
        damaged[bit / 8U] = static_cast<crd::u8>(damaged[bit / 8U] ^ (1U << (bit % 8U)));
        write_all(doc, damaged, damaged.size());
        const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
        INFO("bit " << bit);
        REQUIRE(load.status == fs::SaveStatus::Recovered);
        REQUIRE(load.generation == 4U);
        REQUIRE(same(payload, kSaves[3]));
        REQUIRE((load.fault == fs::SaveFault::HeaderCorrupt || load.fault == fs::SaveFault::PayloadCorrupt));
        REQUIRE(load.rereads >= 1U); // each failing read was repeated once before falling back
        REQUIRE_FALSE(fs::save_load_message(doc, load).empty());
    }

    // Every shorter length of the newest save.
    for (crd::usize length = 0U; length < original.size(); ++length)
    {
        write_all(doc, original, length);
        const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
        INFO("length " << length);
        REQUIRE(load.status == fs::SaveStatus::Recovered);
        REQUIRE(load.fault == fs::SaveFault::Truncated);
        REQUIRE(same(payload, kSaves[3]));
    }
}

TEST_CASE("durable: a flip in transfer is read again; a flip in storage falls back", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 5U);
    cont::Array<crd::u8> payload;

    if (!crd::fault_seams_compiled())
    {
        const FlipReads flips(doc, 0U, 1U);
        CHECK_FALSE(flips.installed);
        CHECK(fs::load_generation(doc, 2U, payload).status == fs::SaveStatus::Ok);
        return;
    }

    for (const crd::u64 bit : {0ULL, 77ULL, 300ULL, 333ULL, 400ULL})
    {
        // One flipped read (the header's or the whole file's): the second read verifies.
        {
            const FlipReads flips(doc, bit, 1U);
            REQUIRE(flips.installed);
            const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
            CHECK(flips.flipped == 1U);
            CHECK(load.status == fs::SaveStatus::Ok);
            CHECK(load.rereads == 1U);
            CHECK(load.generation == 5U);
            CHECK(same(payload, kSaves[4]));
            CHECK_FALSE(fs::save_load_message(doc, load).empty());
        }
        // Every read flipped: the previous generation loads.
        {
            const FlipReads flips(doc, bit, 1000U);
            REQUIRE(flips.installed);
            const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
            CHECK(load.status == fs::SaveStatus::Recovered);
            CHECK(load.generation == 4U);
            CHECK(same(payload, kSaves[3]));
        }
    }
}

TEST_CASE("durable: no verifying generation is a typed error with an empty payload", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 3U);
    for (const char* name : {"", ".1", ".2"})
    {
        const fs::Path file = suffixed(doc, name);
        cont::Array<crd::u8> bytes = read_all(file);
        bytes[bytes.size() - 1U] = static_cast<crd::u8>(bytes[bytes.size() - 1U] ^ 0x10U);
        write_all(file, bytes, bytes.size());
    }
    cont::Array<crd::u8> payload = bytes_of("stale");
    const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Corrupt);
    CHECK(load.fault == fs::SaveFault::PayloadCorrupt);
    CHECK(load.generation == 0U);
    CHECK(payload.empty());
    const cont::String message = fs::save_load_message(doc, load);
    CHECK(std::string_view(message.data(), message.size()).find("no previous save verifies") != std::string_view::npos);
}

TEST_CASE("durable: a save interrupted before its rename loads and is completed", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 3U);
    // The crash point between "the old main file went into the ring" and "the new save took its name".
    REQUIRE(fs::rename_file(doc, suffixed(doc, ".tmp")));

    cont::Array<crd::u8> payload;
    fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Ok);
    CHECK(load.generation == 3U);
    CHECK(load.source == suffixed(doc, ".tmp"));
    CHECK(same(payload, kSaves[2]));

    const fs::SaveWrite w = fs::save_generation(doc, span_of(bytes_of(kSaves[3])), 2U);
    REQUIRE(w.ok);
    CHECK(w.generation == 4U);
    CHECK_FALSE(fs::exists(suffixed(doc, ".tmp")));
    load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Ok);
    CHECK(load.generation == 4U);
    // The completed save is now in the ring: losing the main file delivers it.
    REQUIRE(fs::remove_file(doc));
    load = fs::load_generation(doc, 2U, payload);
    CHECK(load.generation == 3U);
    CHECK(same(payload, kSaves[2]));
}

TEST_CASE("durable: a torn save in flight never displaces the saved one", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 3U);
    const cont::Array<crd::u8> third = read_all(doc);
    const cont::Array<crd::u8> second = read_all(suffixed(doc, ".1"));

    cont::Array<crd::u8> payload;
    for (crd::usize length = 0U; length < third.size(); ++length)
    {
        write_all(doc, second, second.size());
        write_all(suffixed(doc, ".tmp"), third, length);
        const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
        INFO("length " << length);
        REQUIRE(load.status == fs::SaveStatus::Ok);
        REQUIRE(load.generation == 2U);
        REQUIRE(same(payload, kSaves[1]));
    }
}

TEST_CASE("durable: a damaged main file is kept aside, never rotated over a good generation", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path doc = root.path / "doc.crds";
    save_all(doc, 3U);
    cont::Array<crd::u8> bytes = read_all(doc);
    bytes[0] = static_cast<crd::u8>(bytes[0] ^ 1U);
    write_all(doc, bytes, bytes.size());

    const fs::SaveWrite w = fs::save_generation(doc, span_of(bytes_of(kSaves[3])), 2U);
    REQUIRE(w.ok);
    CHECK(w.generation == 3U); // one past the newest generation that verifies
    CHECK(fs::exists(suffixed(doc, ".damaged")));

    cont::Array<crd::u8> payload;
    CHECK(fs::load_generation(doc, 2U, payload).generation == 3U);
    CHECK(same(payload, kSaves[3]));
    REQUIRE(fs::remove_file(doc));
    const fs::SaveLoad load = fs::load_generation(doc, 2U, payload);
    CHECK(load.status == fs::SaveStatus::Recovered);
    CHECK(load.generation == 2U);
    CHECK(same(payload, kSaves[1]));
}

TEST_CASE("durable: the journal replays its records and cuts a torn tail", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path log = root.path / "doc.journal";

    CHECK(fs::replay_journal(log, 7U).status == fs::JournalStatus::NotFound);
    {
        fs::Journal journal;
        REQUIRE(journal.open(log, 7U));
        for (const std::string_view record : kSaves)
        {
            REQUIRE(journal.append(span_of(bytes_of(record))));
        }
        REQUIRE(journal.commit());
        CHECK(journal.next_sequence() == 6U);
    }
    const auto check_prefix = [](const fs::JournalReplay& replay)
    {
        for (crd::usize i = 0U; i < replay.records.size(); ++i)
        {
            const fs::JournalRecord& r = replay.records[i];
            REQUIRE(r.sequence == i + 1U);
            REQUIRE(std::string_view(reinterpret_cast<const char*>(replay.bytes.data() + r.offset), r.size) ==
                    kSaves[i]);
        }
    };
    fs::JournalReplay replay = fs::replay_journal(log, 7U);
    REQUIRE(replay.status == fs::JournalStatus::Ok);
    REQUIRE(replay.records.size() == 5U);
    check_prefix(replay);
    CHECK(fs::replay_journal(log, 8U).status == fs::JournalStatus::OtherBase);

    const cont::Array<crd::u8> original = read_all(log);
    // Every flipped bit: a prefix of the records, never a changed one, and never Ok.
    for (crd::usize bit = 0U; bit < original.size() * 8U; ++bit)
    {
        cont::Array<crd::u8> damaged(original);
        damaged[bit / 8U] = static_cast<crd::u8>(damaged[bit / 8U] ^ (1U << (bit % 8U)));
        write_all(log, damaged, damaged.size());
        replay = fs::replay_journal(log, 7U);
        INFO("bit " << bit);
        REQUIRE((replay.status == fs::JournalStatus::TornTail || replay.status == fs::JournalStatus::HeaderCorrupt));
        REQUIRE(replay.records.size() < 5U);
        REQUIRE(replay.rereads == 1U);
        check_prefix(replay);
    }

    // Every torn length: the whole records before it replay, and appending continues after them.
    for (crd::usize length = 0U; length < original.size(); ++length)
    {
        write_all(log, original, length);
        replay = fs::replay_journal(log, 7U);
        INFO("length " << length);
        check_prefix(replay);
        const crd::usize kept = replay.records.size();
        REQUIRE(replay.valid_bytes + replay.lost_bytes == length);
        fs::Journal journal;
        REQUIRE(journal.open(log, 7U));
        REQUIRE(journal.next_sequence() == kept + 1U);
        REQUIRE(journal.append(span_of(bytes_of(kSaves[kept]))));
        REQUIRE(journal.commit());
        journal.close();
        replay = fs::replay_journal(log, 7U);
        REQUIRE(replay.status == fs::JournalStatus::Ok);
        REQUIRE(replay.records.size() == kept + 1U);
        check_prefix(replay);
    }
}

TEST_CASE("durable: a journal reset after a snapshot starts the next base", "[platform][durable]")
{
    const TempRoot root;
    const fs::Path log = root.path / "doc.journal";
    fs::Journal journal;
    REQUIRE(journal.open(log, 3U));
    REQUIRE(journal.append(span_of(bytes_of("edit"))));
    REQUIRE(journal.commit());
    REQUIRE(journal.reset(4U));
    CHECK(journal.next_sequence() == 1U);
    CHECK(journal.bytes() == fs::kJournalHeaderBytes);
    REQUIRE(journal.append(span_of(bytes_of("after"))));
    REQUIRE(journal.commit());
    journal.close();
    CHECK(fs::replay_journal(log, 3U).status == fs::JournalStatus::OtherBase);
    const fs::JournalReplay replay = fs::replay_journal(log, 4U);
    REQUIRE(replay.status == fs::JournalStatus::Ok);
    REQUIRE(replay.records.size() == 1U);

    // Opening for another base replaces the journal (its records are in a snapshot already).
    REQUIRE(journal.open(log, 5U));
    journal.close();
    CHECK(fs::replay_journal(log, 5U).records.empty());

    if (crd::fault_seams_compiled())
    {
        const FlipReads flips(log, 5U, 1U);
        REQUIRE(flips.installed);
        const fs::JournalReplay again = fs::replay_journal(log, 5U);
        CHECK(again.status == fs::JournalStatus::Ok);
        CHECK(again.rereads == 1U);
    }
}

TEST_CASE("durable: autosave commits within its interval and snapshots by age or size", "[platform][durable]")
{
    constexpr crd::u64 second = 1'000'000'000ULL;
    fs::AutosavePolicy policy;
    policy.commit_interval_ns = 2U * second;
    policy.snapshot_interval_ns = 60U * second;
    policy.snapshot_journal_bytes = 1000U;
    fs::Autosave autosave(policy, 0U);

    CHECK(autosave.due(100U * second, 0U) == fs::AutosaveStep::None); // nothing changed
    autosave.changed(1U * second);
    autosave.changed(2U * second);
    CHECK(autosave.due(2U * second, 10U) == fs::AutosaveStep::None);
    CHECK(autosave.due(3U * second, 10U) == fs::AutosaveStep::Commit); // two seconds after the first change
    autosave.done(fs::AutosaveStep::Commit, 3U * second);
    CHECK(autosave.due(4U * second, 10U) == fs::AutosaveStep::None);
    CHECK(autosave.due(4U * second, 1000U) == fs::AutosaveStep::Snapshot); // the journal reached its size
    CHECK(autosave.due(61U * second, 10U) == fs::AutosaveStep::Snapshot);  // the last snapshot is a minute old
    autosave.done(fs::AutosaveStep::Snapshot, 61U * second);
    CHECK(autosave.due(200U * second, 5000U) == fs::AutosaveStep::None);
}
