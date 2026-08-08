#pragma once

#include "defines.h"

#include <array>
#include <type_traits>

namespace detail
{
	template<size_t... Is, typename T>
	constexpr auto make_filled_array(std::index_sequence<Is...>, const T& v)
	{
		return std::array<std::decay_t<T>, sizeof...(Is)>{((void)Is, v)...};
	}
}

template<size_t N, typename T>
constexpr auto make_filled_array(const T& v)
{
	return detail::make_filled_array(std::make_index_sequence<N>(), v);
}
