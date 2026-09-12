// test_xml.cpp — GEO-5 (D-007): the XML parser gate — a 3MF-shaped document (the parser's first consumer), entity
// decoding incl. numeric references, the accessor discipline, and the failure classes (no partial DOM survives any).

#include <crd/assetio/xml.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
[[nodiscard]] crd::containers::ConstSpan<crd::u8> sv(const char* s)
{
    return {reinterpret_cast<const crd::u8*>(s), std::strlen(s)};
}
} // namespace

TEST_CASE("xml: a 3MF-shaped document parses -- elements, attributes, text, entities, self-closing", "[assetio][xml]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::assetio::XmlDoc       doc(&alloc);

    const char* src = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- a trimmed 3MF core model -->
<model unit="millimeter" xml:lang="en-US">
  <resources>
    <object id="1" type="model" name="Bracket &amp; Pin &#x2764;">
      <mesh>
        <vertices>
          <vertex x="0" y="0" z="0"/>
          <vertex x="10.5" y="0" z="0"/>
          <vertex x="0" y="10.5" z="0"/>
        </vertices>
        <triangles>
          <triangle v1="0" v2="1" v3="2"/>
        </triangles>
      </mesh>
    </object>
  </resources>
  <build>
    <item objectid="1"/>
  </build>
  <metadata name="Title">A &lt;test&gt; part</metadata>
</model>)";

    REQUIRE(doc.parse(sv(src)) == crd::assetio::XmlError::Ok);
    const crd::i32 model = doc.root();
    REQUIRE(model != crd::assetio::kXmlInvalid);
    CHECK(std::strcmp(doc.name(model), "model") == 0);
    REQUIRE(doc.attr(model, "unit") != nullptr);
    CHECK(std::strcmp(doc.attr(model, "unit"), "millimeter") == 0);
    CHECK(std::strcmp(doc.attr(model, "xml:lang"), "en-US") == 0); // the prefix kept verbatim

    const crd::i32 object = doc.child(doc.child(model, "resources"), "object");
    REQUIRE(object != crd::assetio::kXmlInvalid);
    CHECK(std::strcmp(doc.attr(object, "id"), "1") == 0);
    CHECK(std::strcmp(doc.attr(object, "name"), "Bracket & Pin \xE2\x9D\xA4") == 0); // entity + numeric ref decoded

    // walk the vertices: 3 self-closing elements with attributes
    const crd::i32 verts = doc.child(doc.child(object, "mesh"), "vertices");
    REQUIRE(verts != crd::assetio::kXmlInvalid);
    int      count = 0;
    crd::i32 v     = doc.child(verts, "vertex");
    while (v != crd::assetio::kXmlInvalid)
    {
        ++count;
        if (count == 2) { CHECK(std::strcmp(doc.attr(v, "x"), "10.5") == 0); }
        v = doc.sibling(v, "vertex");
    }
    CHECK(count == 3);

    const crd::i32 tri = doc.child(doc.child(doc.child(object, "mesh"), "triangles"), "triangle");
    REQUIRE(tri != crd::assetio::kXmlInvalid);
    CHECK(std::strcmp(doc.attr(tri, "v3"), "2") == 0);
    CHECK(doc.attr(tri, "missing") == nullptr);

    // the metadata TEXT child, entities decoded
    const crd::i32 meta = doc.child(model, "metadata");
    REQUIRE(meta != crd::assetio::kXmlInvalid);
    const crd::i32 txt = doc.first_child(meta);
    REQUIRE(doc.is_text(txt));
    CHECK(std::strcmp(doc.text(txt), "A <test> part") == 0);

    // kXmlInvalid-safe chaining bottoms out cleanly
    CHECK(doc.child(doc.child(model, "nope"), "deeper") == crd::assetio::kXmlInvalid);
    CHECK(doc.attr(crd::assetio::kXmlInvalid, "x") == nullptr);
}

TEST_CASE("xml: failure classes -- no partial DOM survives", "[assetio][xml]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::assetio::XmlDoc       doc(&alloc);
    using crd::assetio::XmlError;

    CHECK(doc.parse(sv("<a><b></a>")) == XmlError::Malformed);            // mismatched close
    CHECK(doc.root() == crd::assetio::kXmlInvalid);                       // and the DOM is EMPTY
    CHECK(doc.parse(sv("<a><b>")) == XmlError::Truncated);                // unclosed
    CHECK(doc.parse(sv("<a attr=oops/>")) == XmlError::Malformed);        // unquoted attribute
    CHECK(doc.parse(sv("<a>&bogus;</a>")) == XmlError::Malformed);        // unknown entity
    CHECK(doc.parse(sv("<a>&#xD800;</a>")) == XmlError::Malformed);       // a lone surrogate is not a character
    CHECK(doc.parse(sv("text<a/>")) == XmlError::Malformed);              // character data outside the root
    CHECK(doc.parse(sv("<a/><b/>")) == XmlError::Malformed);              // two roots
    CHECK(doc.parse(sv("<!-- unterminated")) == XmlError::Truncated);
    CHECK(doc.parse(sv("")) == XmlError::Malformed);                      // no root at all

    // the depth bomb fails cleanly at the cap
    crd::containers::String bomb(&alloc);
    for (int i = 0; i < 100; ++i) { bomb.append("<a>"); }
    CHECK(doc.parse(sv(bomb.c_str())) == XmlError::TooDeep);

    // and a valid parse AFTER failures works (no poisoned state)
    CHECK(doc.parse(sv("<ok v='1'/>")) == XmlError::Ok);
    CHECK(std::strcmp(doc.attr(doc.root(), "v"), "1") == 0); // single-quoted attributes too
}
