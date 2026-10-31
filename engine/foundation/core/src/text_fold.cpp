#include <crd/core/text_fold.hpp>

namespace crd
{
namespace
{
struct FoldRange
{
    u32 first;
    u32 last;
    FoldKind kind;
};

struct FoldMapping
{
    u32 code_point;
    u32 first;
    u32 second; // 0 when the letter folds to one code point
};

#include "text_fold_table.inc"

template <typename T, usize N, typename Below>
[[nodiscard]] const T* find(const T (&table)[N], Below below) noexcept // NOLINT(modernize-avoid-c-arrays)
{
    usize low = 0U;
    usize high = N;
    while (low < high)
    {
        const usize middle = low + ((high - low) / 2U);
        if (below(table[middle]))
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return low < N ? &table[low] : nullptr;
}
} // namespace

CodePointFold fold_code_point(u32 code_point) noexcept
{
    CodePointFold fold;
    if (code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU))
    {
        fold.kind = FoldKind::Separator;
        return fold;
    }
    const FoldRange* range = find(kFoldRanges, [code_point](const FoldRange& r) { return r.last < code_point; });
    if (range != nullptr && range->first <= code_point)
    {
        fold.kind = range->kind;
        return fold;
    }
    const FoldMapping* mapping =
        find(kFoldMappings, [code_point](const FoldMapping& m) { return m.code_point < code_point; });
    fold.count = 1U;
    fold.out[0] = code_point;
    if (mapping != nullptr && mapping->code_point == code_point)
    {
        fold.out[0] = mapping->first;
        fold.out[1] = mapping->second;
        fold.count = mapping->second != 0U ? 2U : 1U;
    }
    return fold;
}

} // namespace crd
