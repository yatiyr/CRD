// NET.3: `ceridc model` trains and measures a channel's body model. Trained on the snapshot deltas the coded-frame tests
// use, it writes the very model tests/foundation/net/schemas/coding_test.json pins; a measured raw dictionary reports the
// hash that schema pins too. What it reports is what crd-net's coder sends, every refusal leaves no model behind, and the
// real binary gives the same bytes.

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/environment.hpp>
#include <crd/core/platform.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/coding.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include <coding_fixture.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace fs = crd::platform::fs;
namespace cont = crd::containers;
namespace net_test = crd::net::test;
using crd::u8;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::ceridc::ModelFlags;
using crd::net::BodyCoder;
using crd::net::BodyModel;
using crd::net::wire::CodecKind;

#if CRD_OS_WINDOWS
constexpr const char* kShellQuote = "\"";
#else
constexpr const char* kShellQuote = "";
#endif

cont::String model(const ModelFlags& flags)
{
    return crd::ceridc::verb_model(flags, crd::memory::default_allocator());
}

ModelFlags train(const char* kind, const char* samples, const char* out)
{
    ModelFlags flags;
    flags.action = "train";
    flags.kind = kind;
    flags.samples = samples;
    flags.out = out;
    return flags;
}

ModelFlags measure(const char* kind, const char* samples, const char* file)
{
    ModelFlags flags;
    flags.action = "measure";
    flags.kind = kind;
    flags.samples = samples;
    flags.model = file;
    return flags;
}

bool has(const cont::String& text, cont::StringView needle)
{
    return cont::StringView(text).find(needle) != cont::StringView::npos;
}

cont::String hex(u64 value)
{
    char text[17];
    (void)std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return cont::String(text);
}

cont::String decimal(u64 value)
{
    char text[24];
    (void)std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    return cont::String(text);
}

bool same(const cont::Array<u8>& a, const cont::Array<u8>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// The number after the first `"key":` in `text`.
u64 number_after(const cont::String& text, cont::StringView key)
{
    cont::String quoted("\"");
    quoted.append(key);
    quoted.append("\":");
    const usize at = cont::StringView(text).find(cont::StringView(quoted));
    REQUIRE(at != cont::StringView::npos);
    u64 value = 0U;
    for (usize index = at + quoted.size(); index < text.size() && text.data()[index] >= '0' && text.data()[index] <= '9';
         ++index)
    {
        value = (value * 10U) + static_cast<u64>(text.data()[index] - '0');
    }
    return value;
}

template <typename... Parts> cont::String joined(const Parts&... parts)
{
    cont::String out;
    (out.append(cont::StringView(parts)), ...);
    return out;
}

void write_bytes(const char* path, cont::ConstSpan<u8> bytes)
{
    REQUIRE(fs::write_file_binary(fs::Path(path), bytes));
}

void write_text(const char* path, cont::StringView text)
{
    REQUIRE(fs::write_file_text(fs::Path(path), text));
}

cont::Array<u8> read_bytes(const char* path)
{
    cont::Array<u8> bytes;
    REQUIRE(fs::read_file_binary(fs::Path(path), bytes));
    return bytes;
}

// Each message its own file under `directory`, in order by name.
void write_samples(const char* directory, const cont::Array<net_test::Bytes>& samples)
{
    REQUIRE(fs::create_directories(fs::Path(directory)));
    for (usize index = 0U; index < samples.size(); ++index)
    {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "%s/%04zu", directory, index);
        write_bytes(name, cont::ConstSpan<u8>(samples[index].data(), samples[index].size()));
    }
}

// Notes an event channel carries: the fixture's phrases in changing order, with numbers.
cont::Array<net_test::Bytes> notes(usize count)
{
    static constexpr const char* kPhrases[] = {"the player joined the room", "the player left the match",
                                               "score updated for team blue", "score updated for team red",
                                               "the server restarted the round", "connection lost",
                                               "connection restored"};
    constexpr usize phrase_count = sizeof(kPhrases) / sizeof(kPhrases[0]);
    cont::Array<net_test::Bytes> out;
    for (usize index = 0U; index < count; ++index)
    {
        char text[256];
        const int written = std::snprintf(text, sizeof(text), "%s; %s at %zu; %s", kPhrases[index % phrase_count],
                                          kPhrases[(index * 3U) % phrase_count], index * 37U,
                                          kPhrases[(index / 2U) % phrase_count]);
        REQUIRE(written > 0);
        out.push_back(net_test::bytes_of(cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), // NOLINT
                                                             static_cast<usize>(written))));
    }
    return out;
}

// What crd-net's writer sends for `samples` with `source`, told apart from the verb.
u64 sent_by_coder(CodecKind kind, cont::ConstSpan<u8> source, const cont::Array<net_test::Bytes>& samples, u32 most)
{
    crd::memory::IAllocator& heap = *crd::memory::default_allocator();
    const BodyModel body(heap, kind, source, most);
    BodyCoder coder(heap, most);
    REQUIRE(body.ready());
    REQUIRE(coder.ready());
    u64 sent = 0U;
    for (const net_test::Bytes& sample : samples)
    {
        const usize coded = coder.encode(body, 65535U, cont::ConstSpan<u8>(sample.data(), sample.size()),
                                         coder.scratch());
        sent += coded != 0U ? coded : sample.size();
    }
    return sent;
}

u64 largest(const cont::Array<net_test::Bytes>& samples)
{
    usize most = 0U;
    for (const net_test::Bytes& sample : samples)
    {
        most = sample.size() > most ? sample.size() : most;
    }
    return most;
}
} // namespace

TEST_CASE("model verb: the state channel's entropy model is the one coding_test.json pins", "[ceridc][model]")
{
    const cont::Array<net_test::Bytes> deltas = net_test::state_delta_samples();
    REQUIRE(deltas.size() == 200U);
    write_samples("deltas", deltas);
    const cont::String pinned = hex(net_test::coding::kChannels[net_test::kCodedState].codecs[0].model);

    ModelFlags all = train("entropy", "deltas", "state.model");
    all.holdout = "0";
    const cont::String out = model(all);
    INFO(out.c_str());
    CHECK(has(out, R"("verb":"model","ok":true,"action":"train","kind":"entropy","file":"state.model","bytes":2048)"));
    CHECK(has(out, joined(R"("hash":")", pinned, R"(")")));
    CHECK_FALSE(has(out, "held_out"));
    const net_test::Bytes expected = net_test::state_entropy_source();
    const cont::Array<u8> written = read_bytes("state.model");
    CHECK(same(written, expected));
    CHECK_FALSE(fs::exists(fs::Path("state.model.partial")));

    // Measured, every delta sends what crd-net's coder sends, and the entry is the schema's.
    const u64 most = largest(deltas);
    const u64 sent =
        sent_by_coder(CodecKind::Entropy, cont::ConstSpan<u8>(written.data(), written.size()), deltas,
                      static_cast<u32>(most));
    const cont::String measured = model(measure("entropy", "deltas", "state.model"));
    INFO(measured.c_str());
    CHECK(has(measured, joined(R"("max_message_bytes":)", decimal(most), R"(,"samples":{"messages":200,)")));
    CHECK(has(measured, joined(R"("sent_bytes":)", decimal(sent), R"(,"coded":200,"stored":0,)")));
    CHECK(has(measured, joined(R"("pays":true,"entry":{"kind":"entropy","hash":")", pinned, R"(","max_ratio":)")));
    // max_ratio is the least power of two above the widest ratio a sample coded at.
    const u64 widest = number_after(measured, "widest_ratio");
    const u64 ratio = number_after(measured, "max_ratio");
    CHECK(widest >= 1U);
    CHECK(ratio > widest);
    CHECK(ratio / 2U <= widest);
    CHECK((ratio & (ratio - 1U)) == 0U);

    // Every fifth delta held out: trained on the other 160, and the held-out 40 still pay.
    const cont::String held = model(train("entropy", "deltas", "held.model"));
    INFO(held.c_str());
    CHECK(has(held, R"("trained":{"messages":160,)"));
    CHECK(has(held, R"("held_out":{"messages":40,)"));
    CHECK(has(held, R"("pays":true)"));
    CHECK_FALSE(has(held, joined(R"("hash":")", pinned, R"(")")));
}

TEST_CASE("model verb: a dictionary trained on notes, and the raw dictionary the schema pins", "[ceridc][model]")
{
    const cont::Array<net_test::Bytes> messages = notes(400U);
    write_samples("notes", messages);

    ModelFlags flags = train("dictionary", "notes", "a.dict");
    flags.dictionary_bytes = "1024";
    const cont::String first = model(flags);
    INFO(first.c_str());
    CHECK(has(first, R"("ok":true,"action":"train","kind":"dictionary","file":"a.dict")"));
    CHECK(has(first, R"("trained":{"messages":320,)"));
    CHECK(has(first, R"("held_out":{"messages":80,)"));
    CHECK(has(first, R"("pays":true,"entry":{"kind":"dictionary",)"));
    CHECK_FALSE(has(first, "small-corpus"));
    const cont::Array<u8> a = read_bytes("a.dict");
    CHECK(a.size() <= 1024U);
    const BodyModel loaded(*crd::memory::default_allocator(), CodecKind::Dictionary,
                           cont::ConstSpan<u8>(a.data(), a.size()), static_cast<u32>(largest(messages)));
    CHECK(loaded.ready());
    CHECK(has(first, joined(R"("hash":")", hex(loaded.hash()), R"(")")));

    // Training is deterministic: the same samples give the same bytes.
    flags.out = "b.dict";
    const cont::String second = model(flags);
    INFO(second.c_str());
    REQUIRE(has(second, R"("ok":true)"));
    const cont::Array<u8> b = read_bytes("b.dict");
    CHECK(same(a, b));

    // A room larger than a tenth of the samples is trained, and reported.
    flags.out = "c.dict";
    flags.dictionary_bytes = "4096";
    const cont::String roomy = model(flags);
    INFO(roomy.c_str());
    CHECK(has(roomy, R"("max_message_bytes":)"));
    CHECK(has(roomy, R"("warning":"small-corpus","trained":)"));

    // A raw dictionary, the content both ends hold, is measured as it is.
    const net_test::Bytes raw = net_test::note_dictionary();
    write_bytes("raw.dict", cont::ConstSpan<u8>(raw.data(), raw.size()));
    ModelFlags raw_flags = measure("dictionary", "notes", "raw.dict");
    raw_flags.max_message_bytes = "20000";
    const cont::String measured = model(raw_flags);
    INFO(measured.c_str());
    const cont::String pinned = hex(net_test::coding::kChannels[net_test::kCodedEvent].codecs[0].model);
    CHECK(has(measured, joined(R"("hash":")", pinned, R"(","max_message_bytes":20000,"samples":{"messages":400,)")));
    const u64 sent = sent_by_coder(CodecKind::Dictionary, cont::ConstSpan<u8>(raw.data(), raw.size()), messages, 20000U);
    CHECK(has(measured, joined(R"("sent_bytes":)", decimal(sent), ",")));
}

TEST_CASE("model verb: which files are held out, and messages coding does not pay for", "[ceridc][model]")
{
    // Every second file by name is held out: the first is trained on, the second measured apart.
    REQUIRE(fs::create_directories(fs::Path("pair")));
    write_text("pair/b", "bbb");
    write_text("pair/a", "a");
    ModelFlags pair = train("entropy", "pair", "pair.model");
    pair.holdout = "2";
    const cont::String split = model(pair);
    INFO(split.c_str());
    CHECK(has(split, R"("max_message_bytes":3,"trained":{"messages":1,"plain_bytes":1,)"));
    CHECK(has(split, R"("held_out":{"messages":1,"plain_bytes":3,)"));
    // The channel's most is a bound a sample may reach, not pass.
    pair.max_message_bytes = "3";
    CHECK(has(model(pair), R"("ok":true)"));
    pair.max_message_bytes = "2";
    const cont::String over = model(pair);
    INFO(over.c_str());
    CHECK(has(over, R"("reason":"sample-too-large","file":"pair/b")"));

    // Random bytes under a model of English: every message is stored, nothing pays and no entry is offered.
    write_samples("notes", notes(100U));
    REQUIRE(has(model(train("entropy", "notes", "english.model")), R"("ok":true)"));
    std::mt19937 rng(7U);
    cont::Array<net_test::Bytes> noise;
    for (u32 index = 0U; index < 20U; ++index)
    {
        net_test::Bytes bytes;
        bytes.resize(64U);
        for (u8& byte : bytes)
        {
            byte = static_cast<u8>(rng());
        }
        noise.push_back(bytes);
    }
    write_samples("noise", noise);
    const cont::String measured = model(measure("entropy", "noise", "english.model"));
    INFO(measured.c_str());
    CHECK(has(measured, R"("samples":{"messages":20,"plain_bytes":1280,"sent_bytes":1280,"coded":0,"stored":20,)"));
    CHECK(has(measured, R"("sent_permille":1000,"widest_ratio":0},"pays":false})"));

    // Whether a trained model pays is judged on what it never saw: text trained on, noise held out.
    const cont::Array<net_test::Bytes> text = notes(10U);
    REQUIRE(fs::create_directories(fs::Path("mixed")));
    for (usize index = 0U; index < 10U; ++index)
    {
        char name[32];
        (void)std::snprintf(name, sizeof(name), "mixed/%02zua", index);
        write_bytes(name, cont::ConstSpan<u8>(text[index].data(), text[index].size()));
        (void)std::snprintf(name, sizeof(name), "mixed/%02zub", index);
        write_bytes(name, cont::ConstSpan<u8>(noise[index].data(), noise[index].size()));
    }
    ModelFlags mixed = train("entropy", "mixed", "mixed.model");
    mixed.holdout = "2";
    const cont::String judged = model(mixed);
    INFO(judged.c_str());
    CHECK(has(judged, R"("trained":{"messages":10,)"));
    CHECK(has(judged, R"("coded":0,"stored":10,"sent_permille":1000,"widest_ratio":0},"pays":false,"entry")"));
}

TEST_CASE("model verb: every malformed request or input is refused and writes nothing", "[ceridc][model]")
{
    write_samples("notes", notes(40U));
    REQUIRE(fs::create_directories(fs::Path("empty")));
    REQUIRE(fs::create_directories(fs::Path("nested/inner")));
    write_text("nested/a", "a message");
    REQUIRE(fs::create_directories(fs::Path("blank")));
    write_text("blank/a", "");
    REQUIRE(fs::create_directories(fs::Path("tiny")));
    write_text("tiny/a", "a");
    write_text("tiny/b", "b");
    write_text("tiny/c", "c");
    write_text("junk.model", "no model");

    struct Case
    {
        const char* name;
        ModelFlags flags;
        const char* reason;
    };
    ModelFlags unknown_action = train("entropy", "notes", "x.model");
    unknown_action.action = "fit";
    ModelFlags no_action = unknown_action;
    no_action.action = nullptr;
    ModelFlags no_out = train("entropy", "notes", nullptr);
    ModelFlags train_with_model = train("entropy", "notes", "x.model");
    train_with_model.model = "junk.model";
    ModelFlags measure_with_out = measure("entropy", "notes", "junk.model");
    measure_with_out.out = "x.model";
    ModelFlags entropy_room = train("entropy", "notes", "x.model");
    entropy_room.dictionary_bytes = "4096";
    ModelFlags small_room = train("dictionary", "notes", "x.model");
    small_room.dictionary_bytes = "255";
    ModelFlags large_room = train("dictionary", "notes", "x.model");
    large_room.dictionary_bytes = "1048577";
    ModelFlags measure_holdout = measure("entropy", "notes", "junk.model");
    measure_holdout.holdout = "5";
    ModelFlags holdout_one = train("entropy", "notes", "x.model");
    holdout_one.holdout = "1";
    ModelFlags holdout_text = train("entropy", "notes", "x.model");
    holdout_text.holdout = "-2";
    ModelFlags no_bytes = train("entropy", "notes", "x.model");
    no_bytes.max_message_bytes = "0";
    ModelFlags too_many_bytes = train("entropy", "notes", "x.model");
    too_many_bytes.max_message_bytes = "67108865";
    ModelFlags over = train("entropy", "notes", "x.model");
    over.max_message_bytes = "8";
    ModelFlags unknown_kind = train("huffman", "notes", "x.model");
    const Case cases[] = {
        {"unknown action", unknown_action, "unknown-action"},
        {"no action", no_action, "unknown-action"},
        {"unknown kind", unknown_kind, "unknown-kind"},
        {"train without --out", no_out, "train needs --samples and --out"},
        {"train given --model", train_with_model, "train needs --samples and --out"},
        {"measure given --out", measure_with_out, "measure needs --samples and --model"},
        {"a room for an entropy model", entropy_room, "--dictionary-bytes is a trained dictionary's"},
        {"a room below zstd's least", small_room, "--dictionary-bytes is from 256 to 1048576"},
        {"a room past the most", large_room, "--dictionary-bytes is from 256 to 1048576"},
        {"a measure's holdout", measure_holdout, "--holdout is a train's"},
        {"every sample held out", holdout_one, "--holdout is 0 or from 2"},
        {"a holdout with a sign", holdout_text, "--holdout is 0 or from 2"},
        {"no message bytes", no_bytes, "--max-message-bytes is from 1 to 67108864"},
        {"past the message bound", too_many_bytes, "--max-message-bytes is from 1 to 67108864"},
        {"no directory", train("entropy", "missing", "x.model"), "samples-not-a-directory"},
        {"a file for a directory", train("entropy", "junk.model", "x.model"), "samples-not-a-directory"},
        {"no samples", train("entropy", "empty", "x.model"), "no-samples"},
        {"a directory among samples", train("entropy", "nested", "x.model"), "sample-not-a-file"},
        {"an empty sample", train("entropy", "blank", "x.model"), "empty-sample"},
        {"a sample past the channel's most", over, "sample-too-large"},
        {"too little for zstd", train("dictionary", "tiny", "x.model"), "training-failed"},
        {"an output with no directory", train("entropy", "notes", "absent/x.model"), "unwritable-output"},
        {"no model to measure", measure("entropy", "notes", "absent.model"), "unreadable-model"},
        {"a directory to measure", measure("entropy", "notes", "empty"), "unreadable-model"},
        {"no entropy model", measure("entropy", "notes", "junk.model"), "not-a-model"},
    };
    for (const Case& refused : cases)
    {
        INFO(refused.name);
        const cont::String out = model(refused.flags);
        INFO(out.c_str());
        CHECK(has(out, R"("verb":"model","ok":false,)"));
        CHECK(has(out, joined(R"("reason":")", refused.reason, R"(")")));
        CHECK_FALSE(fs::exists(fs::Path("x.model")));
        CHECK_FALSE(fs::exists(fs::Path("x.model.partial")));
    }
    // A refusal names the file it is about.
    CHECK(has(model(train("entropy", "blank", "x.model")), R"("file":"blank/a")"));
    CHECK(has(model(over), R"("file":"notes/0000")"));
}

TEST_CASE("model verb: the real ceridc binary answers the same bytes", "[ceridc][model]")
{
    write_samples("notes", notes(60U));
    cont::String exe;
    REQUIRE(crd::read_environment_text("CRD_CERIDC_EXE", exe)); // wired by CMake
    char cmd[1024];
    (void)std::snprintf(cmd, sizeof(cmd),
                        "%s\"%s\" model train --kind dictionary --samples notes --out n.dict --dictionary-bytes 512 "
                        "--holdout 4 > ceridc_model_out.json%s",
                        kShellQuote, exe.c_str(), kShellQuote);
    REQUIRE(std::system(cmd) == 0);
    cont::String out;
    REQUIRE(fs::read_file_text(fs::Path("ceridc_model_out.json"), out));
    while (!out.empty() && (out.data()[out.size() - 1U] == '\n' || out.data()[out.size() - 1U] == '\r'))
    {
        out.pop_back();
    }
    const cont::Array<u8> by_binary = read_bytes("n.dict");
    ModelFlags flags = train("dictionary", "notes", "n.dict");
    flags.dictionary_bytes = "512";
    flags.holdout = "4";
    const cont::String expected = model(flags);
    CHECK(cont::StringView(out) == cont::StringView(expected));
    const cont::Array<u8> by_verb = read_bytes("n.dict");
    CHECK(same(by_binary, by_verb));

    (void)std::snprintf(cmd, sizeof(cmd), R"(%s"%s" model train --kind entropy --samples notes > ceridc_model_out.json%s)",
                        kShellQuote, exe.c_str(), kShellQuote);
    CHECK(std::system(cmd) != 0);
}
