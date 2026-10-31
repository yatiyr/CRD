#include <crd/comply/names.hpp>
#include <crd/core/text_fold.hpp>
#include <crd/core/utf8.hpp>

#include <algorithm>

namespace crd::comply
{
namespace
{
constexpr double kPairFloor = 0.88;    // word pairs below this do not pair
constexpr double kInitial = 0.9;       // an initial against a word it starts
constexpr double kSkeleton = 0.9;      // the least two words spelling one consonant skeleton rate
constexpr double kUnpairedWord = 0.05; // the subset score's cost of each unpaired word of the longer name
constexpr u32 kMaxJoinedCodePoints = 128U;

// Honorifics and legal forms, folded; a name keeps them when nothing else is left. The last five are the Cyrillic
// OOO, OAO, ZAO, PAO and AO.
constexpr const char32_t* kStopWords[] = { // NOLINT(modernize-avoid-c-arrays)
    U"mr",   U"mrs", U"ms",   U"miss", U"dr",   U"prof",    U"sir",          U"llc", U"ltd",         U"limited", U"inc",
    U"corp", U"co",  U"plc",  U"gmbh", U"ag",   U"sa",      U"incorporated", U"sas", U"corporation", U"sarl",    U"srl",
    U"spa",  U"bv",  U"nv",   U"oy",   U"ab",   U"company", U"as",           U"ooo", U"oao",         U"zao",     U"pao",
    U"ao",   U"jsc", U"ojsc", U"cjsc", U"pjsc", U"llp",     U"lp",           U"fze", U"fzco",        U"sti",     U"ооо",
    U"оао",  U"зао", U"пао",  U"ао"};

struct Word
{
    u32 begin = 0U;
    u32 end = 0U;
};

struct WordRef
{
    const u32* text = nullptr;
    u32 size = 0U;
    const u32* skeleton = nullptr;
    u32 skeleton_size = 0U;
};

[[nodiscard]] bool spells(const u32* text, u32 size, const char32_t* word) noexcept
{
    u32 i = 0U;
    for (; i < size && word[i] != U'\0'; ++i)
    {
        if (text[i] != static_cast<u32>(word[i]))
        {
            return false;
        }
    }
    return i == size && word[i] == U'\0';
}

[[nodiscard]] bool is_stop_word(const u32* text, u32 size) noexcept
{
    return std::ranges::any_of(kStopWords, [text, size](const char32_t* word) { return spells(text, size, word); });
}

[[nodiscard]] bool is_vowel(u32 c) noexcept
{
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

// The consonants of an ASCII word with doubled ones collapsed, ph read as f and q as k; empty for other words.
void append_skeleton(const u32* text, u32 size, cont::Array<u32>& out)
{
    for (u32 i = 0U; i < size; ++i)
    {
        if (text[i] >= 0x80U)
        {
            return;
        }
    }
    const usize start = out.size();
    for (u32 i = 0U; i < size; ++i)
    {
        u32 c = text[i];
        if (c == 'p' && i + 1U < size && text[i + 1U] == 'h')
        {
            c = 'f';
            ++i;
        }
        else if (c == 'q')
        {
            c = 'k';
        }
        if (is_vowel(c) || (out.size() > start && out.back() == c))
        {
            continue;
        }
        out.push_back(c);
    }
}

[[nodiscard]] double jaro_winkler(const u32* a, u32 la, const u32* b, u32 lb) noexcept
{
    const u32 longer = la > lb ? la : lb;
    const u32 window = longer / 2U > 0U ? (longer / 2U) - 1U : 0U;
    bool a_matched[kMaxWordCodePoints] = {}; // NOLINT(modernize-avoid-c-arrays)
    bool b_matched[kMaxWordCodePoints] = {}; // NOLINT(modernize-avoid-c-arrays)
    u32 matches = 0U;
    for (u32 i = 0U; i < la; ++i)
    {
        const u32 from = i > window ? i - window : 0U;
        const u32 to = i + window + 1U < lb ? i + window + 1U : lb;
        for (u32 j = from; j < to; ++j)
        {
            if (!b_matched[j] && a[i] == b[j])
            {
                a_matched[i] = true;
                b_matched[j] = true;
                ++matches;
                break;
            }
        }
    }
    if (matches == 0U)
    {
        return 0.0;
    }
    u32 transposed = 0U;
    u32 k = 0U;
    for (u32 i = 0U; i < la; ++i)
    {
        if (!a_matched[i])
        {
            continue;
        }
        while (!b_matched[k])
        {
            ++k;
        }
        transposed += a[i] != b[k] ? 1U : 0U;
        ++k;
    }
    const double m = matches;
    const u32 half_transpositions = transposed / 2U; // Jaro counts half the out-of-order matches, rounded down
    const double jaro = ((m / la) + (m / lb) + ((m - half_transpositions) / m)) / 3.0;
    u32 prefix = 0U;
    while (prefix < 4U && prefix < la && prefix < lb && a[prefix] == b[prefix])
    {
        ++prefix;
    }
    return jaro + (prefix * 0.1 * (1.0 - jaro));
}

[[nodiscard]] bool same(const u32* a, u32 la, const u32* b, u32 lb) noexcept
{
    if (la != lb)
    {
        return false;
    }
    for (u32 i = 0U; i < la; ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] double similarity(const WordRef& a, const WordRef& b) noexcept
{
    if (same(a.text, a.size, b.text, b.size))
    {
        return 1.0;
    }
    if (a.size == 1U || b.size == 1U)
    {
        return a.text[0] == b.text[0] ? kInitial : 0.0;
    }
    const double value = jaro_winkler(a.text, a.size, b.text, b.size);
    const bool skeleton = a.skeleton_size >= 2U && same(a.skeleton, a.skeleton_size, b.skeleton, b.skeleton_size);
    return skeleton && value < kSkeleton ? kSkeleton : value;
}

[[nodiscard]] u32 levenshtein(const u32* a, u32 la, const u32* b, u32 lb) noexcept
{
    u32 previous[kMaxJoinedCodePoints + 1U] = {}; // NOLINT(modernize-avoid-c-arrays)
    u32 current[kMaxJoinedCodePoints + 1U] = {};  // NOLINT(modernize-avoid-c-arrays)
    for (u32 j = 0U; j <= lb; ++j)
    {
        previous[j] = j;
    }
    for (u32 i = 1U; i <= la; ++i)
    {
        current[0] = i;
        for (u32 j = 1U; j <= lb; ++j)
        {
            const u32 replace = previous[j - 1U] + (a[i - 1U] != b[j - 1U] ? 1U : 0U);
            const u32 remove = previous[j] + 1U;
            const u32 insert = current[j - 1U] + 1U;
            u32 best = replace < remove ? replace : remove;
            best = insert < best ? insert : best;
            current[j] = best;
        }
        for (u32 j = 0U; j <= lb; ++j)
        {
            previous[j] = current[j];
        }
    }
    return previous[lb];
}

// NOLINTNEXTLINE(modernize-avoid-c-arrays)
[[nodiscard]] u32 words_of(const FoldedName& name, WordRef (&out)[kMaxNameWords]) noexcept
{
    u32 begin = 0U;
    u32 skeleton_begin = 0U;
    const u32 count = static_cast<u32>(name.ends.size());
    for (u32 w = 0U; w < count; ++w)
    {
        out[w].text = name.text.data() + begin;
        out[w].size = name.ends[w] - begin;
        out[w].skeleton = name.skeletons.data() + skeleton_begin;
        out[w].skeleton_size = name.skeleton_ends[w] - skeleton_begin;
        begin = name.ends[w];
        skeleton_begin = name.skeleton_ends[w];
    }
    return count;
}
} // namespace

FoldedName fold_name(cont::StringView name)
{
    cont::Array<u32> text;
    cont::Array<Word> words;
    const auto* bytes = reinterpret_cast<const u8*>(name.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    bool in_word = false;
    const auto end_word = [&]()
    {
        if (in_word)
        {
            words.back().end = static_cast<u32>(text.size());
            in_word = false;
        }
    };
    usize i = 0U;
    while (i < name.size())
    {
        u32 code_point = 0U;
        const usize length = utf8_decode(bytes + i, name.size() - i, code_point);
        i += length != 0U ? length : 1U;
        const CodePointFold fold = length != 0U ? fold_code_point(code_point) : CodePointFold{FoldKind::Separator};
        if (fold.kind == FoldKind::Separator)
        {
            end_word();
            continue;
        }
        if (fold.kind == FoldKind::Elided)
        {
            continue;
        }
        if (!in_word)
        {
            if (words.size() == kMaxNameWords)
            {
                break;
            }
            words.push_back(Word{static_cast<u32>(text.size()), 0U});
            in_word = true;
        }
        for (u32 k = 0U; k < fold.count; ++k)
        {
            if (text.size() - words.back().begin < kMaxWordCodePoints)
            {
                text.push_back(fold.out[k]);
            }
        }
    }
    end_word();

    // Honorifics and legal forms go, a legal form spelt in single letters (A.Ş., L.L.C.) included, unless nothing
    // else would be left.
    bool drop[kMaxNameWords] = {}; // NOLINT(modernize-avoid-c-arrays)
    const u32 count = static_cast<u32>(words.size());
    u32 dropped = 0U;
    for (u32 w = 0U; w < count;)
    {
        u32 run = w;
        u32 letters[kMaxNameWords] = {}; // NOLINT(modernize-avoid-c-arrays)
        while (run < count && words[run].end - words[run].begin == 1U)
        {
            letters[run - w] = text[words[run].begin];
            ++run;
        }
        if (run - w >= 2U && is_stop_word(letters, run - w))
        {
            for (u32 k = w; k < run; ++k)
            {
                drop[k] = true;
            }
            dropped += run - w;
            w = run;
            continue;
        }
        if (is_stop_word(text.data() + words[w].begin, words[w].end - words[w].begin))
        {
            drop[w] = true;
            ++dropped;
        }
        ++w;
    }
    FoldedName folded;
    for (u32 w = 0U; w < count; ++w)
    {
        if (drop[w] && dropped < count)
        {
            continue;
        }
        const u32* word = text.data() + words[w].begin;
        const u32 size = words[w].end - words[w].begin;
        for (u32 k = 0U; k < size; ++k)
        {
            folded.text.push_back(word[k]);
        }
        folded.ends.push_back(static_cast<u32>(folded.text.size()));
        append_skeleton(word, size, folded.skeletons);
        folded.skeleton_ends.push_back(static_cast<u32>(folded.skeletons.size()));
    }
    return folded;
}

u32 name_score(const FoldedName& a, const FoldedName& b)
{
    WordRef left[kMaxNameWords];  // NOLINT(modernize-avoid-c-arrays)
    WordRef right[kMaxNameWords]; // NOLINT(modernize-avoid-c-arrays)
    const u32 nl = words_of(a, left);
    const u32 nr = words_of(b, right);
    if (nl == 0U || nr == 0U)
    {
        return 0U;
    }
    double similarities[kMaxNameWords][kMaxNameWords] = {}; // NOLINT(modernize-avoid-c-arrays)
    for (u32 i = 0U; i < nl; ++i)
    {
        for (u32 j = 0U; j < nr; ++j)
        {
            similarities[i][j] = similarity(left[i], right[j]);
        }
    }
    bool left_paired[kMaxNameWords] = {};  // NOLINT(modernize-avoid-c-arrays)
    bool right_paired[kMaxNameWords] = {}; // NOLINT(modernize-avoid-c-arrays)
    double both = 0.0;
    double left_covered = 0.0;
    double right_covered = 0.0;
    u32 pairs = 0U;
    for (;;)
    {
        // The best unpaired pair at or above the floor; ties go to the earlier words.
        double best = 0.0;
        u32 bi = nl;
        u32 bj = nr;
        for (u32 i = 0U; i < nl; ++i)
        {
            for (u32 j = 0U; j < nr; ++j)
            {
                const double value = similarities[i][j];
                if (!left_paired[i] && !right_paired[j] && value >= kPairFloor && (bi == nl || value > best))
                {
                    best = value;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bi == nl)
        {
            break;
        }
        left_paired[bi] = true;
        right_paired[bj] = true;
        both += best * (left[bi].size + right[bj].size);
        left_covered += best * left[bi].size;
        right_covered += best * right[bj].size;
        ++pairs;
    }
    u32 left_total = 0U;
    u32 right_total = 0U;
    for (u32 i = 0U; i < nl; ++i)
    {
        left_total += left[i].size;
    }
    for (u32 j = 0U; j < nr; ++j)
    {
        right_total += right[j].size;
    }
    double score = both / (left_total + right_total);
    if (nl != nr)
    {
        const bool left_shorter = nl < nr;
        if ((left_shorter ? nl : nr) >= 2U)
        {
            const double covered = left_shorter ? left_covered / left_total : right_covered / right_total;
            const double subset = covered - (kUnpairedWord * ((left_shorter ? nr : nl) - pairs));
            score = subset > score ? subset : score;
        }
        if (a.text.size() <= kMaxJoinedCodePoints && b.text.size() <= kMaxJoinedCodePoints)
        {
            const u32 la = static_cast<u32>(a.text.size());
            const u32 lb = static_cast<u32>(b.text.size());
            const double joined =
                1.0 - (static_cast<double>(levenshtein(a.text.data(), la, b.text.data(), lb)) / (la > lb ? la : lb));
            score = joined > score ? joined : score;
        }
    }
    const double permille = (score * kMaxScore) + 0.5;
    if (permille >= kMaxScore)
    {
        return kMaxScore;
    }
    return permille <= 0.0 ? 0U : static_cast<u32>(permille);
}
} // namespace crd::comply
