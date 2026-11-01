#pragma once

// crd-platform -- the standard streams of a process that speaks a byte protocol over them.
//
// ceridc's MCP stdio transport reads one JSON-RPC message per line from stdin and writes one per line to stdout, and
// the protocol allows nothing else on stdout. Two things break that unless the process claims its streams first:
//   * the Windows C runtime opens the streams in text mode: a written "\n" goes out as "\r\n", a read "\r\n" arrives
//     as "\n", and a read 0x1A byte ends the input as if the stream had closed (POSIX streams carry bytes unchanged);
//   * any library the process links may print to stdout (the cooker prints its summary there), and one such line is
//     a corrupt message to the client.
// claim_protocol_streams puts stdin in binary mode and moves the protocol onto its own stream: a binary stream on a
// duplicate of the original standard output. Standard output itself then points at standard error, so a stray write
// from anywhere in the process reaches the client's log, never its protocol.

#include <cstdio>

namespace crd::platform
{

// Call once, before the first read of stdin or write of stdout. Answers the stream the protocol writes to (binary;
// the caller closes it at exit), or nullptr when the runtime refused a step: the process cannot serve the protocol
// then, and says so on stderr.
[[nodiscard]] std::FILE* claim_protocol_streams() noexcept;

} // namespace crd::platform
