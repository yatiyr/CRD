#pragma once

// cook_db.hpp — GEO-6 (D-007 row 71): the persistent DEPENDENCY GRAPH (source → job → product) + the crash-safe
// journal. One TOML document (`.cook_cache/cookdb.toml`) records, per source job: the recorded input edges (path +
// content hash — what CookIO saw), the handler version, and every product (id, type, name, artifact hash). The
// processor decides an incremental skip by re-hashing EXACTLY the recorded inputs — the handler never runs on a
// hit. The graph is QUERYABLE (forward: source → products; reverse: which jobs consume a file, which job made a
// product) — the surface GEO-11 agents and the editor ride.
//
// Crash safety: `journal_begin(source)` is appended DURABLY before a job's artifacts are written;
// `journal_commit(source)` after they land. A run that died mid-job leaves a dangling `begin` — on the next run
// those sources are DISTRUSTED (their cache may be torn) and recook unconditionally. The database file itself is
// replaced atomically (write-temp → rename), so a crash anywhere leaves either the old or the new database, never
// a torn one.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::cooker
{

struct DbInput
{
    crd::containers::String path; // ROOT-relative ('/'-separated)
    crd::u64                content_hash = 0;
    bool                    existed      = false;

    explicit DbInput(crd::memory::IAllocator* a) : path(a) {}
    DbInput(DbInput&&)            = default;
    DbInput& operator=(DbInput&&) = default;
};

struct DbProduct
{
    crd::resources::ResourceId id;
    crd::u32                   type_fourcc   = 0;
    crd::containers::String    name;          // display name ("model.glb#Cube")
    crd::u64                   artifact_hash = 0;

    explicit DbProduct(crd::memory::IAllocator* a) : name(a) {}
    DbProduct(DbProduct&&)            = default;
    DbProduct& operator=(DbProduct&&) = default;
};

struct DbJob
{
    crd::containers::String                            source; // ROOT-relative
    crd::u32                                           handler_version = 0;
    crd::containers::Array<DbInput>                    inputs;
    crd::containers::Array<DbProduct>                  products;
    crd::containers::Array<crd::resources::ResourceId> runtime_deps; // product→product references (e.g. material→shader)

    explicit DbJob(crd::memory::IAllocator* a) : source(a), inputs(a), products(a), runtime_deps(a) {}
    DbJob(DbJob&&)            = default;
    DbJob& operator=(DbJob&&) = default;
};

class CookDb
{
public:
    explicit CookDb(crd::memory::IAllocator* a) : m_jobs(a), m_distrusted(a), m_alloc(a) {}

    // Load `.cook_cache/cookdb.toml` under `root` (missing file = empty database, not an error) and read the
    // journal: sources with a dangling `begin` land in the DISTRUSTED set (their cache may be torn).
    void load(const crd::platform::fs::Path& root);

    // Atomically replace the database file (write-temp → rename) and RESET the journal. Returns false on I/O
    // failure (the old database survives).
    [[nodiscard]] bool save(const crd::platform::fs::Path& root);

    // ── the journal ────────────────────────────────────────────────────────────────────────────────────────────
    // Durably append `begin <source>` / `commit <source>`. begin MUST land before the job's artifacts are
    // written; commit after they are all in place.
    [[nodiscard]] bool journal_begin(const crd::platform::fs::Path& root, crd::containers::StringView source);
    [[nodiscard]] bool journal_commit(const crd::platform::fs::Path& root, crd::containers::StringView source);

    // Was `source` left mid-cook by a killed run? (Its cached artifacts are not to be trusted.)
    [[nodiscard]] bool is_distrusted(crd::containers::StringView source) const noexcept;

    // ── the graph ──────────────────────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] const DbJob* find_job(crd::containers::StringView source) const noexcept;
    // replace-or-insert the record for job.source
    void upsert_job(DbJob&& job);
    // drop records whose source no longer exists (the caller knows the live set); removed count returned
    crd::usize prune_missing(const crd::containers::Array<crd::containers::String>& live_sources);

    // reverse edge: every job that recorded `path` (root-relative) as an input
    void jobs_consuming(crd::containers::StringView path, crd::containers::Array<const DbJob*>& out) const;
    // reverse edge: the job that produced `id` (nullptr when unknown)
    [[nodiscard]] const DbJob* find_producer(const crd::resources::ResourceId& id) const noexcept;

    [[nodiscard]] const crd::containers::Array<DbJob>& jobs() const noexcept { return m_jobs; }

private:
    crd::containers::Array<DbJob>                  m_jobs;
    crd::containers::Array<crd::containers::String> m_distrusted;
    crd::memory::IAllocator*                       m_alloc = nullptr;
};

} // namespace crd::cooker
