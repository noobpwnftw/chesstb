#pragma once

#include "defines.h"
#include "math.h"
#include "span.h"
#include "enum.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <cstdlib>

namespace cpp20
{
	template <typename>
	constexpr bool is_unbounded_array_v = false;
	template <typename T>
	constexpr bool is_unbounded_array_v<T[]> = true;

	template <typename>
	constexpr bool is_bounded_array_v = false;
	template <typename T, size_t N>
	constexpr bool is_bounded_array_v<T[N]> = true;
}

NODISCARD void* allocate_large_pages(size_t bytes);
void deallocate_large_pages(void* ptr);

template <typename T>
struct Huge_Array
{
	using iterator = T*;
	using const_iterator = const T*;

	Huge_Array() :
		m_data(nullptr),
		m_size(0),
		m_uses_large_pages(false)
	{
	}

	Huge_Array(size_t count) :
		m_data(nullptr),
		m_size(count),
		m_uses_large_pages(false)
	{
		void* storage = allocate_large_pages(sizeof(T) * count);
		if (storage)
		{
			m_uses_large_pages = true;
			m_data = reinterpret_cast<T*>(storage);
			for (size_t i = 0; i < count; ++i)
				new (m_data + i) T();
		}
		else
		{
			m_uses_large_pages = false;
			m_data = new T[count]();
		}
	}

	Huge_Array(For_Overwrite_Tag, size_t count) :
		m_data(nullptr),
		m_size(count),
		m_uses_large_pages(false)
	{
		void* storage = allocate_large_pages(sizeof(T) * count);
		if (storage)
		{
			m_uses_large_pages = true;
			m_data = reinterpret_cast<T*>(storage);
			for (size_t i = 0; i < count; ++i)
				new (m_data + i) T;
		}
		else
		{
			m_uses_large_pages = false;
			m_data = new T[count];
		}
	}

	Huge_Array(const Huge_Array&) = delete;
	Huge_Array& operator=(const Huge_Array&) = delete;

	Huge_Array(Huge_Array&& other) :
		m_data(std::exchange(other.m_data, nullptr)),
		m_size(std::exchange(other.m_size, 0)),
		m_uses_large_pages(std::exchange(other.m_uses_large_pages, false))
	{
	}

	Huge_Array& operator=(Huge_Array&& other)
	{
		clear();

		m_data = std::exchange(other.m_data, nullptr);
		m_size = std::exchange(other.m_size, 0);
		m_uses_large_pages = std::exchange(other.m_uses_large_pages, false);

		return *this;
	}

	~Huge_Array()
	{
		clear();
	}

	void clear()
	{
		if (m_data == nullptr)
			return;

		if (m_uses_large_pages)
		{
			if constexpr (!std::is_trivially_destructible_v<T>)
				for (size_t i = 0; i < m_size; ++i)
					m_data[i].~T();

			deallocate_large_pages(m_data);
		}
		else
			delete[] m_data;

		m_data = nullptr;
		m_size = 0;
		m_uses_large_pages = false;
	}

	NODISCARD T& operator[](size_t i)
	{
		return m_data[i];
	}

	NODISCARD const T& operator[](size_t i) const
	{
		return m_data[i];
	}

	NODISCARD size_t size() const
	{
		return m_size;
	}

	NODISCARD T* data()
	{
		return m_data;
	}

	NODISCARD const T* data() const
	{
		return m_data;
	}

	NODISCARD T* begin()
	{
		return m_data;
	}

	NODISCARD const T* begin() const
	{
		return m_data;
	}

	NODISCARD T* end()
	{
		return m_data + m_size;
	}

	NODISCARD const T* end() const
	{
		return m_data + m_size;
	}

private:
	T* m_data;
	size_t m_size;
	bool m_uses_large_pages;
};

namespace cpp20
{
	template <typename T>
	NODISCARD inline
	std::enable_if_t<
		cpp20::is_unbounded_array_v<T>,
		std::unique_ptr<T>
	> make_unique_for_overwrite(size_t size)
	{
		return std::unique_ptr<T>(new std::remove_extent_t<T>[size]);
	}
}
