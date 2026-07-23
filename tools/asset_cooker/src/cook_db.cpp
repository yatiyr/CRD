// cook_db.cpp — GEO-6 (D-007 row 71): the dependency-graph database + journal. See cook_db.hpp for the contract.

#include <crd/cooker/cook_db.hpp>

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace fs = crd::platform::fs;

namespace crd::cooker
{

namespace
{

[[nodiscard]] fs::Path db_path(const fs::Path& root) { return root / ".cook_cache" / "cookdb.toml"; }
[[nodiscard]] fs::Path db_tmp_path(const fs::Path& root) { return root / ".cook_cache" / "cookdb.toml.tmp"; }
[[nodiscard]] fs::Path journal_path(const fs::Path& root) { return root / ".cook_cache" / "journal.log"; }

// u64 hashes as fixed-width hex STRINGS — TOML integers are i64 and the top bit of an FNV hash is routinely set
void append_hex64(crd::containers::String& out, crd::u64 v)
{
    static constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) { out.push_back(kHex[(v >> static_cast<crd::u32>(shift)) & 0xFU]); }
}

[[nodiscard]] bool parse_hex64(std::string_view sv, crd::u64& out) noexcept
{
    if (sv.size() != 16U) { return false; }
    crd::u64 v = 0;
    for (char c : sv)
    {
        crd::u32 nib = 0;
        if (c >= '0' && c <= '9') { nib = static_cast<crd::u32>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { nib = static_cast<crd::u32>(10 + c - 'a'); }
        else { return false; }
        v = (v << 4U) | nib;
    }
    out = v;
    return true;
}

// TOML basic-string escape for paths/names (backslash + quote; control chars are not expected in paths we write)
void append_toml_string(crd::containers::String& out, crd::containers::StringView sv)
{
    out.push_back('"');
    for (crd::usize i = 0; i < sv.size(); ++i)
    {
        const char c = sv[i];
        if (c == '"' || c == '\\') { out.push_back('\\'); }
        out.push_back(c);
    }
    out.push_back('"');
}

// durable append: the journal must be append-only (a rewrite could tear PREVIOUS records on crash)
[[nodiscard]] bool journal_append(const fs::Path& root, const char* verb, crd::containers::StringView source)
{
    const fs::Path jp = journal_path(root);
    const crd::containers::StringView jp_sv = jp.generic();
    crd::containers::String path_z(jp_sv.data(), jp_sv.size(), crd::memory::default_allocator());
#ifdef _MSC_VER
    std::FILE* f = nullptr;
    (void)fopen_s(&f, path_z.c_str(), "ab");
#else
    std::FILE* f = std::fopen(path_z.c_str(), "ab");
#endif
    if (f == nullptr) { return false; }
    bool ok = std::fwrite(verb, 1U, std::strlen(verb), f) == std::strlen(verb);
    ok      = ok && std::fwrite(" ", 1U, 1U, f) == 1U;
    ok      = ok && std::fwrite(source.data(), 1U, source.size(), f) == source.size();
    ok      = ok && std::fwrite("\n", 1U, 1U, f) == 1U;
    ok      = ok && std::fflush(f) == 0;
    (void)std::fclose(f);
    return ok;
}

} // namespace

void CookDb::load(const fs::Path& root)
{
    m_jobs.clear();
    m_distrusted.clear();

    // ── the database ───────────────────────────────────────────────────────────────────────────────────────────
    crd::containers::String text(m_alloc);
    if (fs::read_file_text(db_path(root), text))
    {
        const auto parsed = toml::parse(std::string_view{text.data(), text.size()});
        if (parsed) // a malformed database is DISCARDED wholesale — everything recooks (safe, never stale)
        {
            const toml::table& tbl  = parsed.table();
            const auto*        jobs = tbl["job"].as_array();
            if (jobs != nullptr)
            {
                for (const auto& jn : *jobs)
                {
                    const auto* jt = jn.as_table();
                    if (jt == nullptr) { continue; }
                    DbJob job(m_alloc);
                    if (const auto sv = (*jt)["source"].value<std::string_view>())
                    {
                        job.source = crd::containers::String(sv->data(), sv->size(), m_alloc);
                    }
                    else { continue; }
                    job.handler_version =
                        static_cast<crd::u32>((*jt)["handler_version"].value<crd::i64>().value_or(0));

                    if (const auto* ins = (*jt)["input"].as_array())
                    {
                        for (const auto& in_node : *ins)
                        {
                            const auto* it = in_node.as_table();
                            if (it == nullptr) { continue; }
                            DbInput input(m_alloc);
                            if (const auto p = (*it)["path"].value<std::string_view>())
                            {
                                input.path = crd::containers::String(p->data(), p->size(), m_alloc);
                            }
                            if (const auto h = (*it)["hash"].value<std::string_view>())
                            {
                                (void)parse_hex64(*h, input.content_hash);
                            }
                            input.existed = (*it)["existed"].value<bool>().value_or(false);
                            job.inputs.push_back(static_cast<DbInput&&>(input));
                        }
                    }
                    if (const auto* prods = (*jt)["product"].as_array())
                    {
                        for (const auto& pn : *prods)
                        {
                            const auto* pt = pn.as_table();
                            if (pt == nullptr) { continue; }
                            DbProduct prod(m_alloc);
                            if (const auto u = (*pt)["uuid"].value<std::string_view>())
                            {
                                prod.id = crd::resources::ResourceId::parse(*u);
                            }
                            prod.type_fourcc =
                                static_cast<crd::u32>((*pt)["type"].value<crd::i64>().value_or(0));
                            if (const auto n = (*pt)["name"].value<std::string_view>())
                            {
                                prod.name = crd::containers::String(n->data(), n->size(), m_alloc);
                            }
                            if (const auto h = (*pt)["hash"].value<std::string_view>())
                            {
                                (void)parse_hex64(*h, prod.artifact_hash);
                            }
                            job.products.push_back(static_cast<DbProduct&&>(prod));
                        }
                    }
                    if (const auto* deps = (*jt)["runtime_deps"].as_array())
                    {
                        for (const auto& dn : *deps)
                        {
                            if (const auto u = dn.value<std::string_view>())
                            {
                                const auto id = crd::resources::ResourceId::parse(*u);
                                if (!id.is_null()) { job.runtime_deps.push_back(id); }
                            }
                        }
                    }
                    m_jobs.push_back(static_cast<DbJob&&>(job));
                }
            }
        }
    }

    // ── the journal: dangling `begin` = a job killed mid-write; its cache is distrusted ────────────────────────
    crd::containers::String journal(m_alloc);
    if (fs::read_file_text(journal_path(root), journal))
    {
        const std::string_view jsv(journal.data(), journal.size());
        crd::usize             line_start = 0;
        while (line_start < jsv.size())
        {
            auto line_end = jsv.find('\n', line_start);
            if (line_end == std::string_view::npos) { line_end = jsv.size(); }
            const std::string_view line = jsv.substr(line_start, line_end - line_start);
            line_start                  = line_end + 1U;

            if (line.starts_with("begin "))
            {
                const std::string_view src = line.substr(6U);
                bool                   present = false;
                for (crd::usize i = 0; i < m_distrusted.size(); ++i)
                {
                    if (std::string_view(m_distrusted[i].data(), m_distrusted[i].size()) == src)
                    {
                        present = true;
                        break;
                    }
                }
                if (!present) { m_distrusted.push_back(crd::containers::String(src.data(), src.size(), m_alloc)); }
            }
            else if (line.starts_with("commit "))
            {
                const std::string_view src = line.substr(7U);
                for (crd::usize i = 0; i < m_distrusted.size(); ++i)
                {
                    if (std::string_view(m_distrusted[i].data(), m_distrusted[i].size()) == src)
                    {
                        m_distrusted[i] = static_cast<crd::containers::String&&>(m_distrusted.back());
                        m_distrusted.pop_back();
                        break;
                    }
                }
            }
        }
    }
}

bool CookDb::save(const fs::Path& root)
{
    crd::containers::String out(m_alloc);
    out.append("# cookdb.toml — the GEO-6 dependency graph: source -> job -> product, with recorded input edges.\n");
    out.append("# Machine-written by asset_cooker; query with `asset_cooker graph` / `asset_cooker why`.\n\n");

    for (crd::usize j = 0; j < m_jobs.size(); ++j)
    {
        const DbJob& job = m_jobs[j];
        out.append("[[job]]\n");
        out.append("source = ");
        append_toml_string(out, crd::containers::StringView(job.source.data(), job.source.size()));
        out.append("\nhandler_version = ");
        char num[24];
        std::snprintf(num, sizeof(num), "%u", job.handler_version);
        out.append(num);
        out.push_back('\n');
        if (job.runtime_deps.size() > 0U)
        {
            out.append("runtime_deps = [");
            for (crd::usize d = 0; d < job.runtime_deps.size(); ++d)
            {
                const auto id_str = job.runtime_deps[d].to_string(m_alloc);
                if (d > 0U) { out.append(", "); }
                append_toml_string(out, crd::containers::StringView(id_str.c_str()));
            }
            out.append("]\n");
        }
        for (crd::usize i = 0; i < job.inputs.size(); ++i)
        {
            const DbInput& input = job.inputs[i];
            out.append("[[job.input]]\n");
            out.append("path = ");
            append_toml_string(out, crd::containers::StringView(input.path.data(), input.path.size()));
            out.append("\nhash = \"");
            append_hex64(out, input.content_hash);
            out.append("\"\nexisted = ");
            out.append(input.existed ? "true" : "false");
            out.push_back('\n');
        }
        for (crd::usize p = 0; p < job.products.size(); ++p)
        {
            const DbProduct& prod   = m_jobs[j].products[p];
            const auto       id_str = prod.id.to_string(m_alloc);
            out.append("[[job.product]]\n");
            out.append("uuid = ");
            append_toml_string(out, crd::containers::StringView(id_str.c_str()));
            out.append("\ntype = ");
            std::snprintf(num, sizeof(num), "%u", prod.type_fourcc);
            out.append(num);
            out.append("\nname = ");
            append_toml_string(out, crd::containers::StringView(prod.name.data(), prod.name.size()));
            out.append("\nhash = \"");
            append_hex64(out, prod.artifact_hash);
            out.append("\"\n");
        }
        out.push_back('\n');
    }

    // atomic publish: temp → rename (a crash leaves the OLD database, never a torn one)
    if (!fs::write_file_text(db_tmp_path(root), crd::containers::StringView(out.data(), out.size())))
    {
        return false;
    }
    if (!fs::rename_file(db_tmp_path(root), db_path(root))) { return false; }

    // the run is fully recorded — the journal's history is obsolete
    (void)fs::remove_file(journal_path(root));
    return true;
}

bool CookDb::journal_begin(const fs::Path& root, crd::containers::StringView source)
{
    return journal_append(root, "begin", source);
}

bool CookDb::journal_commit(const fs::Path& root, crd::containers::StringView source)
{
    return journal_append(root, "commit", source);
}

bool CookDb::is_distrusted(crd::containers::StringView source) const noexcept
{
    for (crd::usize i = 0; i < m_distrusted.size(); ++i)
    {
        if (crd::containers::StringView(m_distrusted[i].data(), m_distrusted[i].size()) == source) { return true; }
    }
    return false;
}

const DbJob* CookDb::find_job(crd::containers::StringView source) const noexcept
{
    for (crd::usize i = 0; i < m_jobs.size(); ++i)
    {
        if (crd::containers::StringView(m_jobs[i].source.data(), m_jobs[i].source.size()) == source)
        {
            return &m_jobs[i];
        }
    }
    return nullptr;
}

void CookDb::upsert_job(DbJob&& job)
{
    for (crd::usize i = 0; i < m_jobs.size(); ++i)
    {
        if (crd::containers::StringView(m_jobs[i].source.data(), m_jobs[i].source.size())
            == crd::containers::StringView(job.source.data(), job.source.size()))
        {
            m_jobs[i] = std::move(job);
            return;
        }
    }
    m_jobs.push_back(std::move(job));
}

crd::usize CookDb::prune_missing(const crd::containers::Array<crd::containers::String>& live_sources)
{
    crd::usize removed = 0;
    for (crd::usize i = 0; i < m_jobs.size();)
    {
        const crd::containers::StringView src(m_jobs[i].source.data(), m_jobs[i].source.size());
        bool                              live = false;
        for (crd::usize s = 0; s < live_sources.size(); ++s)
        {
            if (crd::containers::StringView(live_sources[s].data(), live_sources[s].size()) == src)
            {
                live = true;
                break;
            }
        }
        if (live) { ++i; }
        else
        {
            m_jobs[i] = static_cast<DbJob&&>(m_jobs.back());
            m_jobs.pop_back();
            ++removed;
        }
    }
    return removed;
}

void CookDb::jobs_consuming(crd::containers::StringView path, crd::containers::Array<const DbJob*>& out) const
{
    out.clear();
    for (crd::usize j = 0; j < m_jobs.size(); ++j)
    {
        for (crd::usize i = 0; i < m_jobs[j].inputs.size(); ++i)
        {
            const DbInput& input = m_jobs[j].inputs[i];
            if (crd::containers::StringView(input.path.data(), input.path.size()) == path)
            {
                out.push_back(&m_jobs[j]);
                break;
            }
        }
    }
}

const DbJob* CookDb::find_producer(const crd::resources::ResourceId& id) const noexcept
{
    for (crd::usize j = 0; j < m_jobs.size(); ++j)
    {
        for (crd::usize p = 0; p < m_jobs[j].products.size(); ++p)
        {
            if (m_jobs[j].products[p].id == id) { return &m_jobs[j]; }
        }
    }
    return nullptr;
}

} // namespace crd::cooker
