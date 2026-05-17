#pragma once

#include "math.h"

#include <utility>
#include <type_traits>
#include <iterator>
#include <algorithm>

template <typename T>
struct Const_Span
{
	constexpr Const_Span() :
		m_begin(nullptr),
		m_end(nullptr)
	{
	}

	template <size_t N>
	constexpr explicit Const_Span(const T(&arr)[N]) :
		m_begin(arr),
		m_end(arr + N)
	{
	}

	template <size_t N>
	constexpr explicit Const_Span(const T(*arr)[N]) :
		m_begin(arr),
		m_end(arr + N)
	{
	}

	template <typename ContT>
	constexpr explicit Const_Span(const ContT& container) :
		m_begin(&*container.begin()),
		m_end(m_begin + container.size())
	{
		using IterT = typename ContT::const_iterator;
		using IterCatT = typename std::iterator_traits<IterT>::iterator_category;
		static_assert(std::is_same_v<IterCatT, std::random_access_iterator_tag>);
	}

	constexpr Const_Span(const T* begin_it, const T* end_it) :
		m_begin(begin_it),
		m_end(end_it)
	{
	}

	constexpr Const_Span(const T* begin_it, size_t size) :
		m_begin(begin_it),
		m_end(begin_it + size)
	{
	}

	constexpr Const_Span(const Const_Span&) = default;
	constexpr Const_Span(Const_Span&& other) noexcept :
		m_begin(std::exchange(other.m_begin, nullptr)),
		m_end(std::exchange(other.m_end, nullptr))
	{
	}

	constexpr Const_Span& operator=(const Const_Span&) = default;
	constexpr Const_Span& operator=(Const_Span&& other) noexcept
	{
		m_begin = std::exchange(other.m_begin, nullptr);
		m_end = std::exchange(other.m_end, nullptr);
		return *this;
	}

	constexpr void reset()
	{
		m_begin = m_end = nullptr;
	}

	NODISCARD Const_Span nth_chunk(size_t n, size_t chunk_size) const
	{
		const size_t size = m_end - m_begin;
		const size_t new_begin = std::min(n * chunk_size, size);
		const size_t new_end = std::min(new_begin + chunk_size, size);
		return Const_Span(m_begin + new_begin, m_begin + new_end);
	}

	NODISCARD bool empty() const
	{
		return m_begin == m_end;
	}

	NODISCARD constexpr const T* data() const
	{
		return m_begin;
	}

	NODISCARD constexpr const T* begin() const
	{
		return m_begin;
	}

	NODISCARD constexpr const T* end() const
	{
		return m_end;
	}

	NODISCARD constexpr const T& operator[](size_t i) const
	{
		ASSERT(i < size());
		return m_begin[i];
	}

	NODISCARD constexpr size_t size() const
	{
		return static_cast<size_t>(std::distance(m_begin, m_end));
	}

private:
	const T* m_begin;
	const T* m_end;
};

template <typename ContT>
Const_Span(const ContT&)->Const_Span<typename ContT::value_type>;

template <typename T>
struct Span
{
	constexpr Span() :
		m_begin(nullptr),
		m_end(nullptr)
	{
	}

	template <size_t N>
	constexpr explicit Span(T(&arr)[N]) :
		m_begin(arr),
		m_end(arr + N)
	{
	}

	template <size_t N>
	constexpr explicit Span(T(*arr)[N]) :
		m_begin(arr),
		m_end(arr + N)
	{
	}

	template <typename ContT>
	constexpr explicit Span(ContT& container) :
		m_begin(&*container.begin()),
		m_end(m_begin + container.size())
	{
		using IterT = typename ContT::const_iterator;
		using IterCatT = typename std::iterator_traits<IterT>::iterator_category;
		static_assert(std::is_same_v<IterCatT, std::random_access_iterator_tag>);
	}

	constexpr Span(T* begin_it, T* end_it) :
		m_begin(begin_it),
		m_end(end_it)
	{
	}

	constexpr Span(T* begin_it, size_t size) :
		m_begin(begin_it),
		m_end(begin_it + size)
	{
	}

	constexpr Span(const Span&) = default;
	constexpr Span(Span&& other) noexcept :
		m_begin(std::exchange(other.m_begin, nullptr)),
		m_end(std::exchange(other.m_end, nullptr))
	{
	}

	constexpr Span& operator=(const Span&) = default;
	constexpr Span& operator=(Span&& other) noexcept
	{
		m_begin = std::exchange(other.m_begin, nullptr);
		m_end = std::exchange(other.m_end, nullptr);
		return *this;
	}

	constexpr void reset()
	{
		m_begin = m_end = nullptr;
	}

	NODISCARD Span nth_chunk(size_t n, size_t chunk_size) const
	{
		const size_t size = m_end - m_begin;
		const size_t new_begin = std::min(n * chunk_size, size);
		const size_t new_end = std::min(new_begin + chunk_size, size);
		return Span(m_begin + new_begin, m_begin + new_end);
	}

	NODISCARD bool empty() const
	{
		return m_begin == m_end;
	}

	NODISCARD constexpr T* data()
	{
		return m_begin;
	}

	NODISCARD constexpr const T* data() const
	{
		return m_begin;
	}

	NODISCARD constexpr T* begin()
	{
		return m_begin;
	}

	NODISCARD constexpr const T* begin() const
	{
		return m_begin;
	}

	NODISCARD constexpr T* end()
	{
		return m_end;
	}

	NODISCARD constexpr const T* end() const
	{
		return m_end;
	}

	NODISCARD constexpr const T& operator[](size_t i) const
	{
		ASSERT(i < size());
		return m_begin[i];
	}

	NODISCARD constexpr T& operator[](size_t i)
	{
		ASSERT(i < size());
		return m_begin[i];
	}

	NODISCARD constexpr size_t size() const
	{
		return static_cast<size_t>(std::distance(m_begin, m_end));
	}

	NODISCARD constexpr operator Const_Span<T>() const
	{
		return Const_Span(m_begin, m_end);
	}

private:
	T* m_begin;
	T* m_end;
};

template <typename ContT>
Span(const ContT&)->Span<typename ContT::value_type>;