// crd-comply -- the name matcher sanctions screening runs (COMPLY.1; docs/design/registers.md#screening): folding,
// legal forms and honorifics, and scores on both sides of the review threshold: the variants it must catch
// (Turkish letters, transliteration, word order, a missing middle name, split and joined words, initials) and the
// different names it must not raise (one letter or one word apart, a shared given name).

#include <crd/comply/names.hpp>
#include <crd/containers/string.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using namespace crd::comply;
namespace cont = crd::containers;

constexpr u32 kReview = 850U; // ScreenPolicy's default review threshold

u32 score(cont::StringView a, cont::StringView b)
{
    const u32 forward = name_score(fold_name(a), fold_name(b));
    INFO(a << " | " << b << " = " << forward);
    return forward;
}

// The folded words, each as UTF-8 for the comparison (ASCII here).
cont::String words(cont::StringView name)
{
    const FoldedName folded = fold_name(name);
    cont::String out;
    u32 begin = 0U;
    for (const u32 end : folded.ends)
    {
        if (!out.empty())
        {
            out.push_back(' ');
        }
        for (u32 i = begin; i < end; ++i)
        {
            out.push_back(folded.text[i] < 0x80U ? static_cast<char>(folded.text[i]) : '?');
        }
        begin = end;
    }
    return out;
}
} // namespace

TEST_CASE("names: folding reads words in search form", "[comply][names]")
{
    CHECK(words("MEHMET YILMAZ") == "mehmet yilmaz");
    CHECK(words("\xC5\x9E"
                "ah\xC4\xB1n \xC3\x96zt\xC3\xBCrk") == "sahin ozturk"); // Şahın Öztürk
    CHECK(words("\xC4\xB0"
                "brahim") == "ibrahim"); // İbrahim
    CHECK(words("S\xCC\xA7"
                "ahin") == "sahin"); // S + combining cedilla
    CHECK(words("O'Brien, Patrick") == "obrien patrick");
    CHECK(words("Qa\xCA\xBF"
                "ida") == "qaida"); // ayn elided
    CHECK(words("Al-Qasim") == "al qasim");
    CHECK(words("  Kaya\t\tAy\xC5\x9F"
                "e  ") == "kaya ayse");
    CHECK(words("Stra\xC3\x9F"
                "e \xC3\x86sir \xC5\x92uvre") == "strasse aesir oeuvre");
    CHECK(words("Ivan\xFFPetrov") == "ivan petrov"); // an ill-formed byte separates
    CHECK(words("") == "");
    CHECK(words(" .,;- ") == "");
}

TEST_CASE("names: honorifics and legal forms go unless nothing else is left", "[comply][names]")
{
    CHECK(words("Mr. John Smith") == "john smith");
    CHECK(words("Acme Trading LLC") == "acme trading");
    CHECK(words("Deniz Lojistik A.\xC5\x9E.") == "deniz lojistik"); // A.Ş. spelt in single letters
    CHECK(words("Deniz Lojistik Ltd. \xC5\x9Eti.") == "deniz lojistik");
    CHECK(words("Global Trade L.L.C.") == "global trade");
    CHECK(words("J. R. Smith") == "j r smith");                      // single letters that spell no legal form stay
    CHECK(words("Ltd") == "ltd");                                    // nothing else: kept
    CHECK(words("\xD0\x9E\xD0\x9E\xD0\x9E Romashka") == "romashka"); // the Cyrillic OOO
}

TEST_CASE("names: a word or a code point beyond the bounds is not read", "[comply][names]")
{
    cont::String many;
    for (u32 i = 0U; i < kMaxNameWords + 4U; ++i)
    {
        many.append("w ");
    }
    CHECK(fold_name(many).ends.size() == kMaxNameWords);
    cont::String long_word(200U, 'a');
    const FoldedName folded = fold_name(long_word);
    REQUIRE(folded.ends.size() == 1U);
    CHECK(folded.ends[0] == kMaxWordCodePoints);
}

TEST_CASE("names: equal names after folding score 1000 in any word order", "[comply][names]")
{
    CHECK(score("Mehmet Y\xC4\xB1lmaz", "MEHMET YILMAZ") == 1000U);
    CHECK(score("\xC5\x9E"
                "ahin \xC3\x96zt\xC3\xBCrk",
                "Sahin Ozturk") == 1000U);
    CHECK(score("Kaya Ay\xC5\x9F"
                "e",
                "Ayse Kaya") == 1000U);
    CHECK(score("Ivan Petrov", "Petrov, Ivan") == 1000U);
    CHECK(score("O'Brien Patrick", "Patrick OBrien") == 1000U);
    CHECK(score("Acme Trading LLC", "ACME TRADING LIMITED") == 1000U);
    CHECK(score("Pak Song Il", "Pak Song-il") == 1000U);
    CHECK(score("Deniz Lojistik A.\xC5\x9E.", "Deniz Lojistik") == 1000U);
    CHECK(score("\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD", "\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD") == 1000U); // Иван, иван
}

TEST_CASE("names: split and joined words score 1000", "[comply][names]")
{
    CHECK(score("Abdulrahman Al-Qasim", "Abdul Rahman al Qasim") == 1000U);
    CHECK(score("Mohammad Reza Ahmadi", "Mohammadreza Ahmadi") == 1000U);
}

TEST_CASE("names: the variants screening must raise reach the review threshold", "[comply][names]")
{
    CHECK(score("Muhammad", "Mohamed") >= kReview); // one consonant skeleton
    CHECK(score("Mohammed Yusuf", "Muhammad Yousef") >= kReview);
    CHECK(score("Yusuf Kaya", "Yousef Kaya") >= kReview);
    CHECK(score("Hussein", "Husayn") >= kReview);
    CHECK(score("Petr Ivanov", "Pyotr Ivanov") >= kReview);
    CHECK(score("Usama bin Rashid", "Osama bin Rashed") >= kReview);
    CHECK(score("Sergei Ivanov", "Sergey Ivanov") >= kReview);
    CHECK(score("Ahmet Yilmaz", "Ahmed Yilmaz") >= kReview);
    CHECK(score("Mustafa Kemal", "Mustafa Kamal") >= kReview);
    CHECK(score("Anna Ivanova", "Anna Ivanov") >= kReview);
    CHECK(score("J. Smith", "John Smith") >= kReview);                    // an initial
    CHECK(score("Oleg Petrenko", "Oleg Vladimirovich Petrenko") == 950U); // a patronymic missing: one unpaired word
    CHECK(score("Ali Hassan Mohammed", "Ali Mohammed") == 950U);
    CHECK(score("Ali Veli", "Veli Ali Yilmaz") == 950U);
}

TEST_CASE("names: different names stay below the review threshold", "[comply][names]")
{
    CHECK(score("Mehmet Yilmaz", "Mehmet Yildiz") < kReview); // one surname apart
    CHECK(score("Mehmet Yilmaz", "Ahmet Yildiz") < kReview);
    CHECK(score("Mehmet Kaya", "Mehmet Kara") < kReview); // one letter apart
    CHECK(score("John Smith", "Joan Smith") < kReview);
    CHECK(score("John Smith", "Jane Smythe") < kReview);
    CHECK(score("Pak Song Il", "Pak Song Chol") < kReview);
    CHECK(score("Alexander Petrov", "Alexander Ivanov") < kReview);
    CHECK(score("Bayrak", "Bayrak Trading Company") < kReview); // one word against three: no subset score
    CHECK(score("Global Logistics Ltd", "Global Shipping Ltd") < kReview);
    CHECK(score("Ali Kaya", "Ali Veli Yilmaz") < kReview);
    CHECK(score("Ivan Petrov", "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD \xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2") <
          kReview); // no transliteration between scripts: the lists carry Latin aliases
}

TEST_CASE("names: a name without words scores 0", "[comply][names]")
{
    CHECK(score("", "Ivan Petrov") == 0U);
    CHECK(score("Ivan Petrov", " - ") == 0U);
}
