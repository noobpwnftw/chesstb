#pragma once

#include "defines.h"

#include <cstdint>
#include <utility>

#if defined(__BMI2__)
  #include <immintrin.h>
  #define CHESSTB_HAVE_PEXT 1
#else
  #define CHESSTB_HAVE_PEXT 0
#endif

NODISCARD INLINE size_t lsb(uint64_t b)
{
#ifndef _MSC_VER
	return (__builtin_ctzll(b));
#else
	unsigned long ret;
	_BitScanForward64(&ret, b);
	return (int)ret;
#endif
}

NODISCARD INLINE size_t msb(uint64_t b)
{
#ifndef _MSC_VER
	return (63 ^ __builtin_clzll(b));
#else
	unsigned long ret;
	_BitScanReverse64(&ret, b);
	return (int)ret;
#endif
}

NODISCARD INLINE size_t pop_first_bit(uint64_t& b)
{
	const size_t r = lsb(b);
	b &= b - 1;
	return r;
}

NODISCARD INLINE size_t popcnt(uint64_t b)
{
#ifndef _MSC_VER
	return __builtin_popcountll(b);
#else
	return (int)__popcnt64(b);
#endif
}

// mask's set bits, lowest first, taking b's low bits in turn (BMI2 pdep).
NODISCARD INLINE uint64_t pdep_u64(uint64_t b, uint64_t mask)
{
#if CHESSTB_HAVE_PEXT
	return _pdep_u64(b, mask);
#else
	uint64_t r = 0;
	for (uint64_t bit = 1; mask; bit += bit, mask &= mask - 1)
		if (b & bit) r |= mask & (0 - mask);
	return r;
#endif
}

NODISCARD uint64_t shiftleft128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift);
NODISCARD uint64_t shiftright128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift);

NODISCARD uint64_t mulhi_epu64(uint64_t lhs, uint64_t rhs);
NODISCARD std::pair<uint64_t, uint64_t> udiv128(uint64_t lhs_high, uint64_t lhs_low, uint64_t rhs);
