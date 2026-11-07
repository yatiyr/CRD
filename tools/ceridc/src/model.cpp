// ceridc model -- a channel's body model, trained and measured (NET.3; contract:
// docs/design/network-foundation.md#body-coding). The trainers are crd-compression's and the coder crd-net's, so a
// model this verb writes is the one a session loads, its hash the one a schema pins, and what it measures is what a
// channel's writer would send: each sample coded alone, stored as it is when coding does not make it smaller.

#include <crd/ceridc/verbs.hpp>

#include "decimal.hpp"

#include <crd/compression/entropy.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/json/json_write.hpp>
#include <crd/net/coding.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio>
#include <cstring>

namespace crd::ceridc
{
namespace
{
namespace cont = crd::containers;
namespace fs = crd::platform::fs;
using crd::json::JsonWriter;
using crd::net::BodyCoder;
using crd::net::BodyModel;
using crd::net::wire::CodecKind;

constexpr const char* kVerb = "model";
constexpr crd::u64 kMaxMessageBytes = crd::u64{64U} << 20U; // the largest message a run codes
constexpr crd::u64 kMaxSampleBytes = crd::u64{1U} << 30U;   // every sample together
constexpr crd::u64 kDefaultDictionaryBytes = 4096U;           // what S32 measured on small messages
constexpr crd::u64 kMinDictionaryBytes = 256U;                // zstd's least (ZDICT_DICTSIZE_MIN)
constexpr crd::u64 kMaxDictionaryBytes = crd::u64{1U} << 20U;
constexpr crd::u64 kDefaultHoldout = 5U;
constexpr crd::u64 kMaxModelBytes = kMaxDictionaryBytes; // a measured model's source
constexpr crd::u16 kWidestRatio = 65535U;                // what a schema's max_ratio can hold
constexpr crd::u64 kCorpusPerRoom = 10U; // zstd's least sample bytes per dictionary byte

// Why a request or an input was refused, and the file when one was.
struct Failure
{
    const char* reason = nullptr;
    cont::String file;
};

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* action, const Failure& failure)
{
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", false);
    w.kv("action", action != nullptr ? action : "");
    w.kv("reason", failure.reason);
    if (!failure.file.empty())
    {
        w.kv("file", cont::StringView(failure.file));
    }
    w.end_object();
    return cont::String(w.str());
}

[[nodiscard]] cont::String refuse(crd::memory::IAllocator* alloc, const char* action, const char* reason)
{
    Failure failure;
    failure.reason = reason;
    return refuse(alloc, action, failure);
}

[[nodiscard]] bool fail(Failure& failure, const char* reason, cont::StringView file = {})
{
    failure.reason = reason;
    failure.file = cont::String(file);
    return false;
}

// `text` (nullptr: `fallback`) as a number from `least` to `most`.
[[nodiscard]] bool read_bounded(const char* text, crd::u64 fallback, crd::u64 least, crd::u64 most, crd::u64& out)
{
    out = fallback;
    return (text == nullptr || read_decimal(cont::StringView(text), out)) && out >= least && out <= most;
}

// Messages back to back, with each one's size.
struct Samples
{
    explicit Samples(crd::memory::IAllocator* alloc) : bytes(alloc), sizes(alloc) {}

    void add(cont::ConstSpan<crd::u8> sample)
    {
        const crd::usize at = bytes.size();
        bytes.resize(at + sample.size());
        std::memcpy(bytes.data() + at, sample.data(), sample.size());
        sizes.push_back(sample.size());
    }

    cont::Array<crd::u8> bytes;
    cont::Array<crd::usize> sizes;
};

// The files of `directory` in name order, every `holdout`th one (0: none) into `held`, the rest into `trained` (never
// empty: holdout is never 1), each one message of 1 to `most` bytes; `largest` is the largest.
[[nodiscard]] bool read_samples(const char* directory, crd::u64 holdout, crd::u64 most, Samples& trained,
                                Samples& held, crd::u64& largest, crd::memory::IAllocator* alloc, Failure& failure)
{
    const fs::Path root{cont::StringView(directory)};
    if (!fs::is_directory(root))
    {
        return fail(failure, "samples-not-a-directory", directory);
    }
    cont::Array<fs::Path> files(alloc);
    fs::list_directory(root, files);
    cont::sort(files.begin(), files.end(),
               [](const fs::Path& a, const fs::Path& b)
               {
                   return a.generic() < b.generic();
               });
    if (files.empty())
    {
        return fail(failure, "no-samples", directory);
    }
    cont::Array<crd::u8> sample(alloc);
    crd::u64 total = 0U;
    largest = 0U;
    for (crd::usize index = 0U; index < files.size(); ++index)
    {
        const fs::Path& file = files[index];
        if (!fs::is_file(file))
        {
            return fail(failure, "sample-not-a-file", file.generic());
        }
        const crd::u64 size = fs::file_size(file);
        if (size > most)
        {
            return fail(failure, "sample-too-large", file.generic());
        }
        total += size;
        if (total > kMaxSampleBytes)
        {
            return fail(failure, "samples-too-large", directory);
        }
        sample.clear();
        if (!fs::read_file_binary(file, sample) || sample.size() != size)
        {
            return fail(failure, "unreadable-sample", file.generic());
        }
        if (sample.empty())
        {
            return fail(failure, "empty-sample", file.generic());
        }
        largest = size > largest ? size : largest;
        const bool held_out = holdout != 0U && (index + 1U) % holdout == 0U;
        (held_out ? held : trained).add(cont::ConstSpan<crd::u8>(sample.data(), sample.size()));
    }
    return true;
}

// What coding a set of messages sent, as a channel's writer codes them.
struct Tally
{
    crd::u64 messages = 0U;
    crd::u64 plain = 0U;
    crd::u64 sent = 0U;
    crd::u64 coded = 0U;
    crd::u64 widest = 0U; // the most a coded message expanded, rounded up
};

// Codes each message of `samples` and decodes it back. False when a decode did not give the message back.
[[nodiscard]] bool measure(BodyCoder& coder, const BodyModel& model, const Samples& samples,
                           cont::Span<crd::u8> check, Tally& tally)
{
    const cont::Span<crd::u8> room = coder.scratch();
    crd::usize at = 0U;
    for (const crd::usize size : samples.sizes)
    {
        const cont::ConstSpan<crd::u8> plain(samples.bytes.data() + at, size);
        at += size;
        ++tally.messages;
        tally.plain += size;
        const crd::usize coded = coder.encode(model, kWidestRatio, plain, room);
        if (coded == 0U)
        {
            tally.sent += size;
            continue;
        }
        const cont::Span<crd::u8> back(check.data(), size);
        if (!coder.decode(model, cont::ConstSpan<crd::u8>(room.data(), coded), back) ||
            std::memcmp(back.data(), plain.data(), size) != 0)
        {
            return false;
        }
        ++tally.coded;
        tally.sent += coded;
        const crd::u64 ratio = (size + coded - 1U) / coded;
        tally.widest = ratio > tally.widest ? ratio : tally.widest;
    }
    return true;
}

void write_tally(JsonWriter& w, const char* key, const Tally& tally)
{
    w.key(key);
    w.begin_object();
    w.kv("messages", tally.messages);
    w.kv("plain_bytes", tally.plain);
    w.kv("sent_bytes", tally.sent);
    w.kv("coded", tally.coded);
    w.kv("stored", tally.messages - tally.coded);
    w.kv("sent_permille", tally.plain == 0U ? crd::u64{0U} : ((tally.sent * 1000U) + (tally.plain / 2U)) / tally.plain);
    w.kv("widest_ratio", tally.widest);
    w.end_object();
}

// The least power of two above the widest ratio seen, which a schema's max_ratio may hold: a message coded wider than
// its channel's ratio is only sent as it is, so a message coding a little better than any sample still pays.
[[nodiscard]] crd::u64 ratio_for(crd::u64 widest)
{
    crd::u64 ratio = 2U;
    while (ratio <= widest && ratio < kWidestRatio)
    {
        ratio *= 2U;
    }
    return ratio < kWidestRatio ? ratio : kWidestRatio;
}

// The report both actions share: the model, each set's tally and the schema's entry when any message was coded.
// `room` is a trained dictionary's: zstd asks for ten times its size in samples, and less is reported.
[[nodiscard]] cont::String report(crd::memory::IAllocator* alloc, const char* action, const char* kind,
                                  const char* file, cont::ConstSpan<crd::u8> source, const BodyModel& model,
                                  crd::u64 max_message_bytes, crd::u64 room, const Tally& first,
                                  const char* first_key, const Tally* held)
{
    char hash[17];
    (void)std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(model.hash()));
    const crd::u64 widest = held != nullptr && held->widest > first.widest ? held->widest : first.widest;
    JsonWriter w(alloc);
    w.begin_object();
    w.kv("verb", kVerb);
    w.kv("ok", true);
    w.kv("action", action);
    w.kv("kind", kind);
    w.kv("file", file);
    w.kv("bytes", static_cast<crd::u64>(source.size()));
    w.kv("hash", hash);
    w.kv("max_message_bytes", max_message_bytes);
    if (first.plain < room * kCorpusPerRoom)
    {
        w.kv("warning", "small-corpus");
    }
    write_tally(w, first_key, first);
    if (held != nullptr)
    {
        write_tally(w, "held_out", *held);
    }
    // Whether coding pays where it was measured fairly: on the messages it did not see, when there were any.
    const Tally& judged = held != nullptr ? *held : first;
    w.kv("pays", judged.sent < judged.plain);
    if (widest != 0U)
    {
        w.key("entry");
        w.begin_object();
        w.kv("kind", kind);
        w.kv("hash", hash);
        w.kv("max_ratio", ratio_for(widest));
        w.end_object();
    }
    w.end_object();
    return cont::String(w.str());
}

// `source` written to `path` whole or not at all.
[[nodiscard]] bool publish(const char* path, cont::ConstSpan<crd::u8> source)
{
    cont::String partial(path);
    partial += ".partial";
    const fs::Path staged{cont::StringView(partial)};
    if (!fs::write_file_binary(staged, source))
    {
        return false;
    }
    if (!fs::rename_file(staged, fs::Path(path)))
    {
        (void)fs::remove_file(staged);
        return false;
    }
    return true;
}
} // namespace

cont::String verb_model(const ModelFlags& flags, crd::memory::IAllocator* alloc)
{
    const char* action = flags.action;
    const bool train = action != nullptr && std::strcmp(action, "train") == 0;
    if (!train && (action == nullptr || std::strcmp(action, "measure") != 0))
    {
        return refuse(alloc, action, "unknown-action");
    }
    if (flags.kind == nullptr ||
        (std::strcmp(flags.kind, "entropy") != 0 && std::strcmp(flags.kind, "dictionary") != 0))
    {
        return refuse(alloc, action, "unknown-kind");
    }
    const CodecKind kind = std::strcmp(flags.kind, "entropy") == 0 ? CodecKind::Entropy : CodecKind::Dictionary;
    if (flags.samples == nullptr || (train ? flags.out == nullptr || flags.model != nullptr
                                           : flags.model == nullptr || flags.out != nullptr))
    {
        return refuse(alloc, action, train ? "train needs --samples and --out" : "measure needs --samples and --model");
    }
    crd::u64 dictionary_bytes = 0U;
    if ((!train || kind != CodecKind::Dictionary) && flags.dictionary_bytes != nullptr)
    {
        return refuse(alloc, action, "--dictionary-bytes is a trained dictionary's");
    }
    if (!read_bounded(flags.dictionary_bytes, kDefaultDictionaryBytes, kMinDictionaryBytes, kMaxDictionaryBytes,
                      dictionary_bytes))
    {
        return refuse(alloc, action, "--dictionary-bytes is from 256 to 1048576");
    }
    crd::u64 holdout = 0U;
    if (!train && flags.holdout != nullptr)
    {
        return refuse(alloc, action, "--holdout is a train's");
    }
    if (!read_bounded(flags.holdout, train ? kDefaultHoldout : 0U, 0U, ~crd::u64{0U}, holdout) || holdout == 1U)
    {
        return refuse(alloc, action, "--holdout is 0 or from 2");
    }
    crd::u64 most = 0U;
    if (!read_bounded(flags.max_message_bytes, kMaxMessageBytes, 1U, kMaxMessageBytes, most))
    {
        return refuse(alloc, action, "--max-message-bytes is from 1 to 67108864");
    }

    Failure failure;
    Samples trained(alloc);
    Samples held(alloc);
    crd::u64 largest = 0U;
    if (!read_samples(flags.samples, holdout, most, trained, held, largest, alloc, failure))
    {
        return refuse(alloc, action, failure);
    }
    const crd::u64 max_message_bytes = flags.max_message_bytes != nullptr ? most : largest;

    cont::Array<crd::u8> source(alloc);
    if (train && kind == CodecKind::Entropy)
    {
        crd::compression::EntropyTrainer trainer;
        crd::usize at = 0U;
        for (const crd::usize size : trained.sizes)
        {
            trainer.add(cont::ConstSpan<crd::u8>(trained.bytes.data() + at, size));
            at += size;
        }
        source.resize(crd::compression::kEntropyModelBytes);
        trainer.write_model(cont::Span<crd::u8>(source.data(), source.size()));
    }
    else if (train)
    {
        source.resize(static_cast<crd::usize>(dictionary_bytes));
        const crd::usize written = crd::compression::zstd_train_dictionary(
            cont::ConstSpan<crd::u8>(trained.bytes.data(), trained.bytes.size()),
            cont::ConstSpan<crd::usize>(trained.sizes.data(), trained.sizes.size()),
            cont::Span<crd::u8>(source.data(), source.size()));
        if (written == 0U)
        {
            return refuse(alloc, action, "training-failed");
        }
        source.resize(written);
    }
    else
    {
        const fs::Path path(flags.model);
        if (!fs::is_file(path) || fs::file_size(path) > kMaxModelBytes || !fs::read_file_binary(path, source))
        {
            Failure unreadable;
            (void)fail(unreadable, "unreadable-model", flags.model);
            return refuse(alloc, action, unreadable);
        }
    }

    crd::memory::IAllocator& heap = alloc != nullptr ? *alloc : *crd::memory::default_allocator();
    const auto bound = static_cast<crd::u32>(max_message_bytes);
    const BodyModel model(heap, kind, cont::ConstSpan<crd::u8>(source.data(), source.size()), bound);
    if (!model.ready())
    {
        return refuse(alloc, action, train ? "model-refused" : "not-a-model");
    }
    BodyCoder coder(heap, bound);
    cont::Array<crd::u8> check(alloc);
    check.resize(static_cast<crd::usize>(max_message_bytes));
    if (!coder.ready())
    {
        return refuse(alloc, action, "out-of-memory");
    }
    const cont::Span<crd::u8> check_room(check.data(), check.size());
    Tally first;
    Tally held_tally;
    if (!measure(coder, model, trained, check_room, first) ||
        (!held.sizes.empty() && !measure(coder, model, held, check_room, held_tally)))
    {
        return refuse(alloc, action, "round-trip");
    }
    const cont::ConstSpan<crd::u8> bytes(source.data(), source.size());
    if (train && !publish(flags.out, bytes))
    {
        Failure unwritable;
        (void)fail(unwritable, "unwritable-output", flags.out);
        return refuse(alloc, action, unwritable);
    }
    const crd::u64 room = train && kind == CodecKind::Dictionary ? dictionary_bytes : 0U;
    return report(alloc, action, flags.kind, train ? flags.out : flags.model, bytes, model, max_message_bytes, room,
                  first, train ? "trained" : "samples", held.sizes.empty() ? nullptr : &held_tally);
}

} // namespace crd::ceridc
