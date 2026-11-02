// crd-crypto -- the signature list every signed Cerid artefact ends with, and its trust check (signed_artifact.hpp).
#include "signed_artifact.hpp"

namespace crd::crypto::signed_artifact
{
bool is_signature(AlgorithmId algorithm) noexcept
{
    const AlgorithmClass type = algorithm_class(algorithm);
    return type == AlgorithmClass::PostQuantumSignature || type == AlgorithmClass::ClassicalSignature;
}

Error read(cont::WireReader& reader, cont::Span<KeySignature> out, u32& count) noexcept
{
    count       = 0U;
    u8 declared = 0U;
    if (!reader.take_u8(declared))
    {
        return Error::Truncated;
    }
    if (declared == 0U)
    {
        return Error::Unsigned;
    }
    if (declared > out.size())
    {
        return Error::TooLarge;
    }
    for (u32 i = 0U; i < declared; ++i)
    {
        KeySignature&       signature = out[i];
        cont::ConstSpan<u8> key;
        u16                 algorithm = 0U;
        u16                 length    = 0U;
        if (!reader.take_bytes(kKeyIdBytes, key) || !reader.take_u16(algorithm) || !reader.take_u16(length))
        {
            return Error::Truncated;
        }
        for (usize b = 0U; b < kKeyIdBytes; ++b)
        {
            signature.key.bytes[b] = key[b];
        }
        signature.algorithm = static_cast<AlgorithmId>(algorithm);
        if (length > kMaxSignatureBytes)
        {
            return Error::TooLarge;
        }
        if (!reader.take_bytes(length, signature.signature))
        {
            return Error::Truncated;
        }
    }
    count = declared;
    return Error::None;
}

Error validate(cont::ConstSpan<KeySignature> signatures, usize capacity) noexcept
{
    if (signatures.empty())
    {
        return Error::Unsigned;
    }
    if (signatures.size() > capacity)
    {
        return Error::TooLarge;
    }
    for (usize i = 0U; i < signatures.size(); ++i)
    {
        if (signatures[i].signature.empty())
        {
            return Error::BadEntry;
        }
        if (signatures[i].signature.size() > kMaxSignatureBytes)
        {
            return Error::TooLarge;
        }
        for (usize j = 0U; j < i; ++j)
        {
            if (signatures[j].key == signatures[i].key)
            {
                return Error::Duplicate;
            }
        }
    }
    return Error::None;
}

usize encoded_size(cont::ConstSpan<KeySignature> signatures) noexcept
{
    usize size = 1U;
    for (const KeySignature& signature : signatures)
    {
        size += kEntryHeadBytes + signature.signature.size();
    }
    return size;
}

void write(cont::WireWriter& writer, cont::ConstSpan<KeySignature> signatures) noexcept
{
    writer.put_u8(static_cast<u8>(signatures.size()));
    for (const KeySignature& signature : signatures)
    {
        writer.put_bytes(cont::ConstSpan<u8>(signature.key.bytes, kKeyIdBytes));
        writer.put_u16(static_cast<u16>(signature.algorithm));
        writer.put_u16(static_cast<u16>(signature.signature.size()));
        writer.put_bytes(signature.signature);
    }
}

Trust verify_all(cont::ConstSpan<TrustKey> required, const SignatureVerifier& verifier, cont::ConstSpan<u8> message,
                 cont::ConstSpan<KeySignature> signatures) noexcept
{
    for (const TrustKey& key : required)
    {
        const KeySignature* found = nullptr;
        for (const KeySignature& signature : signatures)
        {
            if (signature.key == key.id)
            {
                found = &signature; // validate() refused a second entry by one key
                break;
            }
        }
        if (found == nullptr)
        {
            return Trust::Missing;
        }
        if (found->algorithm != key.algorithm)
        {
            return Trust::Mismatch;
        }
        switch (verifier.verify(key.algorithm, key.public_key, message, found->signature))
        {
        case Verification::Valid:
            break;
        case Verification::Unsupported:
            return Trust::Unsupported;
        case Verification::Invalid:
            return Trust::Invalid;
        }
    }
    return Trust::Valid;
}
} // namespace crd::crypto::signed_artifact
