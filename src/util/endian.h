#pragma once

#include "defines.h"

#include <cstring>

NODISCARD inline bool is_little_endian()
{
	std::uint64_t value = 0x0807060504030201ull;
	char little_endian_bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	char bytes[8];
	std::memcpy(bytes, &value, 8);
	return std::memcmp(bytes, little_endian_bytes, 8) == 0;
}
