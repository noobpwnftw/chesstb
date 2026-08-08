#include "intrin.h"

#include "system/system.h"

#if defined(OS_WINDOWS)
#include <windows.h>
#endif

void atomic_fetch_or(uint8_t* p, uint8_t v)
{
#if defined(OS_WINDOWS)
	InterlockedOr8(reinterpret_cast<volatile char*>(p), v);
#else
	__atomic_fetch_or(p, v, __ATOMIC_RELAXED);
#endif
}

void atomic_fetch_or(uint16_t* p, uint16_t v)
{
#if defined(OS_WINDOWS)
	InterlockedOr16(reinterpret_cast<volatile SHORT*>(p), v);
#else
	__atomic_fetch_or(p, v, __ATOMIC_RELAXED);
#endif
}

void atomic_fetch_or(uint32_t* p, uint32_t v)
{
#if defined(OS_WINDOWS)
	InterlockedOr(reinterpret_cast<volatile LONG*>(p), v);
#else
	__atomic_fetch_or(p, v, __ATOMIC_RELAXED);
#endif
}

void atomic_fetch_or(uint64_t* p, uint64_t v)
{
#if defined(OS_WINDOWS)
	InterlockedOr64(reinterpret_cast<volatile LONG64*>(p), v);
#else
	__atomic_fetch_or(p, v, __ATOMIC_RELAXED);
#endif
}

NODISCARD uint64_t mulhi_epu64(uint64_t lhs, uint64_t rhs)
{
#if defined(OS_WINDOWS)
	return __umulh(lhs, rhs);
#else
	return (lhs * (unsigned __int128)rhs) >> 64;
#endif
}

NODISCARD std::pair<uint64_t, uint64_t> udiv128(uint64_t lhs_high, uint64_t lhs_low, uint64_t rhs)
{
#if defined(_MSC_VER)
	uint64_t rem;
	uint64_t div = _udiv128(lhs_high, lhs_low, rhs, &rem);
	return { div, rem };
#else
	unsigned __int128 lhs = ((unsigned __int128)lhs_high << 64) + lhs_low;
	uint64_t div = lhs / rhs;
	uint64_t rem = lhs % rhs;
	return { div, rem };
#endif
}

#if defined(OS_WINDOWS)

NODISCARD uint64_t shiftleft128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift)
{
	return __shiftleft128(LowPart, HighPart, Shift);
}

NODISCARD uint64_t shiftright128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift)
{
	return __shiftright128(LowPart, HighPart, Shift);
}

#else

NODISCARD uint64_t shiftleft128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift)
{
	unsigned __int128 val = ((unsigned __int128)HighPart << 64) | LowPart;
	unsigned __int128 res = val << (Shift & 63);
	return static_cast<uint64_t>(res >> 64);
}

NODISCARD uint64_t shiftright128(uint64_t LowPart, uint64_t HighPart, uint8_t Shift)
{
	unsigned __int128 val = ((unsigned __int128)HighPart << 64) | LowPart;
	unsigned __int128 res = val >> (Shift & 63);
	return static_cast<uint64_t>(res);
}

#endif