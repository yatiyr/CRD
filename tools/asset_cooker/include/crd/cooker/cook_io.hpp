#pragma once

// cook_io.hpp — GEO-6 (D-007 row 71): the DECLARED-INPUT seam. The ⛔ design rule made structural: a cook receives
// every byte it consumes THROUGH this object, and every read is RECORDED as a dependency edge — source, .meta,
// auxiliary inputs (external .bin, referenced images, included files), and the id-stability sidecars. Undeclared
// inputs cannot exist because there is no other road to bytes; the processor persists the recorded edges into the
// cook database and re-hashes exactly them to decide incremental recooks (the ninja/Bevy depfile model — O3DE's
// core lesson: an unrecorded dependency is a stale product waiting to ship).
//
// ABSENCE IS A DEPENDENCY: a read that finds nothing is recorded with existed=false — if the file appears later,
// the job's input set has changed and it recooks. Auxiliary reads resolve RELATIVE TO THE SOURCE'S DIRECTORY and
// refuse absolute paths, drive letters/schemes, and ".." escapes — a cook never reads outside its source tree.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::cooker
{

// One recorded input edge. `path` is the path AS RESOLVED for the read (the processor root-relativizes it when
// persisting); `content_hash` is FNV-1a 64 over the bytes (0 when absent).
struct CookInput
{
    crd::containers::String path;
    crd::u64                content_hash = 0;
    bool                    existed      = false;

    explicit CookInput(crd::memory::IAllocator* a) : path(a) {}
    CookInput(CookInput&&)            = default;
    CookInput& operator=(CookInput&&) = default;
};

// The content hash the whole cook pipeline keys on (FNV-1a 64) — ONE definition, shared by the seam, the
// database, and the processor.
[[nodiscard]] crd::u64 cook_hash64(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

class CookIO
{
public:
    // `source_path` / `meta_path` as the processor resolved them (meta may not exist yet). `root` is the source
    // TREE boundary auxiliary reads may not escape ('/'-separated, the processor's cook root); empty = the
    // source's own directory (the strictest boundary — what handler-direct tests want).
    CookIO(crd::containers::StringView source_path, crd::containers::StringView meta_path,
           crd::memory::IAllocator* alloc, crd::containers::StringView root = {});

    CookIO(const CookIO&)            = delete;
    CookIO& operator=(const CookIO&) = delete;

    // The source's own bytes. Recorded. False (and recorded absent) when unreadable.
    [[nodiscard]] bool read_source(crd::containers::Array<crd::u8>& out);

    // The .meta sidecar's text. Recorded EITHER WAY (absence included — adding a .meta later must recook).
    // Returns false when absent/unreadable; `out` is empty then.
    [[nodiscard]] bool read_meta(crd::containers::String& out);

    // An auxiliary input named RELATIVE to the source's directory ('/'-separated; what glTF uris, TOML bundle
    // references, and include directives carry). Refuses absolute paths and schemes/drive letters outright;
    // ".." segments are allowed but the LEXICALLY NORMALIZED result must stay inside `root` — a cook never
    // reads outside its source tree. Recorded either way (absence included).
    [[nodiscard]] bool read_input(crd::containers::StringView rel_path, crd::containers::Array<crd::u8>& out);

    // A STABLE id for a named sub-product, persisted in "<source><suffix>.meta" (e.g. suffix ".mesh.hull",
    // ".tex.0_albedo", ".scen") — read on every cook, MINTED once on first use, replayed forever after (the
    // id-stability contract). The sidecar's content is recorded as an input so a hand-edited id recooks the job.
    // Returns false when the sidecar cannot be written (out_id null).
    [[nodiscard]] bool stable_id(crd::containers::StringView suffix, crd::resources::ResourceId& out_id);

    // The recorded dependency edges, in first-read order (deterministic given a deterministic handler).
    [[nodiscard]] const crd::containers::Array<CookInput>& inputs() const noexcept { return m_inputs; }

private:
    void record(crd::containers::StringView path, crd::containers::ConstSpan<crd::u8> bytes, bool existed);

    crd::containers::String            m_source_path;
    crd::containers::String            m_meta_path;
    crd::containers::String            m_root; // the escape boundary (source dir when none given)
    crd::containers::Array<CookInput>  m_inputs;
    crd::memory::IAllocator*           m_alloc = nullptr;

    // the source/meta are read once from disk and served from cache after — the processor force-records both
    // (their content governs the cook key even for handlers that never look at them) and the handler's own read
    // must not double the I/O
    crd::containers::Array<crd::u8>    m_source_cache{m_alloc};
    crd::containers::String            m_meta_cache{m_alloc};
    bool                               m_source_read = false;
    bool                               m_source_ok   = false;
    bool                               m_meta_read   = false;
    bool                               m_meta_ok     = false;
};

} // namespace crd::cooker
