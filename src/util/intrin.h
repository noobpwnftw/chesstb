#pragma once

#include "defines.h"

#include <utility>

void atomic_fetch_or(uint8_t* p, uint8_t v);
void atomic_fetch_or(uint16_t* p, uint16_t v);
void atomic_fetch_or(uint32_t* p, uint32_t v);
void atomic_fetch_or(uint64_t* p, uint64_t v);

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

NODISCARD uint64_t shiftleft128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift);
NODISCARD uint64_t shiftright128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift);

NODISCARD uint64_t mulhi_epu64(uint64_t lhs, uint64_t rhs);
NODISCARD std::pair<uint64_t, uint64_t> udiv128(uint64_t lhs_high, uint64_t lhs_low, uint64_t rhs);
