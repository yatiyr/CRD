// crd-net -- what makes an HTTP message well formed (4.2a; docs/design/network-foundation.md#http3): every rule of RFC
// 9110, 9113, 9114, 8441 and 9220 the checks hold, one refusal at a time from a good request or response.

#include "http3_fixture.hpp"

#include <crd/net/http_message.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;

bool request(const H3Fields& fields)
{
    HttpMessageInfo info;
    return http_check_request(fields.span(), info);
}

bool response(const H3Fields& fields)
{
    HttpMessageInfo info;
    return http_check_response(fields.span(), info);
}

H3Fields base()
{
    return H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a.test"}, {":path", "/"}};
}
} // namespace

TEST_CASE("http message: a well-formed request reports what it carries", "[http3]")
{
    H3Fields fields = base();
    fields.add("content-length", "12");
    fields.add("te", "trailers");
    HttpMessageInfo info;
    REQUIRE(http_check_request(fields.span(), info));
    CHECK(info.has_content_length);
    CHECK(info.content_length == 12U);
    CHECK_FALSE(info.connect);
    CHECK_FALSE(info.extended);
    CHECK_FALSE(info.head);
    CHECK(info.method.size() == 3U);
    CHECK(info.path.size() == 1U);
}

TEST_CASE("http message: a field name is a lower-case token", "[http3]")
{
    const char* const bad[] = {"Upper", "a b", "a:b", "a\"b", "a(b", "a/b", "a\x7f", "a\x80"};
    for (const char* name : bad)
    {
        H3Fields fields = base();
        fields.add(name, "x");
        CHECK_FALSE(request(fields));
    }
    H3Fields empty = base();
    empty.add("", "x");
    CHECK_FALSE(request(empty));
    H3Fields good = base();
    good.add("x-a.b_c!#$%&'*+-^`|~0", "x");
    CHECK(request(good));
}

TEST_CASE("http message: a field value hides no line break, NUL or edge space", "[http3]")
{
    const char* const bad[] = {"a\rb", "a\nb", " a", "a ", "\ta", "a\t"};
    for (const char* value : bad)
    {
        H3Fields fields = base();
        fields.add("x", value);
        CHECK_FALSE(request(fields));
    }
    H3Fields nul = base();
    nul.add("x", "ab");
    static constexpr u8 kNul[] = {'a', 0U, 'b'};
    nul.items[nul.count - 1U].value = containers::ConstSpan<u8>(kNul, 3U);
    CHECK_FALSE(request(nul));
    H3Fields good = base();
    good.add("x", "a b\t\xC3\xA9");
    good.add("empty", "");
    CHECK(request(good));
}

TEST_CASE("http message: connection-specific fields are refused and te is only trailers", "[http3]")
{
    const char* const banned[] = {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"};
    for (const char* name : banned)
    {
        H3Fields fields = base();
        fields.add(name, "x");
        CHECK_FALSE(request(fields));
    }
    H3Fields te = base();
    te.add("te", "gzip");
    CHECK_FALSE(request(te));
}

TEST_CASE("http message: content-length is one decimal number", "[http3]")
{
    const char* const bad[] = {"", "x", "1x", "-1", "+1", "1,1", "1 1", "4611686018427387904"};
    for (const char* value : bad)
    {
        H3Fields fields = base();
        fields.add("content-length", value);
        CHECK_FALSE(request(fields));
    }
    H3Fields different = base();
    different.add("content-length", "1");
    different.add("content-length", "2");
    CHECK_FALSE(request(different));
    H3Fields same = base();
    same.add("content-length", "2");
    same.add("content-length", "2");
    CHECK(request(same));
    H3Fields largest = base();
    largest.add("content-length", "4611686018427387903");
    CHECK(request(largest));
}

TEST_CASE("http message: pseudo-fields come first, once, and known", "[http3]")
{
    H3Fields late{{":method", "GET"}, {":scheme", "https"}, {"x", "y"}, {":authority", "a.test"}, {":path", "/"}};
    CHECK_FALSE(request(late));
    H3Fields twice = base();
    twice.items[1].name = twice.items[0].name;
    CHECK_FALSE(request(twice));
    H3Fields unknown = base();
    unknown.add(":x", "y");
    CHECK_FALSE(request(unknown));
    H3Fields status = base();
    status.add(":status", "200");
    CHECK_FALSE(request(status));
    H3Fields duplicates[] = {
        H3Fields{{":method", "GET"}, {":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}},
        H3Fields{{":method", "GET"}, {":scheme", "https"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}},
        H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":authority", "a"}, {":path", "/"}},
        H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {":path", "/"}},
    };
    for (const H3Fields& fields : duplicates)
    {
        CHECK_FALSE(request(fields));
    }
}

TEST_CASE("http message: a request has its pseudo-fields and a valid target", "[http3]")
{
    CHECK_FALSE(request(H3Fields{{":scheme", "https"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}}));
    CHECK_FALSE(request(H3Fields{{":method", "G T"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "1x"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "h_s"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", ""}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", ""}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "x"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "*"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/a b"}}));
    CHECK(request(H3Fields{{":method", "OPTIONS"}, {":scheme", "https"}, {":authority", "a"}, {":path", "*"}}));
    CHECK(request(H3Fields{{":method", "GET"}, {":scheme", "other+x.y-z"}, {":path", "anything"}}));
}

TEST_CASE("http message: authority and host agree and carry no userinfo", "[http3]")
{
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}}));
    CHECK(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", "a"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", ""}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", ""}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "u@a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a b"}, {":path", "/"}}));
    CHECK(request(
        H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "A.b"}, {":path", "/"}, {"host", "a.B"}}));
    CHECK_FALSE(request(
        H3Fields{{":method", "GET"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}, {"host", "b"}}));
    CHECK_FALSE(
        request(H3Fields{{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", "a"}, {"host", "a"}}));
}

TEST_CASE("http message: CONNECT has :authority alone, and an extended CONNECT all three", "[http3]")
{
    HttpMessageInfo info;
    const H3Fields connect{{":method", "CONNECT"}, {":authority", "a:443"}};
    REQUIRE(http_check_request(connect.span(), info));
    CHECK(info.connect);
    CHECK_FALSE(info.extended);
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}}));
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":authority", ""}}));
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":authority", "a"}, {":scheme", "https"}}));
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":authority", "a"}, {":path", "/"}}));

    const H3Fields extended{{":method", "CONNECT"},
                            {":protocol", "webtransport"},
                            {":scheme", "https"},
                            {":authority", "a"},
                            {":path", "/"}};
    REQUIRE(http_check_request(extended.span(), info));
    CHECK(info.connect);
    CHECK(info.extended);
    CHECK(info.protocol.size() == 12U);
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":protocol", "x"}, {":scheme", "https"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":protocol", "x"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(
        request(H3Fields{{":method", "CONNECT"}, {":protocol", "x"}, {":authority", "a"}, {":scheme", "https"}}));
    CHECK_FALSE(request(H3Fields{
        {":method", "CONNECT"}, {":protocol", "x y"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}}));
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"},
                                 {":protocol", "x"},
                                 {":protocol", "x"},
                                 {":scheme", "https"},
                                 {":authority", "a"},
                                 {":path", "/"}}));
    CHECK_FALSE(request(
        H3Fields{{":method", "GET"}, {":protocol", "x"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}}));
    // An extended CONNECT names its authority whatever its scheme.
    CHECK_FALSE(request(H3Fields{{":method", "CONNECT"}, {":protocol", "x"}, {":scheme", "other"}, {":path", "/"}}));
}

TEST_CASE("http message: HEAD is known to the caller", "[http3]")
{
    HttpMessageInfo info;
    const H3Fields head{{":method", "HEAD"}, {":scheme", "https"}, {":authority", "a"}, {":path", "/"}};
    REQUIRE(http_check_request(head.span(), info));
    CHECK(info.head);
}

TEST_CASE("http message: a response has one three-digit :status and no 101", "[http3]")
{
    HttpMessageInfo info;
    const H3Fields ok{{":status", "204"}, {"content-length", "0"}};
    REQUIRE(http_check_response(ok.span(), info));
    CHECK(info.status == 204U);
    CHECK(info.has_content_length);
    CHECK_FALSE(response(H3Fields{}));
    CHECK_FALSE(response(H3Fields{{"x", "y"}}));
    CHECK_FALSE(response(H3Fields{{":status", "20"}}));
    CHECK_FALSE(response(H3Fields{{":status", "2000"}}));
    CHECK_FALSE(response(H3Fields{{":status", "20x"}}));
    CHECK_FALSE(response(H3Fields{{":status", "x00"}}));
    CHECK_FALSE(response(H3Fields{{":status", "2x0"}}));
    CHECK_FALSE(response(H3Fields{{":status", "099"}}));
    CHECK_FALSE(response(H3Fields{{":status", "101"}}));
    CHECK_FALSE(response(H3Fields{{":status", "200"}, {":status", "200"}}));
    CHECK_FALSE(response(H3Fields{{"x", "y"}, {":status", "200"}}));
    CHECK_FALSE(response(H3Fields{{":status", "200"}, {":method", "GET"}}));
    CHECK_FALSE(response(H3Fields{{":status", "200"}, {"connection", "close"}}));
    CHECK_FALSE(response(H3Fields{{":status", "200"}, {"Bad", "x"}}));
    CHECK(response(H3Fields{{":status", "100"}}));
    CHECK(response(H3Fields{{":status", "999"}}));
}

TEST_CASE("http message: trailers carry no pseudo-fields", "[http3]")
{
    CHECK(http_check_trailers(H3Fields{{"x", "y"}, {"grpc-status", "0"}}.span()));
    CHECK(http_check_trailers(H3Fields{}.span()));
    CHECK_FALSE(http_check_trailers(H3Fields{{":status", "200"}}.span()));
    CHECK_FALSE(http_check_trailers(H3Fields{{"connection", "x"}}.span()));
    CHECK_FALSE(http_check_trailers(H3Fields{{"Bad", "x"}}.span()));
}

TEST_CASE("http message: a section's size counts 32 bytes per field", "[http3]")
{
    CHECK(http_section_size(H3Fields{{"ab", "cd"}, {"e", ""}}.span()) == 4U + 1U + 64U);
    CHECK(http_section_size(H3Fields{}.span()) == 0U);
}
