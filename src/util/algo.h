#pragma once

#include "defines.h"
#include "span.h"

#include <cstdint>
#include <vector>
#include <algorithm>
#include <utility>
#include <atomic>
#include <limits>

template <typename T = size_t>
struct Mixed_Radix
{
	struct iterator_end_sentinel {};
	struct const_iterator
	{
		using value_type = std::vector<T>;
		using reference = value_type&;
		using const_reference = const value_type&;
		using pointer = value_type*;
		using const_pointer = const value_type*;

		const_iterator(std::vector<T> radices, std::vector<T> base_values) :
			m_base_values(std::move(base_values)),
			m_radices(std::move(radices)),
			m_current(this->m_base_values),
			m_have_next(true)
		{
		}

		const_iterator& operator++()
		{
			m_have_next = false;
			++m_current[0];
			for (size_t i = 0; i < m_current.size(); ++i)
			{
				if (m_current[i] == m_radices[i])
				{
					m_current[i] = m_base_values[i];
					if (i + 1 < m_current.size())
						++m_current[i + 1];
				}
				else
				{
					m_have_next = true;
					break;
				}
			}

			return *this;
		}

		NODISCARD const_reference operator*() const
		{
			return m_current;
		}

		NODISCARD const_pointer operator->() const
		{
			return &m_current;
		}

		NODISCARD friend bool operator==(const const_iterator& it, const iterator_end_sentinel&)
		{
			return !it.m_have_next;
		}

		NODISCARD friend bool operator!=(const const_iterator& it, const iterator_end_sentinel&)
		{
			return it.m_have_next;
		}

	private:
		std::vector<T> m_base_values;
		std::vector<T> m_radices;
		std::vector<T> m_current;
		bool m_have_next;
	};

	explicit Mixed_Radix(Const_Span<T> radices) :
		m_radices(radices.begin(), radices.end())
	{
		m_base_values.resize(this->m_radices.size(), 0);
	}

	NODISCARD static Mixed_Radix with_inclusive_ranges(Const_Span<std::pair<T, T>> ranges)
	{
		return Mixed_Radix(ranges, true);
	}

	NODISCARD static Mixed_Radix with_ranges(Const_Span<std::pair<T, T>> ranges)
	{
		return Mixed_Radix(ranges, false);
	}

	NODISCARD const_iterator begin() const
	{
		return const_iterator(m_radices, m_base_values);
	}

	NODISCARD iterator_end_sentinel end() const
	{
		return {};
	}

	NODISCARD size_t size() const
	{
		size_t s = 1;
		for (size_t i = 0; i < m_radices.size(); ++i)
			s *= m_radices[i] - m_base_values[i];
		return s;
	}

private:
	std::vector<T> m_base_values;
	std::vector<T> m_radices;

	Mixed_Radix(Const_Span<std::pair<T, T>> ranges, bool inclusive)
	{
		m_base_values.reserve(ranges.size());
		m_radices.reserve(ranges.size());
		for (auto [a, b] : ranges)
		{
			if (b + inclusive - a < 1)
				throw std::runtime_error("Radix must have at least 1 allowed value.");

			m_base_values.emplace_back(a);
			m_radices.emplace_back(b + inclusive);
		}
	}
};

template <typename T>
struct Multi_Permuter
{
	using Source_Iterator_Type = typename T::iterator;

	explicit Multi_Permuter(std::vector<std::pair<Source_Iterator_Type, Source_Iterator_Type>> ranges) :
		m_ranges(std::move(ranges))
	{
		for (const auto& [begin, end] : this->m_ranges)
			std::sort(begin, end);
	}

	Multi_Permuter(T& all, const std::vector<std::pair<size_t, size_t>>& idx_ranges)
	{
		m_ranges.reserve(idx_ranges.size());
		for (const auto& [begin_idx, end_idx] : idx_ranges)
		{
			const Source_Iterator_Type begin = all.begin() + begin_idx;
			const Source_Iterator_Type end = all.begin() + end_idx;
			std::sort(begin, end);
			m_ranges.emplace_back(begin, end);
		}
	}

	NODISCARD bool try_advance()
	{
		if (m_ranges.empty())
			return false;

		bool perm_overflow = !std::next_permutation(m_ranges[0].first, m_ranges[0].second);
		for (size_t i = 0; i < m_ranges.size(); ++i)
		{
			if (!perm_overflow)
				return true;
			else if (i + 1 < m_ranges.size())
				perm_overflow = !std::next_permutation(m_ranges[i + 1].first, m_ranges[i + 1].second);
			else
				perm_overflow = false;
		}

		return false;
	}

private:
	std::vector<std::pair<Source_Iterator_Type, Source_Iterator_Type>> m_ranges;
};
