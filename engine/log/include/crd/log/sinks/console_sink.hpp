#pragma once

#include <crd/log/log_sink.hpp>

namespace crd::log
{
// Writes formatted records to stdout / stderr.
// - Errors and Critical go to stderr; everything else to stdout.
// - When the destination is a TTY, ANSI color codes are emitted per-level.
//   On Windows, the ENABLE_VIRTUAL_TERMINAL_PROCESSING console mode is set
//   on the relevant handle; we never patch the parent process's mode back.
class ConsoleSink : public ISink
{
public:
    ConsoleSink() noexcept;
    explicit ConsoleSink(bool force_color) noexcept;
    ~ConsoleSink() override = default;

    void write(const LogRecord& rec) override;
    void flush() override;

    bool color_enabled() const noexcept { return m_color; }

private:
    bool m_color = false;
};
} // namespace crd::log
