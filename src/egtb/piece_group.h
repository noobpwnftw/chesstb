#pragma once

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/span.h"
#include "util/fixed_vector.h"
#include "util/enum.h"
#include "util/utility.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// A Piece_Group holds N identical pieces of one type, ranked combinatorially
// over a fixed legal-square set. Knows nothing about symmetry transforms or
// other groups; callers must canonicalize placements before ranking.

// Pascal's triangle. BINOMIAL[k][n] = C(k, n). Used for combinadic ranking.
constexpr std::array<std::array<uint64_t, 8>, 65> BINOMIAL = []() {
	std::array<std::array<uint64_t, 8>, 65> b{};
	for (size_t k = 0; k <= 64; ++k)
	{
		b[k][0] = 1;
		for (size_t n = 1; n <= 7; ++n)
		{
			if (n > k)
				b[k][n] = 0;
			else
				b[k][n] = b[k - 1][n - 1] + b[k - 1][n];
		}
	}
	return b;
}();

NODISCARD INLINE constexpr uint64_t binomial(size_t k, size_t n)
{
	if (n > 7 || k > 64)
		return 0;
	return BINOMIAL[k][n];
}

struct Piece_Group
{
	// Bumping past 7 would overflow Placement_Index (uint32_t) for some shapes.
	static constexpr size_t MAX_PIECE_GROUP_SIZE = 7;
	static constexpr size_t MAX_NON_UNIQUE_LUT_SIZE = size_t(1) << 28;

	using Placement_Index = uint32_t;

	// Sorted ascending list of squares, up to MAX_PIECE_GROUP_SIZE entries.
	struct alignas(8) Placement
	{
		Placement() :
			m_size(0)
		{
		}

		INLINE void clear() { m_size = 0; }

		// Replace `from` with `to`, preserving ascending sort.
		NODISCARD INLINE Placement with_moved_square(Square from, Square to) const
		{
			Placement dst = *this;
			for (size_t i = 0; i < static_cast<size_t>(dst.m_size); ++i)
				if (dst.m_squares[i] == from)
				{
					dst.m_squares[i] = to;
					break;
				}
			dst.sort();
			return dst;
		}

		NODISCARD INLINE Placement with_removed_square(Square to_remove) const
		{
			Placement dst;
			for (size_t i = 0; i < static_cast<size_t>(m_size); ++i)
				if (m_squares[i] != to_remove)
					dst.add(m_squares[i]);
			return dst;
		}

		// Apply `f` to every square, then re-sort.
		template <typename F>
		NODISCARD INLINE Placement with_transformed_squares(F&& f) const
		{
			Placement dst;
			dst.m_size = m_size;
			for (size_t i = 0; i < static_cast<size_t>(m_size); ++i)
				dst.m_squares[i] = f(m_squares[i]);
			dst.sort();
			return dst;
		}

		INLINE void add(Square s)
		{
			ASSERT(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
			size_t i = m_size;
			while (i > 0 && m_squares[i - 1] > s)
			{
				m_squares[i] = m_squares[i - 1];
				--i;
			}
			m_squares[i] = s;
			m_size += 1;
		}

		// Append without sorting; caller must sort() afterward.
		INLINE void add_unsorted(Square s)
		{
			ASSERT(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
			m_squares[m_size++] = s;
		}

		INLINE void set_single(Square s)
		{
			m_squares[0] = s;
			m_size = 1;
		}

		INLINE void sort()
		{
			std::sort(m_squares, m_squares + m_size);
		}

		NODISCARD bool are_all_squares_unique() const
		{
			for (size_t i = 1; i < static_cast<size_t>(m_size); ++i)
				if (m_squares[i] == m_squares[i - 1])
					return false;
			return true;
		}

		NODISCARD INLINE Square& operator[](size_t i) { ASSERT(i < static_cast<size_t>(m_size)); return m_squares[i]; }
		NODISCARD INLINE Square  operator[](size_t i) const { ASSERT(i < static_cast<size_t>(m_size)); return m_squares[i]; }

		NODISCARD INLINE size_t size() const { return m_size; }

		NODISCARD INLINE const Square* begin() const { return m_squares; }
		NODISCARD INLINE       Square* begin()       { return m_squares; }
		NODISCARD INLINE const Square* end()   const { return m_squares + m_size; }
		NODISCARD INLINE       Square* end()         { return m_squares + m_size; }

	private:
		Square m_squares[MAX_PIECE_GROUP_SIZE];
		int8_t m_size;
	};

	// `legal_squares` need not be sorted; we sort/dedupe internally.
	Piece_Group(Piece pc, size_t count, Const_Span<Square> legal_squares);

	NODISCARD INLINE Placement_Index compound_index(const Placement& list) const
	{
		ASSERT(list.size() == m_num_pieces);
		// Placement is sorted ascending; m_sq_to_pos preserves that order
		// (legal_squares were sorted at construction), satisfying combinadic.
		uint64_t rank = 0;
		for (size_t i = 0; i < m_num_pieces; ++i)
		{
			const int p = m_sq_to_pos[list[i]];
			ASSERT(p >= 0);
			rank += BINOMIAL[static_cast<size_t>(p)][i + 1];
		}
		ASSERT(rank < m_table_size);
		return static_cast<Placement_Index>(rank);
	}

	// O(1) decode of rank to canonical sorted placement.
	NODISCARD INLINE const Placement& squares(Placement_Index idx) const
	{
		ASSERT(idx < m_table_size);
		return m_placements_cache[idx];
	}

	// O(1) rank update for a quiet move via diff-on-move LUT in non-unique space,
	// then mapped back to canonical rank.
	NODISCARD INLINE Placement_Index compound_index_after_quiet_move(
		Placement_Index curr, Square from, Square to) const
	{
		static_assert(MAX_PIECE_GROUP_SIZE == 7);
		const Placement& list = m_placements_cache[curr];
		if (m_non_unique_to_unique.empty())
			return compound_index(list.with_moved_square(from, to));
		size_t slot;
		if      (list.size() >= 1 && list[0] == from) slot = 0;
		else if (list.size() >= 2 && list[1] == from) slot = 1;
		else if (list.size() >= 3 && list[2] == from) slot = 2;
		else if (list.size() >= 4 && list[3] == from) slot = 3;
		else if (list.size() >= 5 && list[4] == from) slot = 4;
		else if (list.size() >= 6 && list[5] == from) slot = 5;
		else if (list.size() >= 7 && list[6] == from) slot = 6;
		else { ASSERT(false); slot = 0; }
		const size_t non_unique_before = m_unique_to_non_unique[curr];
		const size_t non_unique_after =
			non_unique_before + m_diff_on_move[slot][from][to];
		return m_non_unique_to_unique[non_unique_after];
	}

	NODISCARD INLINE size_t size() const { return m_num_pieces; }
	NODISCARD INLINE size_t table_size() const { return m_table_size; }
	NODISCARD INLINE Piece piece() const { return m_piece; }

	NODISCARD INLINE bool sq_is_legal(Square sq) const { return m_sq_to_pos[sq] >= 0; }

private:
	Piece m_piece;
	size_t m_num_pieces;
	size_t m_num_legal_squares;
	uint64_t m_table_size;  // C(num_legal_squares, num_pieces)

	// sq -> position within legal-square list, -1 for illegal.
	int8_t m_sq_to_pos[SQUARE_NB];
	Square m_pos_to_sq[SQUARE_NB];

	std::vector<Placement> m_placements_cache;

	// Non-unique index space: slot i has weight num_legal_squares^i, size N^count.
	// All N! permutations of one multiset map to distinct non-unique indices that
	// collapse to a single canonical rank.
	std::vector<int64_t> m_unique_to_non_unique;
	std::vector<Placement_Index> m_non_unique_to_unique;

	// m_diff_on_move[slot][from][to] = weights[slot] * (sq_to_pos[to] - sq_to_pos[from]).
	int64_t m_diff_on_move[MAX_PIECE_GROUP_SIZE][SQUARE_NB][SQUARE_NB];
};

NODISCARD inline std::vector<Square> default_legal_squares(Piece pc)
{
	std::vector<Square> out;
	const size_t n = possible_sq_nb(pc);
	out.reserve(n);
	for (size_t i = 0; i < n; ++i)
		out.push_back(possible_sq(pc, i));
	return out;
}
