#pragma once

#include "defines.h"

#include <type_traits>

#define ENUM_ENABLE_OPERATOR_INC(T) \
constexpr T& operator++(T& lhs) \
{ \
	return lhs = static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) + 1); \
}

#define ENUM_ENABLE_OPERATOR_DEC(T) \
constexpr T& operator--(T& lhs) \
{ \
	return lhs = static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) - 1); \
}

#define ENUM_ENABLE_OPERATOR_ADD_EQ(T) \
template <typename IntT> \
constexpr T& operator+=(T& lhs, IntT rhs) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return lhs = static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) + rhs); \
}

#define ENUM_ENABLE_OPERATOR_SUB_EQ(T) \
template <typename IntT> \
constexpr T& operator-=(T& lhs, IntT rhs) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return lhs = static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) - rhs); \
}

#define ENUM_ENABLE_OPERATOR_ADD(T) \
template <typename IntT> \
NODISCARD constexpr T operator+(T lhs, IntT rhs) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) + rhs); \
}

#define ENUM_ENABLE_OPERATOR_SUB(T) \
template <typename IntT> \
NODISCARD constexpr T operator-(T lhs, IntT rhs) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(lhs) - rhs); \
}

#define ENUM_ENABLE_OPERATOR_DIFF(T) \
NODISCARD constexpr ptrdiff_t operator-(T lhs, T rhs) \
{ \
	return static_cast<ptrdiff_t>(lhs) - static_cast<ptrdiff_t>(rhs); \
}

#define ENUM_ENABLE_OPERATOR_OR(T) \
NODISCARD constexpr T operator|(T a, T b) \
{ \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_OR_EQ(T) \
constexpr T& operator|=(T& a, T b) \
{ \
	return a = static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_XOR(T) \
NODISCARD constexpr T operator^(T a, T b) \
{ \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_XOR_EQ(T) \
constexpr T& operator^=(T& a, T b) \
{ \
	return a = static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_AND(T) \
NODISCARD constexpr T operator&(T a, T b) \
{ \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_AND_EQ(T) \
constexpr T& operator&=(T& a, T b) \
{ \
	return a = static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_INV(T) \
NODISCARD constexpr T operator~(T a) \
{ \
	return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a)); \
}

#define ENUM_ENABLE_OPERATOR_RSHIFT(T) \
template <typename IntT> \
NODISCARD constexpr T operator>>(T a, IntT b) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) >> static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_RSHIFT_EQ(T) \
template <typename IntT> \
constexpr T& operator>>=(T& a, IntT b) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return a = static_cast<T>(static_cast<std::underlying_type_t<T>>(a) >> static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_LSHIFT(T) \
template <typename IntT> \
NODISCARD constexpr T operator<<(T a, IntT b) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) << static_cast<std::underlying_type_t<T>>(b)); \
}

#define ENUM_ENABLE_OPERATOR_LSHIFT_EQ(T) \
template <typename IntT> \
constexpr T& operator<<=(T& a, IntT b) \
{ \
	static_assert(std::is_integral_v<IntT> && !std::is_same_v<IntT, T>); \
	return a = static_cast<T>(static_cast<std::underlying_type_t<T>>(a) << static_cast<std::underlying_type_t<T>>(b)); \
}
