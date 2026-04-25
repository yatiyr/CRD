#pragma once

#include <crd/log/log_sink.hpp>

namespace crd::log
{
// Discards every record. Useful for benchmarks (measure call-site cost only)
// and for unit tests that want a default sink without I/O noise.
class NullSink : public ISink
{
public:
    void write(const LogRecord&) override {}
    void flush() override {}
};
} // namespace crd::log
