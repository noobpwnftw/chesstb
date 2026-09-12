#pragma once

#include "defines.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

NODISCARD INLINE bool is_little_endian()
{
	std::uint64_t value = 0x0807060504030201ull;
	char little_endian_bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	char bytes[8];
	std::memcpy(bytes, &value, 8);
	return std::memcmp(bytes, little_endian_bytes, 8) == 0;
}

// Loads the little-endian value at p, of n <= sizeof(T) bytes.
template <typename T>
NODISCARD INLINE T read_le(const uint8_t* p, size_t n = sizeof(T))
{
	T v = 0;
	if (is_little_endian())
		std::memcpy(&v, p, n);
	else
		for (size_t i = 0; i < n; ++i)
			v |= static_cast<T>(static_cast<T>(p[i]) << (8 * i));
	return v;
}
