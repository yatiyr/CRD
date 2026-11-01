// crd-platform -- the program's arguments (crd/platform/arguments.hpp): the Microsoft C runtime's splitting rules,
// case by case from its documentation ("Parsing C++ command-line arguments"), the program name's own rule, and the
// arguments handed on as UTF-8 with their characters intact, a best-fit quote included. The splitting is the Windows
// constructor's, run on every platform; on POSIX the constructor views argv in place.

#include <crd/core/platform.hpp>
#include <crd/platform/arguments.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <initializer_list>

namespace
{
using crd::platform::Arguments;

void check_split(const char16_t* line, std::initializer_list<const char*> expected)
{
    const Arguments arguments = Arguments::from_windows_command_line(line);
    REQUIRE(arguments.count() == static_cast<int>(expected.size()));
    int i = 0;
    for (const char* argument : expected)
    {
        INFO("argument " << i);
        CHECK(std::strcmp(arguments.values()[i], argument) == 0);
        ++i;
    }
    CHECK(arguments.values()[arguments.count()] == nullptr);
}

} // namespace

TEST_CASE("arguments: the C runtime's documented examples split the same way", "[platform][arguments]")
{
    check_split(u"p \"a b c\" d e", {"p", "a b c", "d", "e"});
    check_split(u"p \"ab\\\"c\" \"\\\\\" d", {"p", "ab\"c", "\\", "d"});
    check_split(uR"(p a\\\b d"e f"g h)", {"p", R"(a\\\b)", "de fg", "h"});
    check_split(u"p a\\\\\\\"b c d", {"p", "a\\\"b", "c", "d"});
    check_split(u"p a\\\\\\\\\"b c\" d e", {"p", "a\\\\b c", "d", "e"});
    check_split(u"p a\"b\"\" c d", {"p", "ab\" c d"});
}

TEST_CASE("arguments: blanks, empty arguments, quotes and trailing backslashes", "[platform][arguments]")
{
    check_split(u"p\ta \t  b\t", {"p", "a", "b"});
    check_split(u"p \"\" x \"\"", {"p", "", "x", ""});
    check_split(u"p a\\\\", {"p", "a\\\\"});   // backslashes before no quote are themselves
    check_split(u"p \"a\\\\\"", {"p", "a\\"}); // two before the closing quote are one
    check_split(u"p \"unclosed run  ", {"p", "unclosed run  "});
    check_split(u"p a\"\"b", {"p", "ab"}); // outside a quoted run two quotes open and close an empty one
    check_split(u"p \"\"\"\"", {"p", "\""});
}

TEST_CASE("arguments: the program name keeps its backslashes and drops its quotes", "[platform][arguments]")
{
    check_split(uR"("C:\Program Files\Cerid\ceridc.exe" screen)", {R"(C:\Program Files\Cerid\ceridc.exe)", "screen"});
    check_split(uR"(C:\tools\ceridc.exe\"x y" z)", {R"(C:\tools\ceridc.exe\x y)", "z"});
    check_split(u"", {""});
    check_split(u"ceridc", {"ceridc"});
    check_split(u"  leading", {"", "leading"}); // a line that starts blank has an empty program name
}

TEST_CASE("arguments: characters outside ASCII arrive as UTF-8, a best-fit quote as itself", "[platform][arguments]")
{
    // \103 and \143 spell 'C' and 'c' after a hex escape: an octal escape stops at three digits, a hex one runs on.
    check_split(u"ceridc screen --name \"KUZEY DEN\u0130Z T\u0130CARET A.\u015E.\" --name \u011F\u00FC\u015F\u0131",
                {"ceridc", "screen", "--name", "KUZEY DEN\xC4\xB0Z T\xC4\xB0\103ARET A.\xC5\x9E.", "--name",
                 "\xC4\x9F\xC3\xBC\xC5\x9F\xC4\xB1"});
    // U+FF02 FULLWIDTH QUOTATION MARK, which the ANSI code pages' best-fit mapping turns into '"': here it neither
    // opens a quoted run nor ends an argument.
    check_split(u"p a\uFF02 b\uFF02c", {"p", "a\xEF\xBC\x82", "b\xEF\xBC\x82\143"});
    check_split(u"p \U0001D538\U0001D539", {"p", "\xF0\x9D\x94\xB8\xF0\x9D\x94\xB9"});
    const char16_t unpaired[] = {u'p', u' ', 0xD800U, u'x', 0xDC00U, 0U};
    check_split(unpaired, {"p", "\xEF\xBF\xBDx\xEF\xBF\xBD"});
}

TEST_CASE("arguments: a moved object keeps its arguments", "[platform][arguments]")
{
    Arguments first = Arguments::from_windows_command_line(u"p one two");
    const Arguments moved(static_cast<Arguments&&>(first));
    REQUIRE(moved.count() == 3);
    CHECK(std::strcmp(moved.values()[2], "two") == 0);
}

#if !CRD_OS_WINDOWS
TEST_CASE("arguments: POSIX hands main's arguments on as they are", "[platform][arguments]")
{
    char name[] = "p";
    char value[] = "DEN\xC4\xB0Z";
    char* argv[] = {name, value, nullptr};
    const Arguments arguments(2, argv);
    CHECK(arguments.count() == 2);
    CHECK(arguments.values() == argv);
}
#endif
