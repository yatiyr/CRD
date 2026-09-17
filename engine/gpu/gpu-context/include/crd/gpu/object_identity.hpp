#pragma once

// DIAG.7a(c): a stable Cerid GPU object identity value type.
//
// The design (design/runtime-diagnostics.md#diag-7a, ADR-0133) requires "stable Cerid resource/program/pass IDs and
// generation" attached to native object names and validation reports, where an ID "cannot silently alias when a ...
// GPU object is reused" -- i.e. the classic index+generation model. This header defines ONLY the value type and its
// name-encoding/parsing contract; WHO allocates identities and bumps generations, and the actual threading of the
// encoded name through `vkSetDebugUtilsObjectNameEXT` / `ID3D12Object::SetName` and into the report, is sub-unit (d).
//
// The (index, generation) layout deliberately mirrors `crd::containers::SlotMap::Handle` (do not invent a second
// handle shape): index 0xFFFFFFFF is the null slot, generation 0 is reserved so a default identity is invalid, and
// live generations start at 1 and bump on recreation so a recycled slot never aliases an older identity.
//
// No vendor types (`D3D12_*` / `Vk*`) appear here. Encoding is ASCII-only, fixed-width hex and locale-independent
// (hand-formatted, never snprintf) so (d) can widen it char-by-char to `LPCWSTR` for DX12 and embed it in the debug
// layers' message prose. `parse` scans for the token INSIDE a longer string, because a debug-layer message quotes the
// object name in prose (e.g. "[crd:res:...] shadowmap 2048x2048x1"): whole-string-only parsing is the wrong contract.

#include <crd/core/types.hpp>

#include <string_view>

namespace crd::gpu
{

enum class ObjectKind : crd::u8
{
    Resource,
    Program,
    Pass,
};

// The kind token used in the encoded name. Fixed, ASCII, no separator characters.
[[nodiscard]] constexpr const char* to_string(ObjectKind k) noexcept
{
    switch (k)
    {
    case ObjectKind::Resource: return "res";
    case ObjectKind::Program:  return "prog";
    case ObjectKind::Pass:     return "pass";
    }
    return "res";
}

// A stable, alias-proof identity for a GPU resource / program / pass. Trivially copyable value; the generation makes
// a recycled index distinguishable from the identity that used to hold it (mirrors SlotMap::Handle).
struct ObjectIdentity
{
    ObjectKind kind       = ObjectKind::Resource;
    crd::u32   index      = 0xFFFFFFFFU; // 0xFFFFFFFF == null slot (matches SlotMap::Handle)
    crd::u32   generation = 0U;          // 0 reserved (default == invalid); live generations start at 1 and bump

    [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFU && generation != 0U; }
    [[nodiscard]] bool operator==(const ObjectIdentity& o) const noexcept
    {
        return kind == o.kind && index == o.index && generation == o.generation;
    }
};

// Encoded form: "crd:" <kind> ":" <8 hex index> ":g" <8 hex generation>, e.g. "crd:res:0000002a:g00000007".
// The widest kind ("prog"/"pass", 4 chars) gives the longest string. Callers size a buffer to kObjectIdentityBufferSize.
inline constexpr crd::usize kObjectIdentityMaxChars   = 27U; // == length of "crd:prog:0000002a:g00000007"
inline constexpr crd::usize kObjectIdentityBufferSize = kObjectIdentityMaxChars + 1U; // + NUL terminator

// Write a NUL-terminated encoding of `id` into out[0, cap). Returns the character count written (excluding the NUL),
// or 0 -- writing NOTHING -- if `id` is invalid or the buffer is too small. An invalid identity is never named: a
// name that round-trips must denote a real, live-or-retired object, so encode REFUSES the null identity (it does not
// emit a distinguished "invalid" string that could be mistaken for one).
[[nodiscard]] inline crd::usize encode(const ObjectIdentity& id, char* out, crd::usize cap) noexcept
{
    if (out == nullptr || !id.valid()) { return 0U; }
    const char* kind = to_string(id.kind);
    crd::usize kind_len = 0U;
    while (kind[kind_len] != '\0') { ++kind_len; }
    const crd::usize need = 4U + kind_len + 1U + 8U + 2U + 8U; // "crd:" + kind + ":" + idx + ":g" + gen (no NUL)
    if (cap < need + 1U) { return 0U; }

    static constexpr char kHex[] = "0123456789abcdef";
    crd::usize p = 0U;
    out[p++] = 'c'; out[p++] = 'r'; out[p++] = 'd'; out[p++] = ':';
    for (crd::usize i = 0U; i < kind_len; ++i) { out[p++] = kind[i]; }
    out[p++] = ':';
    for (int shift = 28; shift >= 0; shift -= 4) { out[p++] = kHex[(id.index >> shift) & 0xFU]; }
    out[p++] = ':'; out[p++] = 'g';
    for (int shift = 28; shift >= 0; shift -= 4) { out[p++] = kHex[(id.generation >> shift) & 0xFU]; }
    out[p] = '\0';
    return p;
}

namespace detail
{
[[nodiscard]] constexpr bool hex_nibble(char c, crd::u32& value) noexcept
{
    if (c >= '0' && c <= '9') { value = static_cast<crd::u32>(c - '0');       return true; }
    if (c >= 'a' && c <= 'f') { value = static_cast<crd::u32>(c - 'a' + 10);  return true; }
    if (c >= 'A' && c <= 'F') { value = static_cast<crd::u32>(c - 'A' + 10);  return true; }
    return false;
}

// Read EXACTLY 8 hex digits at s[i], advancing i past them on success. Fixed width keeps the token boundaries
// unambiguous when it is embedded in a longer string (encode always zero-pads to 8).
[[nodiscard]] constexpr bool read_hex8(std::string_view s, crd::usize& i, crd::u32& out) noexcept
{
    if (i + 8U > s.size()) { return false; }
    crd::u32 acc = 0U;
    for (crd::usize k = 0U; k < 8U; ++k)
    {
        crd::u32 nibble = 0U;
        if (!hex_nibble(s[i + k], nibble)) { return false; }
        acc = (acc << 4) | nibble;
    }
    i += 8U;
    out = acc;
    return true;
}

// Parse a full token whose "crd:" prefix has already been consumed (i points just past it).
[[nodiscard]] inline bool parse_after_prefix(std::string_view s, crd::usize i, ObjectIdentity& out) noexcept
{
    const crd::usize kind_begin = i;
    while (i < s.size() && s[i] != ':') { ++i; }
    if (i >= s.size()) { return false; }
    const std::string_view kind = s.substr(kind_begin, i - kind_begin);
    ObjectKind resolved = ObjectKind::Resource;
    if      (kind == "res")  { resolved = ObjectKind::Resource; }
    else if (kind == "prog") { resolved = ObjectKind::Program; }
    else if (kind == "pass") { resolved = ObjectKind::Pass; }
    else                     { return false; }
    ++i; // consume ':' after the kind

    crd::u32 index = 0U;
    if (!read_hex8(s, i, index)) { return false; }
    if (i + 2U > s.size() || s[i] != ':' || s[i + 1U] != 'g') { return false; }
    i += 2U; // consume ":g"
    crd::u32 generation = 0U;
    if (!read_hex8(s, i, generation)) { return false; }

    out = ObjectIdentity{resolved, index, generation};
    return true;
}
} // namespace detail

// Find and parse the FIRST valid identity token anywhere in `text` (so a debug-layer message that quotes the object
// name in prose still yields the identity). Returns true and fills `out` on success; leaves `out` untouched on
// failure. Only a token whose parsed identity is valid() is accepted.
[[nodiscard]] inline bool parse(std::string_view text, ObjectIdentity& out) noexcept
{
    constexpr std::string_view kPrefix = "crd:";
    crd::usize start = 0U;
    for (;;)
    {
        const crd::usize pos = text.find(kPrefix, start);
        if (pos == std::string_view::npos) { return false; }
        ObjectIdentity candidate;
        if (detail::parse_after_prefix(text, pos + kPrefix.size(), candidate) && candidate.valid())
        {
            out = candidate;
            return true;
        }
        start = pos + 1U; // this "crd:" did not begin a valid token; keep scanning
    }
}

// DIAG.7a(d2a): build the native debug name for an object -- the bracketed-prefix form parse() was built to find:
// "[<encoded identity>] <site label>", e.g. "[crd:res:0000002a:g00000007] shadowmap 2048x2048x1". This keeps the
// human SITE+SIZE label (still useful) AND carries the stable Cerid identity, which the validation callback then
// resolves via parse(). (d2b) hands this to DX12 `SetName` (widened to LPCWSTR) and `vkSetDebugUtilsObjectNameEXT`.
// The widest prefix uses the longest kind; callers size a buffer to kDebugNamePrefixChars + label length + 1 (NUL).
inline constexpr crd::usize kDebugNamePrefixChars = 1U + kObjectIdentityMaxChars + 2U; // "[" + token + "] "

// Write "[<encoded id>] <site_label>" (NUL-terminated) into out[0, cap). Returns the character count (excluding NUL),
// or 0 -- writing NOTHING -- if `id` is invalid or the buffer is too small (same refusal contract as encode()).
[[nodiscard]] inline crd::usize format_debug_name(const ObjectIdentity& id, std::string_view site_label, char* out,
                                                  crd::usize cap) noexcept
{
    if (out == nullptr || !id.valid()) { return 0U; }
    char token[kObjectIdentityBufferSize];
    const crd::usize token_len = encode(id, token, sizeof(token));
    if (token_len == 0U) { return 0U; }
    const crd::usize need = 1U + token_len + 2U + site_label.size(); // "[" + token + "] " + label (excludes NUL)
    if (cap < need + 1U) { return 0U; }

    crd::usize p = 0U;
    out[p++] = '[';
    for (crd::usize i = 0U; i < token_len; ++i) { out[p++] = token[i]; }
    out[p++] = ']';
    out[p++] = ' ';
    for (crd::usize i = 0U; i < site_label.size(); ++i) { out[p++] = site_label[i]; }
    out[p] = '\0';
    return p;
}

} // namespace crd::gpu
