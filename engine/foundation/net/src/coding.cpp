// crd-net -- coded bodies (NET.3; docs/design/network-foundation.md#body-coding).

#include <crd/compression/entropy.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/coding.hpp>

namespace crd::net
{
namespace
{
// A level past the default costs more per message than it saves on messages this small.
constexpr int kDictionaryLevel = compression::kZstdDefaultLevel;

// A compiled object that could not start is no object.
template <typename T> void drop_unless(bool ready, memory::IAllocator& allocator, T*& object) noexcept
{
    if (!ready)
    {
        memory::destroy(allocator, object);
        object = nullptr;
    }
}
} // namespace

BodyModel::BodyModel(memory::IAllocator& allocator, wire::CodecKind kind, cont::ConstSpan<u8> source,
                     u32 max_message_bytes) noexcept
    : m_allocator(allocator), m_kind(kind), m_hash(cont::xxh3_64(source))
{
    if (kind == wire::CodecKind::Entropy)
    {
        m_entropy = memory::construct<compression::EntropyModel>(allocator);
        drop_unless(m_entropy != nullptr && m_entropy->load(source), allocator, m_entropy);
        return;
    }
    m_dictionary = memory::construct<compression::ZstdDictionary>(allocator, allocator, source, max_message_bytes,
                                                                  kDictionaryLevel);
    drop_unless(m_dictionary != nullptr && m_dictionary->ready(), allocator, m_dictionary);
}

BodyModel::~BodyModel()
{
    memory::destroy(m_allocator, m_entropy);
    memory::destroy(m_allocator, m_dictionary);
}

BodyCoder::BodyCoder(memory::IAllocator& allocator, u32 max_message_bytes) noexcept
    : m_allocator(allocator), m_scratch_bytes(max_message_bytes)
{
    m_coder =
        memory::construct<compression::ZstdMessageCoder>(allocator, allocator, max_message_bytes, kDictionaryLevel);
    drop_unless(m_coder != nullptr && m_coder->ready(), allocator, m_coder);
    void* const scratch = allocator.try_allocate(max_message_bytes, 1U);
    if (scratch != nullptr)
    {
        m_scratch = static_cast<u8*>(scratch);
    }
}

BodyCoder::~BodyCoder()
{
    memory::destroy(m_allocator, m_coder);
    m_allocator.deallocate(m_scratch);
}

usize BodyCoder::encode(const BodyModel& model, u16 max_ratio, cont::ConstSpan<u8> plain, cont::Span<u8> out) noexcept
{
    // Past the plain size coding has lost: no codec spends longer than that finding out.
    const cont::Span<u8> room = out.first((plain.size() < out.size()) ? plain.size() : out.size());
    const usize coded = (model.m_entropy != nullptr) ? model.m_entropy->encode(plain, room)
                                                     : m_coder->compress(*model.m_dictionary, plain, room);
    return expansion_holds(coded, plain.size(), max_ratio) ? coded : 0U;
}

bool BodyCoder::decode(const BodyModel& model, cont::ConstSpan<u8> coded, cont::Span<u8> out) noexcept
{
    return (model.m_entropy != nullptr) ? model.m_entropy->decode(coded, out)
                                        : m_coder->decompress_exact(*model.m_dictionary, coded, out);
}

} // namespace crd::net
