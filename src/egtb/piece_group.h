#pragma once

#include "chess/bitboard.h"
#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/span.h"
#include "util/fixed_vector.h"
#include "util/enum.h"
#include "util/utility.h"

// Pascal's triangle. BINOMIAL[k][n] = C(k, n). Used for combinadic ranking.
inline constexpr std::array<std::array<uint32_t, 8>, 65> BINOMIAL = []() {
	std::array<std::array<uint32_t, 8>, 65> b{};
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

NODISCARD constexpr uint32_t binomial(size_t k, size_t n)
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

		// Append a square above all the others.
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

		NODISCARD INLINE Square& operator[](size_t i) { ASSERT(i < static_cast<size_t>(m_size)); return m_squares[i]; }
		NODISCARD INLINE Square  operator[](size_t i) const { ASSERT(i < static_cast<size_t>(m_size)); return m_squares[i]; }

		NODISCARD INLINE size_t size() const { return m_size; }
		NODISCARD INLINE Bitboard occupancy() const
		{
			Bitboard bb = Bitboard::make_empty();
			for (int8_t i = 0; i < m_size; ++i) bb |= m_squares[i];
			return bb;
		}

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

	// Colex rank of the pieces on `b`: the bits pop in ascending order.
	NODISCARD Placement_Index index_of(Bitboard b) const
	{
		ASSERT(popcnt(b.bits()) == m_num_pieces);
		uint32_t rank = 0;
		for (size_t k = 1; b; ++k)
		{
			const int p = m_sq_to_pos[b.pop_first_square()];
			ASSERT(p >= 0);
			rank += BINOMIAL[static_cast<size_t>(p)][k];
		}
		ASSERT(rank < m_table_size);
		return static_cast<Placement_Index>(rank);
	}

	NODISCARD const Placement& squares(Placement_Index idx) const
	{
		ASSERT(idx < m_table_size);
		return m_placements_cache[idx];
	}

	// O(1) rank update for a quiet move. `list` is the caller's placement for this
	// group, i.e. squares(curr). The non-unique index is built in `list`'s slot
	// order, not sorted; m_non_unique_to_unique accepts all N! permutations.
	NODISCARD Placement_Index compound_index_after_quiet_move(
		const Placement& list, Square from, Square to) const
	{
		if (m_non_unique_to_unique.empty())
			return index_of((list.occupancy() ^ from) | to);
		ASSERT(m_sq_to_pos[to] >= 0);
		int32_t non_unique = 0;
		for (size_t i = 0; i < m_num_pieces; ++i)
		{
			const Square s = (list[i] == from) ? to : list[i];
			non_unique += m_weights[i] * m_sq_to_pos[s];
		}
		ASSERT(non_unique >= 0
		       && static_cast<size_t>(non_unique) < m_non_unique_to_unique.size());
		return m_non_unique_to_unique[static_cast<size_t>(non_unique)];
	}

	NODISCARD size_t size() const { return m_num_pieces; }
	NODISCARD size_t table_size() const { return m_table_size; }
	NODISCARD Piece piece() const { return m_piece; }

private:
	Piece m_piece;
	size_t m_num_pieces;
	size_t m_num_legal_squares;
	uint32_t m_table_size;  // C(num_legal_squares, num_pieces), <= C(64, 7)

	// sq -> position within legal-square list, -1 for illegal.
	int8_t m_sq_to_pos[SQUARE_NB];
	Square m_pos_to_sq[SQUARE_NB];

	std::vector<Placement> m_placements_cache;

	// Non-unique index space: slot i has weight num_legal_squares^i, size N^count.
	// All N! permutations of one multiset map to distinct non-unique indices that
	// collapse to a single canonical rank.
	int32_t m_weights[MAX_PIECE_GROUP_SIZE];
	std::vector<Placement_Index> m_non_unique_to_unique;
};

NODISCARD INLINE std::vector<Square> default_legal_squares(Piece pc)
{
	std::vector<Square> out;
	const size_t n = possible_sq_nb(pc);
	out.reserve(n);
	for (size_t i = 0; i < n; ++i)
		out.push_back(possible_sq(pc, i));
	return out;
}
