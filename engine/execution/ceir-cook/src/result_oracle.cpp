#include <crd/ceir/cook/result_oracle.hpp>

#include <utility> // std::move

namespace crd::ceir::cook
{

namespace
{
void append_unsigned(containers::String& out, crd::u64 v)
{
    char       buf[20];
    crd::usize n = 0U;
    do
    {
        buf[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U)
    {
        out.push_back(buf[--n]);
    }
}

void append_signed(containers::String& out, crd::i64 v)
{
    if (v < 0)
    {
        out.push_back('-');
        // The magnitude in unsigned arithmetic, so INT64_MIN does not overflow.
        append_unsigned(out, 0U - static_cast<crd::u64>(v));
        return;
    }
    append_unsigned(out, static_cast<crd::u64>(v));
}

void append_view(containers::String& out, containers::StringView v)
{
    out.append(v.data(), v.size());
}
} // namespace

containers::StringView result_verdict_name(ResultVerdict v) noexcept
{
    switch (v) // no default: -Werror=switch names a new verdict here
    {
    case ResultVerdict::Agrees: return containers::StringView("agrees");
    case ResultVerdict::InvalidDomain: return containers::StringView("invalid-domain");
    case ResultVerdict::CountMismatch: return containers::StringView("count-mismatch");
    case ResultVerdict::OutOfDomain: return containers::StringView("out-of-domain");
    case ResultVerdict::DiffersFromReference: return containers::StringView("differs-from-reference");
    }
    return containers::StringView("?");
}

ResultCheck check_results(containers::ConstSpan<crd::i64> values, const ResultExpectation& e) noexcept
{
    ResultCheck c{};
    c.count          = values.size();
    c.expected_count = e.has_reference ? e.reference.size() : 0U;
    if (!domain_valid(e.domain))
    {
        c.verdict = ResultVerdict::InvalidDomain;
        return c;
    }
    if (e.has_reference && values.size() != e.reference.size())
    {
        const crd::usize at = values.size() < e.reference.size() ? values.size() : e.reference.size();
        c.verdict           = ResultVerdict::CountMismatch;
        c.index             = static_cast<crd::u32>(at);
        c.value             = at < values.size() ? values[at] : 0;
        c.expected          = at < e.reference.size() ? e.reference[at] : 0;
        return c;
    }
    for (crd::usize i = 0U; i < values.size(); ++i)
    {
        const crd::u32 bad = value_violations(e.domain, values[i]);
        if (bad != 0U)
        {
            c.verdict    = ResultVerdict::OutOfDomain;
            c.index      = static_cast<crd::u32>(i);
            c.value      = values[i];
            c.violations = bad;
            return c;
        }
        if (e.has_reference && values[i] != e.reference[i])
        {
            c.verdict  = ResultVerdict::DiffersFromReference;
            c.index    = static_cast<crd::u32>(i);
            c.value    = values[i];
            c.expected = e.reference[i];
            return c;
        }
    }
    return c;
}

ResultFinding check_program_results(const ReloadSet& set, const ProgramHandle& h, containers::StringView entry,
                                    containers::ConstSpan<crd::i64> values, const ResultExpectation& e,
                                    memory::IAllocator* out)
{
    ResultFinding f(out);
    f.check = check_results(values, e);
    if (f.check.verdict == ResultVerdict::Agrees || f.check.verdict == ResultVerdict::InvalidDomain)
    {
        return f;
    }
    GenerationSite site = set.locate_result(h, entry, f.check.index, out);
    f.site.asset        = site.asset;
    f.site.generation   = site.generation;
    f.site.state        = site.state;
    f.site.content_hash = site.content_hash;
    f.site.where        = std::move(site.where);
    return f;
}

containers::String render_result_finding(const ResultFinding& f, memory::IAllocator* out)
{
    containers::String r(out);
    const ResultCheck& c = f.check;
    append_view(r, result_verdict_name(c.verdict));
    if (!c.wrong() || c.verdict == ResultVerdict::InvalidDomain)
    {
        return r;
    }
    r.append(" result#");
    append_unsigned(r, c.index);
    r.append(" = ");
    append_signed(r, c.value);
    if (c.verdict == ResultVerdict::OutOfDomain)
    {
        r.append(" [");
        bool first = true;
        for (crd::u32 b = 0U; b < kValueViolationCount; ++b)
        {
            const crd::u32 bit = 1U << b;
            if ((c.violations & bit) == 0U)
            {
                continue;
            }
            if (!first)
            {
                r.push_back(',');
            }
            first = false;
            r.append(value_violation_name(static_cast<ValueViolation>(bit)));
        }
        r.push_back(']');
    }
    else
    {
        r.append(", expected ");
        append_signed(r, c.expected);
    }
    if (c.verdict == ResultVerdict::CountMismatch)
    {
        r.append(" (");
        append_unsigned(r, c.count);
        r.append(" results, expected ");
        append_unsigned(r, c.expected_count);
        r.push_back(')');
    }
    r.append(" at ");
    const containers::String site = render_generation_site(f.site, out);
    r.append(site.data(), site.size());
    return r;
}

} // namespace crd::ceir::cook
