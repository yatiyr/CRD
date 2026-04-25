#pragma once

#include <crd/log/log_sink.hpp>

namespace crd::log
{
// Sends formatted records to the platform debugger:
//   Windows : OutputDebugStringA
//   others  : no-op (still constructs / writes successfully so user code is portable)
//
// Useful during development: messages show up in Visual Studio's Output window
// without needing a terminal attached.
class DebuggerSink : public ISink
{
public:
    DebuggerSink() noexcept = default;
    ~DebuggerSink() override = default;

    void write(const LogRecord& rec) override;
    void flush() override;
};
} // namespace crd::log
