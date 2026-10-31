// The program's command-line arguments (crd/platform/arguments.hpp).

#include <crd/core/platform.hpp>
#include <crd/core/utf16.hpp>
#include <crd/platform/arguments.hpp>

#if CRD_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace crd::platform
{
namespace
{

// Appends the code point at `p` (not the NUL) as UTF-8 and returns the units it took. The terminating NUL stands
// behind every unit, so two units are always readable there.
template <typename Unit> usize append_code_point(const Unit* p, containers::Array<char>& bytes)
{
    u32         code_point = 0U;
    const usize units      = utf16_decode(p, 2U, code_point);
    u8          sequence[4] = {}; // NOLINT(modernize-avoid-c-arrays): one UTF-8 sequence
    const usize size        = utf8_encode(code_point, sequence);
    for (usize i = 0U; i < size; ++i)
    {
        bytes.push_back(static_cast<char>(sequence[i]));
    }
    return units;
}

} // namespace

template <typename Unit> void Arguments::split(const Unit* line)
{
    const auto unit  = [line](usize at) { return static_cast<u32>(static_cast<u16>(line[at])); };
    const auto blank = [](u32 c) { return c == u32{' '} || c == u32{'\t'}; };
    usize i      = 0U;
    bool  quoted = false;

    // The program name: quotes toggle and are dropped, a backslash is itself.
    for (u32 c = unit(i); c != 0U && (quoted || !blank(c)); c = unit(i))
    {
        if (c == u32{'"'})
        {
            quoted = !quoted;
            ++i;
        }
        else
        {
            i += append_code_point(line + i, m_bytes);
        }
    }
    m_bytes.push_back('\0');
    m_count = 1;

    quoted = false;
    for (;;)
    {
        while (blank(unit(i)))
        {
            ++i;
        }
        if (unit(i) == 0U)
        {
            break;
        }
        for (;;)
        {
            bool  copy        = true;
            usize backslashes = 0U;
            while (unit(i) == u32{'\\'})
            {
                ++backslashes;
                ++i;
            }
            if (unit(i) == u32{'"'})
            {
                if (backslashes % 2U == 0U)
                {
                    if (quoted && unit(i + 1U) == u32{'"'})
                    {
                        ++i; // two quotes inside a quoted run: the second is a literal quote
                    }
                    else
                    {
                        copy   = false;
                        quoted = !quoted;
                    }
                }
                backslashes /= 2U;
            }
            for (; backslashes > 0U; --backslashes)
            {
                m_bytes.push_back('\\');
            }
            const u32 c = unit(i);
            if (c == 0U || (!quoted && blank(c)))
            {
                break;
            }
            i += copy ? append_code_point(line + i, m_bytes) : 1U;
        }
        m_bytes.push_back('\0');
        ++m_count;
    }

    // The pointers are taken once the bytes no longer move.
    m_pointers.resize(static_cast<usize>(m_count) + 1U);
    char* argument = m_bytes.data();
    for (int a = 0; a < m_count; ++a)
    {
        m_pointers[static_cast<usize>(a)] = argument;
        while (*argument != '\0')
        {
            ++argument;
        }
        ++argument;
    }
    m_pointers[static_cast<usize>(m_count)] = nullptr;
    m_values                                = m_pointers.data();
}

#if CRD_OS_WINDOWS
Arguments::Arguments(int /*argc*/, char** /*argv*/)
{
    split(::GetCommandLineW());
}
#else
Arguments::Arguments(int argc, char** argv) : m_count(argc), m_values(argv)
{
}
#endif

Arguments Arguments::from_windows_command_line(const char16_t* line)
{
    Arguments arguments;
    arguments.split(line);
    return arguments;
}

} // namespace crd::platform
