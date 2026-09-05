#pragma once

#include "chess/chess.h"

#include "util/defines.h"

#include <array>
#include <cstdint>
#include <vector>

// An opposing pawn pair (lowercase 'p'): a white and a black pawn on the same
// file, white below black, neither able to pass or promote while the pair
// stands. This ranks the joint placement, collapsing two free-pawn dimensions
// into one. White ranks 2..6, black 3..7: C(6,2) x 8 files = 120 placements,
// all 8 enumerated because the slice manager folds the mirror on top.
struct Pair_Group
{
	using Index = uint32_t;

	Pair_Group()
	{
		m_inverse.fill(-1);
		// Layout: file-major; within a file, ascending white rank then black rank.
		for (int f = 0; f < 8; ++f)
		{
			const File file = static_cast<File>(f);
			for (int wr = static_cast<int>(RANK_2); wr <= static_cast<int>(RANK_6); ++wr)
				for (int br = wr + 1; br <= static_cast<int>(RANK_7); ++br)
				{
					const Square w = sq_make(static_cast<Rank>(wr), file);
					const Square b = sq_make(static_cast<Rank>(br), file);
					m_inverse[inv_key(w, b)] = static_cast<int32_t>(m_white.size());
					m_white.push_back(w);
					m_black.push_back(b);
				}
		}
		ASSERT(m_white.size() == PLACEMENT_COUNT);
	}

	static constexpr size_t PLACEMENT_COUNT = 120;

	NODISCARD size_t table_size() const { return m_white.size(); }

	NODISCARD Square white_square(Index i) const { return m_white[i]; }
	NODISCARD Square black_square(Index i) const { return m_black[i]; }

	// Caller guarantees a legal pair.
	NODISCARD Index index_of(Square white_sq, Square black_sq) const
	{
		const int32_t idx = m_inverse[inv_key(white_sq, black_sq)];
		ASSERT(idx >= 0);
		return static_cast<Index>(idx);
	}

	// Same file, white below black, both on ranks the enumeration covers.
	NODISCARD static bool is_opposing_pair(Square white_sq, Square black_sq)
	{
		if (sq_file(white_sq) != sq_file(black_sq))
			return false;
		const int wr = static_cast<int>(sq_rank(white_sq));
		const int br = static_cast<int>(sq_rank(black_sq));
		return wr >= static_cast<int>(RANK_2)
		    && br <= static_cast<int>(RANK_7)
		    && wr < br;
	}

	// The pair pawns are indistinguishable from free ones on a board, so the slot
	// is positional: the opposing pair minimal by (file, white_rank, black_rank).
	static void canonical_pair(Const_Span<Square> white_sqs,
	                           Const_Span<Square> black_sqs,
	                           Square& out_white, Square& out_black)
	{
		const bool ok = find_canonical(white_sqs, black_sqs, out_white, out_black);
		ASSERT(ok);
		(void)ok;
	}

	NODISCARD static bool find_canonical(Const_Span<Square> white_sqs,
	                                     Const_Span<Square> black_sqs,
	                                     Square& out_white, Square& out_black)
	{
		bool found = false;
		for (size_t i = 0; i < white_sqs.size(); ++i)
			for (size_t j = 0; j < black_sqs.size(); ++j)
			{
				const Square w = white_sqs[i], b = black_sqs[j];
				if (!is_opposing_pair(w, b))
					continue;
				if (!found || less_pair(w, b, out_white, out_black))
				{
					out_white = w;
					out_black = b;
					found = true;
				}
			}
		return found;
	}

private:
	NODISCARD static int inv_key(Square w, Square b)
	{
		return static_cast<int>(w) * 64 + static_cast<int>(b);
	}

	// Lexicographic order on (file, white_rank, black_rank).
	NODISCARD static bool less_pair(Square w1, Square b1, Square w2, Square b2)
	{
		if (sq_file(w1) != sq_file(w2)) return sq_file(w1) < sq_file(w2);
		if (sq_rank(w1) != sq_rank(w2)) return sq_rank(w1) < sq_rank(w2);
		return sq_rank(b1) < sq_rank(b2);
	}

	std::vector<Square> m_white;
	std::vector<Square> m_black;
	std::array<int32_t, 64 * 64> m_inverse;
};
