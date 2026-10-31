#pragma once

// crd-platform -- the program's command-line arguments, as UTF-8 on every platform.
//
// POSIX hands main its arguments as the bytes they were passed. Windows hands main's char** the command line converted
// to the ANSI code page: a character the code page lacks arrives as '?', and the best-fit mapping turns some characters
// into the quote or the backslash that delimit and escape arguments (U+FF02 FULLWIDTH QUOTATION MARK into '"'), so a
// program can be handed other arguments than it was passed. Every tool's main reads its arguments through Arguments
// instead: on Windows it splits the UTF-16 command line itself, by the Microsoft C runtime's rules, and hands each
// argument on as UTF-8 (crd/core/utf16.hpp), so the program sees the arguments main would have seen, characters intact.

#include <crd/containers/array.hpp>

namespace crd::platform
{

class Arguments
{
public:
    // The arguments main was handed: viewed in place on POSIX, read again from the UTF-16 command line on Windows.
    Arguments(int argc, char** argv);

    // `line` split as the Windows constructor splits the process's command line, on every platform. The rules are the
    // Microsoft C runtime's (Visual Studio 2008 and later, "Parsing C++ command-line arguments"), not
    // CommandLineToArgvW's: the first argument, the program name, runs to the first space or tab outside double
    // quotes, its quotes dropped and its backslashes kept. Each later argument runs to the next space or tab outside
    // double quotes; a double quote opens or closes a quoted run and is dropped; 2n backslashes before a double quote
    // are n backslashes and the quote delimits, 2n + 1 are n backslashes and a literal quote; two double quotes inside
    // a quoted run are one literal quote; any other backslash is itself. An unpaired surrogate reads as U+FFFD.
    [[nodiscard]] static Arguments from_windows_command_line(const char16_t* line);

    [[nodiscard]] int count() const noexcept { return m_count; }
    // argv's shape: count() NUL-terminated UTF-8 strings, then a null pointer; valid while this object lives.
    [[nodiscard]] char** values() const noexcept { return m_values; }

    Arguments(const Arguments&)            = delete;
    Arguments& operator=(const Arguments&) = delete;
    Arguments(Arguments&&) noexcept        = default; // the arrays move their storage, so the pointers stay valid
    Arguments& operator=(Arguments&&)      = delete;
    ~Arguments()                           = default;

private:
    Arguments() = default;
    template <typename Unit> void split(const Unit* line);

    containers::Array<char>  m_bytes;    // the split arguments, each followed by its NUL
    containers::Array<char*> m_pointers; // into m_bytes, then nullptr
    int                      m_count  = 0;
    char**                   m_values = nullptr;
};

} // namespace crd::platform
