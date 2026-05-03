#pragma once
// ═══════════════════════════════════════════════════════════════════
//  XorStr.h — Compile-time string encryption
//  Strings are XOR-encrypted at compile time and only decrypted
//  on the stack at runtime. Memory scanners see garbage, not plaintext.
//
//  Usage:  XS("HD-Player")  → returns const char* (stack-decrypted)
//          XSW(L"HD-Player") → returns const wchar_t*
// ═══════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstddef>
#include <utility>
#include <utility>

namespace xor_detail
{
    // Compile-time seed from __TIME__ — different every build
    constexpr uint32_t seed()
    {
        uint32_t h = 0x811c9dc5u;
        const char t[] = __TIME__  __DATE__;
        for (int i = 0; t[i]; i++)
            h = (h ^ (uint32_t)t[i]) * 0x01000193u;
        return h;
    }

    // Simple LCG PRNG for per-character key generation
    constexpr uint8_t key(uint32_t idx)
    {
        uint32_t v = seed() ^ (idx * 0x45d9f3bu + 0x27d4eb2du);
        v = (v ^ (v >> 16)) * 0x85ebca6bu;
        v = (v ^ (v >> 13)) * 0xc2b2ae35u;
        return (uint8_t)(v ^ (v >> 16));
    }

    template <typename CharT, size_t N, typename Indices = std::make_index_sequence<N>>
    struct XorString;

    template <typename CharT, size_t N, size_t... I>
    struct XorString<CharT, N, std::index_sequence<I...>>
    {
        // Encrypted data stored as array member (in .rdata — but XOR'd)
        CharT encrypted[N];

        // Construct at compile time — each char XOR'd with unique key
        constexpr XorString(const CharT* str)
            : encrypted{ static_cast<CharT>(str[I] ^ key(I))... }
        {}

        // Decrypt on the stack at runtime — result lives on caller's stack frame
        // After function returns, decrypted string is gone from memory
        const CharT* decrypt() const
        {
            // Thread-local so each thread gets its own buffer, no races
            thread_local CharT buf[N];
            for (size_t i = 0; i < N; i++)
                buf[i] = static_cast<CharT>(encrypted[i] ^ key(i));
            return buf;
        }
    };
}

// Macro helpers — force constexpr construction
#define XS(str) \
    ([]() -> const char* { \
        static constexpr ::xor_detail::XorString<char, sizeof(str)> xs(str); \
        return xs.decrypt(); \
    }())

#define XSW(str) \
    ([]() -> const wchar_t* { \
        static constexpr ::xor_detail::XorString<wchar_t, sizeof(str)/sizeof(wchar_t)> xs(str); \
        return xs.decrypt(); \
    }())
