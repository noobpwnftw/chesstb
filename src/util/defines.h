#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

#if defined(NDEBUG)
#if defined(_MSC_VER)
#define ASSUME(a) __assume(a)
#else
#define ASSUME(a) if (!(a)) __builtin_unreachable()
#endif
#else
#define ASSUME(a) assert(a)
#endif

#define ASSERT(a) assert(a)
#define INLINE inline
#define NODISCARD [[nodiscard]]

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__ ((noinline))
#endif

#if defined(NDEBUG)
constexpr inline void NOINLINE ASSERT_ALWAYS(bool a)
{
	if (!a)
		abort();
}
#else
#define ASSERT_ALWAYS(a) ASSERT(a);
#endif

using std::uint8_t;
using std::int8_t;
using std::uint16_t;
using std::int16_t;
using std::uint32_t;
using std::int32_t;
using std::uint64_t;
using std::int64_t;
using std::size_t;
using std::ptrdiff_t;

static_assert(sizeof(size_t) >= 8);

struct For_Overwrite_Tag {};

template <size_t N>
using Unsigned_Int_Of_Size =
	std::conditional_t<N == 1, uint8_t,
	std::conditional_t<N == 2, uint16_t,
	std::conditional_t<N == 4, uint32_t,
	std::conditional_t<N == 8, uint64_t, void>>>>;

template <typename T>
using Identity = T;

constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;

template <typename ToT, typename FromT>
NODISCARD constexpr ToT narrowing_static_cast(FromT from)
{
	ASSERT(static_cast<FromT>(static_cast<ToT>(from)) == from);
	return static_cast<ToT>(from);
}
