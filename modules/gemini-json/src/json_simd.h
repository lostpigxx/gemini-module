#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define GEMINI_JSON_SIMD_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define GEMINI_JSON_SIMD_SSE2 1
#endif

namespace json_simd {

// Scan [p, end) for the first byte that is '"', '\\', < 0x20, or >= 0x80.
// Used by the JSON parser's unescaped string fast path.
inline const char* FindNextSpecialInString(const char* p, const char* end) {
#if defined(GEMINI_JSON_SIMD_NEON)
  const uint8x16_t v_quote = vdupq_n_u8('"');
  const uint8x16_t v_bs = vdupq_n_u8('\\');
  const uint8x16_t v_ctl = vdupq_n_u8(0x20);
  const uint8x16_t v_hi = vdupq_n_u8(0x80);

  while (p + 16 <= end) {
    uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    uint8x16_t any = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_quote), vceqq_u8(v, v_bs)),
        vorrq_u8(vcltq_u8(v, v_ctl), vcgeq_u8(v, v_hi)));
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(any), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    if (mask != 0) {
      return p + (static_cast<size_t>(__builtin_ctzll(mask)) >> 2);
    }
    p += 16;
  }
#elif defined(GEMINI_JSON_SIMD_SSE2)
  const __m128i v_quote = _mm_set1_epi8('"');
  const __m128i v_bs = _mm_set1_epi8('\\');
  const __m128i v_ctl = _mm_set1_epi8(0x20);

  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    // Signed < 0x20 catches BOTH [0x00..0x1F] and [0x80..0xFF] (negative signed bytes)
    __m128i any = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, v_quote), _mm_cmpeq_epi8(v, v_bs)),
        _mm_cmplt_epi8(v, v_ctl));
    int mask = _mm_movemask_epi8(any);
    if (mask != 0) {
      return p + static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
    }
    p += 16;
  }
#else
  // 8-byte SWAR fallback
  constexpr uint64_t kOnes = 0x0101010101010101ULL;
  constexpr uint64_t kHigh = 0x8080808080808080ULL;
  constexpr uint64_t kQuote = kOnes * static_cast<uint8_t>('"');
  constexpr uint64_t kBs = kOnes * static_cast<uint8_t>('\\');
  constexpr uint64_t kCtl = kOnes * 0x20ULL;

  while (p + 8 <= end) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    uint64_t has_hi = w & kHigh;
    uint64_t has_ctl = (w - kCtl) & ~w & kHigh;
    uint64_t xq = w ^ kQuote;
    uint64_t has_q = (xq - kOnes) & ~xq & kHigh;
    uint64_t xb = w ^ kBs;
    uint64_t has_b = (xb - kOnes) & ~xb & kHigh;
    uint64_t any = has_hi | has_ctl | has_q | has_b;
    if (any != 0) {
      return p + (static_cast<size_t>(__builtin_ctzll(any)) >> 3);
    }
    p += 8;
  }
#endif

  while (p < end) {
    auto c = static_cast<uint8_t>(*p);
    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) return p;
    p++;
  }
  return end;
}

// Scan [p, end) for the first byte that needs JSON escaping: '"', '\\', or < 0x20.
// Valid UTF-8 bytes >= 0x80 are passed through without stopping.
inline const char* FindNextEscapeInSerialize(const char* p, const char* end) {
#if defined(GEMINI_JSON_SIMD_NEON)
  const uint8x16_t v_quote = vdupq_n_u8('"');
  const uint8x16_t v_bs = vdupq_n_u8('\\');
  const uint8x16_t v_ctl = vdupq_n_u8(0x20);

  while (p + 16 <= end) {
    uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    uint8x16_t any = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_quote), vceqq_u8(v, v_bs)),
        vcltq_u8(v, v_ctl));
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(any), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    if (mask != 0) {
      return p + (static_cast<size_t>(__builtin_ctzll(mask)) >> 2);
    }
    p += 16;
  }
#elif defined(GEMINI_JSON_SIMD_SSE2)
  const __m128i v_quote = _mm_set1_epi8('"');
  const __m128i v_bs = _mm_set1_epi8('\\');
  const __m128i v_mask_e0 = _mm_set1_epi8(static_cast<char>(0xE0));
  const __m128i v_zero = _mm_setzero_si128();

  while (p + 16 <= end) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i is_ctl = _mm_cmpeq_epi8(_mm_and_si128(v, v_mask_e0), v_zero);
    __m128i any = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, v_quote), _mm_cmpeq_epi8(v, v_bs)),
        is_ctl);
    int mask = _mm_movemask_epi8(any);
    if (mask != 0) {
      return p + static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
    }
    p += 16;
  }
#else
  constexpr uint64_t kOnes = 0x0101010101010101ULL;
  constexpr uint64_t kHigh = 0x8080808080808080ULL;
  constexpr uint64_t kQuote = kOnes * static_cast<uint8_t>('"');
  constexpr uint64_t kBs = kOnes * static_cast<uint8_t>('\\');

  while (p + 8 <= end) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    // (b & 0xE0) == 0 iff b < 0x20
    uint64_t masked = w & 0xE0E0E0E0E0E0E0E0ULL;
    uint64_t has_ctl = (masked - kOnes) & ~masked & kHigh;
    uint64_t xq = w ^ kQuote;
    uint64_t has_q = (xq - kOnes) & ~xq & kHigh;
    uint64_t xb = w ^ kBs;
    uint64_t has_b = (xb - kOnes) & ~xb & kHigh;
    uint64_t any = has_ctl | has_q | has_b;
    if (any != 0) {
      return p + (static_cast<size_t>(__builtin_ctzll(any)) >> 3);
    }
    p += 8;
  }
#endif

  while (p < end) {
    auto c = static_cast<uint8_t>(*p);
    if (c == '"' || c == '\\' || c < 0x20) return p;
    p++;
  }
  return end;
}

}  // namespace json_simd
