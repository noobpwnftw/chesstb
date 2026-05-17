#pragma once

#include "defines.h"

#include <utility>
#include <string>
#include <chrono>
#include <cstring>
#include <vector>
#include <cstdio>

template <typename T>
using Vector_Not_Bool = std::vector<
	std::conditional_t<
		std::is_same_v<
			T,
			bool
		>,
		uint8_t,
		T
	>
>;

template<typename... Ts>
struct overload : Ts... {
	explicit overload(Ts&&... funcs) :
		Ts(std::forward<Ts>(funcs))...
	{
	}

	using Ts::operator()...;
};

NODISCARD INLINE std::string strip(const std::string& s, const char ws[] = " \t\r\n\v\f")
{
	const auto start = s.find_first_not_of(ws);
	if (start == std::string::npos)
		return {};

	const auto end = s.find_last_not_of(ws);
	
	return s.substr(start, end - start + 1);
}

template <typename T>
NODISCARD INLINE double elapsed_milliseconds(T start_time, T end_time)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / 1e6;
}

template <typename T>
NODISCARD INLINE double elapsed_seconds(T start_time, T end_time)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / 1e9;
}

template <typename T>
NODISCARD INLINE std::string format_elapsed_time(T start_time, T end_time)
{
	const size_t t = static_cast<size_t>(elapsed_milliseconds(start_time, end_time));
	const size_t ms = t % 1000;
	const size_t s = t / 1000 % 60;
	const size_t m = t / 1000 / 60 % 60;
	const size_t h = t / 1000 / 60 / 60;

	char buf[32];
	snprintf(buf, sizeof(buf), "%02zu:%02zu:%02zu.%03zu", h, m, s, ms);
	return std::string(buf);
}

template <typename... T>
[[noreturn]] inline void print_and_abort(T&&... args)
{
	printf(std::forward<T>(args)...);
	abort();
}
