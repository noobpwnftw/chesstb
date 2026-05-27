#include "egtb/piece_group.h"

#include "chess/chess.h"
#include "util/defines.h"

#include <algorithm>
#include <array>
#include <vector>

Piece_Group::Piece_Group(Piece pc, size_t count, Const_Span<Square> legal_squares) :
	m_piece(pc),
	m_num_pieces(count),
	m_diff_on_move{}
{
	std::vector<Square> ls(legal_squares.begin(), legal_squares.end());
	std::sort(ls.begin(), ls.end());
	ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
	m_num_legal_squares = ls.size();

	std::fill(std::begin(m_sq_to_pos), std::end(m_sq_to_pos), int8_t(-1));
	for (size_t i = 0; i < m_num_legal_squares; ++i)
	{
		m_pos_to_sq[i] = ls[i];
		m_sq_to_pos[ls[i]] = static_cast<int8_t>(i);
	}

	m_table_size = binomial(m_num_legal_squares, count);

	// Non-unique slot weights: num_legal_squares^slot. Detect overflow and
	// budget overrun so we can skip the LUT for very large groups.
	int64_t weights[MAX_PIECE_GROUP_SIZE] = {};
	size_t non_unique_size = 1;
	bool build_non_unique_lut = true;
	for (size_t i = 0; i < count; ++i)
	{
		weights[i] = static_cast<int64_t>(non_unique_size);
		if (m_num_legal_squares != 0
		    && non_unique_size > MAX_NON_UNIQUE_LUT_SIZE / m_num_legal_squares)
		{
			build_non_unique_lut = false;
			break;
		}
		non_unique_size *= m_num_legal_squares;
	}
	if (non_unique_size > MAX_NON_UNIQUE_LUT_SIZE)
		build_non_unique_lut = false;

	// Enumerate all C(legal, N) sorted subsets; emit slot == combinadic rank.
	m_placements_cache.resize(m_table_size);
	int8_t pos[MAX_PIECE_GROUP_SIZE] = {};
	for (size_t i = 0; i < count; ++i)
		pos[i] = static_cast<int8_t>(i);
	while (true)
	{
		Placement pl;
		uint64_t rank = 0;
		for (size_t i = 0; i < count; ++i)
		{
			pl.add_unsorted(m_pos_to_sq[pos[i]]);
			rank += BINOMIAL[static_cast<size_t>(pos[i])][i + 1];
		}
		ASSERT(rank < m_table_size);
		m_placements_cache[rank] = pl;

		ptrdiff_t k = static_cast<ptrdiff_t>(count) - 1;
		while (k >= 0
		       && pos[k] >= static_cast<int8_t>(m_num_legal_squares - count + k))
			--k;
		if (k < 0) break;
		pos[k] += 1;
		for (size_t j = static_cast<size_t>(k) + 1; j < count; ++j)
			pos[j] = pos[j - 1] + 1;
	}

	// Forward map stores the canonical (sorted) non-unique index. Inverse LUT
	// must accept every one of the N! permutations of each multiset. Skipped
	// for large groups; compound_index_after_quiet_move falls back to direct
	// combinadic recomputation in that case.
	if (build_non_unique_lut)
	{
		m_unique_to_non_unique.assign(m_table_size, 0);
		m_non_unique_to_unique.assign(non_unique_size, Placement_Index(0));
		std::array<size_t, MAX_PIECE_GROUP_SIZE> perm{};
		for (Placement_Index r = 0; r < static_cast<Placement_Index>(m_table_size); ++r)
		{
			const Placement& pl = m_placements_cache[r];
			int64_t canon_nu = 0;
			for (size_t i = 0; i < count; ++i)
				canon_nu += weights[i] * m_sq_to_pos[pl[i]];
			m_unique_to_non_unique[r] = canon_nu;

			for (size_t i = 0; i < count; ++i)
				perm[i] = i;
			do
			{
				int64_t nu = 0;
				for (size_t i = 0; i < count; ++i)
					nu += weights[i] * m_sq_to_pos[pl[perm[i]]];
				ASSERT(nu >= 0 && static_cast<size_t>(nu) < non_unique_size);
				m_non_unique_to_unique[static_cast<size_t>(nu)] = r;
			} while (std::next_permutation(perm.begin(), perm.begin() + count));
		}
	}

	// diff-on-move LUT. Only legal source/destination squares are filled.
	for (size_t slot = 0; slot < count; ++slot)
	{
		for (size_t f = 0; f < SQUARE_NB; ++f)
		{
			const int8_t pf = m_sq_to_pos[f];
			if (pf < 0) continue;
			for (size_t t = 0; t < SQUARE_NB; ++t)
			{
				const int8_t pt = m_sq_to_pos[t];
				if (pt < 0) continue;
				m_diff_on_move[slot][f][t] =
					weights[slot] * (static_cast<int64_t>(pt) - static_cast<int64_t>(pf));
			}
		}
	}
}
