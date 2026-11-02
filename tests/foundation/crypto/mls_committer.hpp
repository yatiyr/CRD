#pragma once

// crd-crypto tests -- a committer the MLS suites make by hand (test_mls_group.cpp, test_mls_resume.cpp): the epoch a
// group a test holds starts by adding joiners, every input of it chosen (the path, the tree in the GroupInfo, the
// PSKs, a wrong tag, signer or path secret), and the Welcome for them, so a joiner's refusal of any fault a real
// committer would never make can be checked; and the encodings it writes.

#include "mls_test_support.hpp"

#include <crd/crypto/mls_group.hpp>

#include <catch2/catch_test_macros.hpp>

namespace crd::crypto::test
{
using containers::ConstSpan;
using containers::WireWriter;

template <typename T, typename Size, typename Put>
inline Bytes written(const T& value, Size size, Put put)
{
    Bytes out = filled(size(value), 0U);
    REQUIRE(!out.empty());
    WireWriter writer(span_of(out));
    put(writer, value);
    REQUIRE(writer.written() == out.size());
    return out;
}

inline Bytes context_bytes(const mls::GroupContext& context)
{
    return written(context, mls::group_context_bytes, mls::put_group_context);
}

inline Bytes group_info_written(const mls::GroupInfo& info)
{
    return written(info, mls::group_info_bytes, mls::put_group_info);
}

// One extension of `type` with `data`, as an Extension list's entry.
inline Bytes extension(u16 type, ConstSpan<u8> data)
{
    Bytes      out = filled(2U + mls::vector_bytes(data.size()), 0U);
    WireWriter writer(span_of(out));
    writer.put_u16(type);
    mls::put_vector(writer, data);
    return out;
}

// How a commit that adds joiners is made.
struct AddOptions
{
    bool                          path         = true;
    bool                          tree_in_info = true;
    ConstSpan<mls::PreSharedKey>  psks;
    bool                          wrong_tag    = false; // a GroupInfo whose confirmation tag is not the epoch's
    bool                          wrong_signer = false; // a GroupInfo signed by a key that is not the signer leaf's
    bool                          wrong_path   = false; // a path secret that is not the joiner's
};

// The epoch a committer starts by adding joiners to its group (the commit itself is not modelled: a joiner never
// sees it, only its transcript hash in the GroupInfo), and the Welcome for them.
struct NextEpoch
{
    mls::RatchetTree  tree;
    mls::PathUpdate   update;
    mls::EpochSecrets secrets;
    Bytes             path;
    Bytes             confirmed;
    Bytes             tag;
    Bytes             tree_bytes;
    Bytes             extensions;
    Bytes             signature;
    Bytes             group_info;
    Bytes             welcome;
    u32               leaves[8] = {};
};

inline void commit_adds(const mls::CipherSuite& suite, const Identity& committer, const mls::Group& group,
                        Joiner* joiners, usize count, const AddOptions& options, NextEpoch& out)
{
    const usize             hash = suite.hash_bytes();
    const mls::GroupContext old  = group.context();
    REQUIRE(out.tree.copy_from(group.tree()) == CryptoError::None);
    for (usize i = 0U; i < count; ++i)
    {
        REQUIRE(out.tree.add(joiners[i].package.leaf_node, out.leaves[i]) == CryptoError::None);
    }
    Bytes commit_secret = filled(hash, 0U);
    if (options.path)
    {
        mls::LeafNode template_leaf{};
        REQUIRE(out.tree.leaf_node(group.own_leaf(), template_leaf));
        const mls::GroupContext provisional{suite.id, old.group_id, old.epoch + 1U, {},
                                            old.confirmed_transcript_hash, old.extensions};
        crd::containers::Array<u8> path;
        REQUIRE(mls::create_update_path(out.tree, group.own_leaf(), committer.signer, template_leaf, provisional,
                                        ConstSpan<u32>(out.leaves, count), path, out.update) == CryptoError::None);
        out.path      = Bytes(path.begin(), path.end());
        commit_secret = Bytes(out.update.commit_secret().begin(), out.update.commit_secret().end());
    }
    Bytes psk_secret = filled(hash, 0U);
    REQUIRE(mls::psk_secret(suite, options.psks, span_of(psk_secret)) == CryptoError::None);
    out.confirmed = random_of(hash);
    const mls::GroupContext next{suite.id,          old.group_id, old.epoch + 1U, out.tree.root_hash(),
                                 view(out.confirmed), old.extensions};
    REQUIRE(out.secrets.advance(suite, group.secret(mls::EpochSecret::Init), commit_secret, psk_secret,
                                context_bytes(next)) == CryptoError::None);
    out.tag = filled(hash, 0U);
    REQUIRE(mls::confirmation_tag(suite, out.secrets.get(mls::EpochSecret::Confirmation), view(out.confirmed),
                                  span_of(out.tag)) == CryptoError::None);
    if (options.wrong_tag)
    {
        out.tag[0] ^= 0x01U;
    }
    out.tree_bytes = filled(out.tree.encoded_bytes(), 0U);
    WireWriter tree_writer(span_of(out.tree_bytes));
    out.tree.encode(tree_writer);
    out.extensions = options.tree_in_info ? extension(2U, view(out.tree_bytes)) : Bytes();

    mls::GroupInfo info;
    info.context          = next;
    info.extensions       = view(out.extensions);
    info.confirmation_tag = view(out.tag);
    info.signer           = group.own_leaf();
    Identity stranger;
    if (options.wrong_signer)
    {
        make_identity(suite, stranger, 99U);
    }
    out.signature = filled(signature_sizes(suite.signature).signature, 0U);
    usize written_bytes = 0U;
    REQUIRE(mls::sign_group_info(suite, options.wrong_signer ? stranger.signer : committer.signer, info,
                                 span_of(out.signature), written_bytes) == CryptoError::None);
    out.signature.resize(written_bytes);
    info.signature = view(out.signature);
    out.group_info = group_info_written(info);

    crd::containers::Array<mls::WelcomeMember> members;
    for (usize i = 0U; i < count; ++i)
    {
        mls::WelcomeMember member;
        member.package = joiners[i].package;
        if (options.path)
        {
            const u32 ancestor =
                *mls::tree::common_ancestor(2U * group.own_leaf(), 2U * out.leaves[i], out.tree.leaves());
            member.path_secret = out.update.path_secret(options.wrong_path ? *mls::tree::root(out.tree.leaves())
                                                                           : ancestor);
            if (options.wrong_path && ancestor == *mls::tree::root(out.tree.leaves()))
            {
                member.path_secret = out.update.commit_secret();
            }
            REQUIRE(member.path_secret.size() == hash);
        }
        members.push_back(member);
    }
    crd::containers::Array<u8> welcome;
    REQUIRE(mls::seal_welcome(suite, out.secrets.get(mls::EpochSecret::Joiner), options.psks, view(out.group_info),
                              ConstSpan<mls::WelcomeMember>(members.data(), members.size()), welcome) ==
            CryptoError::None);
    out.welcome = Bytes(welcome.begin(), welcome.end());
}
} // namespace crd::crypto::test
