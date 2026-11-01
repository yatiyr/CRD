// Example: the package's compression codecs (OWN.1).
//
//   diag-consumer codecs
//
// The diagnostic host compresses its bundle sections through Cerid::compression, which the package carries with the
// pinned zstd archive behind it, link-only: an application reaches zstd only through crd/compression/zstd.hpp and
// never includes zstd's own header. This route compresses one text with each codec, decodes it into a buffer of
// exactly its size and compares it with the original, and prints CONSUMER_CODECS with the sizes and the verdict, so
// the zstd edge is proven to resolve from the installed prefix alone.
#include "examples.hpp"

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace diag_consumer
{

int example_codecs()
{
    namespace cc = crd::compression;
    namespace cont = crd::containers;

    // A log the application might keep: the same line with a changing number, which both codecs shrink.
    cont::Array<crd::u8> text;
    for (crd::u32 order = 0; order < 200U; ++order)
    {
        char line[64];
        const int n = std::snprintf(line, sizeof(line), "order %u booked, total still positive\n", order);
        for (int i = 0; i < n; ++i)
        {
            text.push_back(static_cast<crd::u8>(line[i]));
        }
    }
    const cont::ConstSpan<crd::u8> original{text.data(), text.size()};

    const cont::Array<crd::u8> deflated = cc::deflate_raw(original, crd::memory::default_allocator());
    cont::Array<crd::u8> inflated;
    inflated.resize(text.size());
    const bool deflate_ok = cc::inflate_raw(cont::ConstSpan<crd::u8>{deflated.data(), deflated.size()},
                                            cont::Span<crd::u8>{inflated.data(), inflated.size()}) &&
                            std::memcmp(inflated.data(), text.data(), text.size()) == 0;

    cont::Array<crd::u8> frame;
    cont::Array<crd::u8> decoded;
    decoded.resize(text.size());
    const bool zstd_ok = cc::zstd_compress(original, cc::kZstdDefaultLevel, frame) &&
                         cc::zstd_decompress_exact(cont::ConstSpan<crd::u8>{frame.data(), frame.size()},
                                                   cont::Span<crd::u8>{decoded.data(), decoded.size()}) &&
                         std::memcmp(decoded.data(), text.data(), text.size()) == 0;

    std::printf("CONSUMER_CODECS bytes=%zu deflate=%zu zstd=%zu deflate_round_trip=%d zstd_round_trip=%d\n",
                static_cast<std::size_t>(text.size()), static_cast<std::size_t>(deflated.size()),
                static_cast<std::size_t>(frame.size()), deflate_ok ? 1 : 0, zstd_ok ? 1 : 0);
    return deflate_ok && zstd_ok ? 0 : 6;
}

} // namespace diag_consumer
