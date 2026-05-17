#pragma once

#include "defines.h"
#include "intrin.h"

#include <stdexcept>
#include <limits>

template <typename T>
struct Divider;

template <>
struct Divider<uint64_t>
{
	static constexpr bool ASSUME_NO_OVERFLOW = true;

	Divider() = default;

	Divider(uint64_t divisor)
	{
		if (divisor <= 1)
			throw std::runtime_error("Divisor must be greater than 1.");

		const size_t divisor_log2 = msb(divisor);

		if ((divisor & (divisor - 1)) == 0) {
			m_magic = 0;
			m_shift = divisor_log2 - 1 + ASSUME_NO_OVERFLOW;
		}
		else
		{
			auto [div, rem] = udiv128(1ull << divisor_log2, 0, divisor);
			m_magic = div * 2 + 1 + (rem + rem >= divisor);
			m_shift = divisor_log2 + ASSUME_NO_OVERFLOW;
		}
	}

	NODISCARD friend inline uint64_t operator/(uint64_t n, const Divider& div)
	{
		const uint64_t q = mulhi_epu64(div.m_magic, n);

		if constexpr (ASSUME_NO_OVERFLOW)
		{
			// We use a faster path that assumes (n + q) does not overflow.
			ASSERT(n < std::numeric_limits<size_t>::max() / 2);
			return (n + q) >> div.m_shift; // + 1 preadded during construction
		}
		else
		{
			return (((n - q) >> 1) + q) >> div.m_shift;
		}
	}

	// Overload of operator /= for scalar division
	friend inline uint64_t& operator/=(uint64_t& n, const Divider& div)
	{
		return n = n / div;
	}

private:
	uint64_t m_magic;
	uint64_t m_shift;
};