// standard_streams.cpp -- the standard streams of a byte protocol (crd/platform/standard_streams.hpp).

#include <crd/platform/standard_streams.hpp>

#if defined(_WIN32)
#include <fcntl.h> // _O_BINARY
#include <io.h>    // _setmode, _dup, _dup2, _fdopen, _close
#else
#include <unistd.h> // dup, dup2, close
#endif

namespace crd::platform
{

namespace
{
#if defined(_WIN32)
    [[nodiscard]] int descriptor(std::FILE* stream) noexcept
    {
        return _fileno(stream);
    }
    [[nodiscard]] int duplicate(int fd) noexcept
    {
        return _dup(fd);
    }
    [[nodiscard]] bool point(int from, int to) noexcept
    {
        return _dup2(from, to) == 0;
    }
    void close_fd(int fd) noexcept
    {
        (void)_close(fd);
    }
    [[nodiscard]] std::FILE* open_binary(int fd) noexcept
    {
        return _setmode(fd, _O_BINARY) == -1 ? nullptr : _fdopen(fd, "wb");
    }
#else
    [[nodiscard]] int descriptor(std::FILE* stream) noexcept
    {
        return ::fileno(stream);
    }
    [[nodiscard]] int duplicate(int fd) noexcept
    {
        return ::dup(fd);
    }
    [[nodiscard]] bool point(int from, int to) noexcept
    {
        return ::dup2(from, to) == to;
    }
    void close_fd(int fd) noexcept
    {
        (void)::close(fd);
    }
    [[nodiscard]] std::FILE* open_binary(int fd) noexcept
    {
        return ::fdopen(fd, "wb");
    }
#endif
} // namespace

std::FILE* claim_protocol_streams() noexcept
{
#if defined(_WIN32)
    // _setmode answers the previous mode, or -1 when it refuses.
    if (_setmode(descriptor(stdin), _O_BINARY) == -1)
    {
        return nullptr;
    }
#endif
    (void)std::fflush(stdout); // nothing written so far is left in the buffer of the descriptor about to move
    const int protocol = duplicate(descriptor(stdout));
    if (protocol == -1)
    {
        return nullptr;
    }
    std::FILE* const out = open_binary(protocol);
    if (out == nullptr)
    {
        close_fd(protocol);
        return nullptr;
    }
    if (!point(descriptor(stderr), descriptor(stdout)))
    {
        (void)std::fclose(out);
        return nullptr;
    }
    return out;
}

} // namespace crd::platform
