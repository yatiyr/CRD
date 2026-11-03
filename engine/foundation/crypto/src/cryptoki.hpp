// crd-crypto -- OASIS's PKCS #11 header as Cerid includes it (NET.2 decision K14): the five macros the header asks its
// includer to define, and the 1-byte packing Cryptoki uses on Windows. The PKCS #11 key device and the PIV test module
// (tests/foundation/crypto/piv_module.cpp) include it, so both sides of the interface agree on every structure; a
// module defines CK_DECLARE_FUNCTION itself first, to export its entry point.
#pragma once

// NOLINTBEGIN(cppcoreguidelines-macro-usage, bugprone-macro-parentheses): the header's own interface
#define CK_PTR *
#ifndef CK_DECLARE_FUNCTION
#define CK_DECLARE_FUNCTION(returnType, name) returnType name
#endif
#define CK_DECLARE_FUNCTION_POINTER(returnType, name) returnType(*name)
#define CK_CALLBACK_FUNCTION(returnType, name) returnType(*name)
#ifndef NULL_PTR
#define NULL_PTR nullptr
#endif
// NOLINTEND(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#if defined(_WIN32)
#pragma pack(push, cryptoki, 1)
#endif
#include <pkcs11.h>
#if defined(_WIN32)
#pragma pack(pop, cryptoki)
#endif
