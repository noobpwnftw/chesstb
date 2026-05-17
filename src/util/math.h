#pragma once

#include "defines.h"

#include <atomic>

template <typename T>
NODISCARD INLINE constexpr T ceil_to_multiple(const T& val, const T& alignment)
{
	const size_t misalignment = val % alignment;
	if (misalignment == 0)
		return val;

	return val + (alignment - misalignment);
}

template <typename T>
NODISCARD constexpr T ceil_to_power_of_2(const T& val)
{
	T ret = 1;
	while (ret < val)
		ret *= 2;
	return ret;
}

NODISCARD constexpr size_t nth_bit(size_t e)
{
	ASSERT(e < 64);
	return static_cast<size_t>(1) << e;
}

NODISCARD constexpr size_t pow_2(size_t e)
{
	ASSERT(e < 64);
	return static_cast<size_t>(1) << e;
}

template <typename T>
NODISCARD INLINE constexpr T ceil_div(const T& val, const T& divisor)
{
	return val / divisor + (val % divisor != 0);
}

template <typename T>
NODISCARD inline T* align_ptr_up(T* ptr, size_t alignment)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
	v = ceil_to_multiple(v, alignment);
	return reinterpret_cast<T*>(v);
}

template <typename T>
NODISCARD inline const T* align_ptr_up(const T* ptr, size_t alignment)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
	v = ceil_to_multiple(v, alignment);
	return reinterpret_cast<const T*>(v);
}

template <typename T>
NODISCARD inline bool ptr_is_aligned(const T* ptr, size_t alignment)
{
	return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

template <typename T>
NODISCARD T ceil_to_odd(const T& val)
{
	return val + (1 - (val & 1));
}

template <typename T>
NODISCARD T ceil_to_even(const T& val)
{
	return val + (val & 1);
}

template <typename T>
void atomic_update_max(std::atomic<T>& maximum_value, const T& value) noexcept
{
	T prev_value = maximum_value;
	while (value > prev_value
		&& !maximum_value.compare_exchange_weak(prev_value, value));
}

template <typename T>
void atomic_update_min(std::atomic<T>& minimum_value, const T& value) noexcept
{
	T prev_value = minimum_value;
	while (value < prev_value
		&& !minimum_value.compare_exchange_weak(prev_value, value));
}

template <typename T>
void update_max(T& current_value, const T& new_value)
{
	if (new_value > current_value)
		current_value = new_value;
}

template <typename T>
void update_min(T& current_value, const T& new_value)
{
	if (new_value < current_value)
		current_value = new_value;
}

template<typename T>
NODISCARD INLINE bool is_mid(const T& m, const T& a, const T& b)
{
	return ((m - b) * (a - m) >= 0);
}
