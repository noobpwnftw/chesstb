#pragma once

#include "chess/chess.h"

#include "util/defines.h"

#include <array>
#include <cstdint>
#include <vector>

// An opposing pawn pair (lowercase 'p'): one white pawn and one black pawn on
// the SAME FILE with the white pawn below the black pawn (white on rank r, black
// on rank s, r < s). Neither can pass the other and neither can promote while
// the pair stands; they may push toward each other until adjacent (then frozen),
// and the pair is broken only by a capture (a member captures, or is captured).
// Both members are ordinary pawns on the board -- this class only enumerates and
// ranks the joint placement, collapsing what would be two independent free-pawn
// dimensions (~2304) into one same-file dimension. At most one pair per material.
//
// The slice manager folds file-mirror symmetry on top and drives the push
// transitions between placements, so all 8 files are enumerated here. White
// ranks run 2..6, black 3..7 with black strictly above white: C(6,2) = 15 rank
// pairs x 8 files = 120 placements.
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
	}

	NODISCARD size_t table_size() const { return m_white.size(); }

	NODISCARD Square white_square(Index i) const { return m_white[i]; }
	NODISCARD Square black_square(Index i) const { return m_black[i]; }

	// Rank of a (white, black) opposing pair. Caller guarantees a legal pair.
	NODISCARD Index index_of(Square white_sq, Square black_sq) const
	{
		const int32_t idx = m_inverse[inv_key(white_sq, black_sq)];
		ASSERT(idx >= 0);
		return static_cast<Index>(idx);
	}

	// Whether two squares form a legal opposing pair as enumerated here:
	// same file, white below black, both on ranks the enumeration covers.
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

	NODISCARD Index file_mirror(Index i) const
	{
		return index_of(sq_file_mirror(m_white[i]), sq_file_mirror(m_black[i]));
	}

	// On a board the pair pawns are indistinguishable from free pawns, so the
	// pair slot is identified positionally: it is the opposing pawn pair (same
	// file, white below black) minimal by (file, white_rank, black_rank). Both
	// the forward enumeration (which prunes cells where the designated pair is
	// not this minimum) and the reverse board->index lookup use this one rule, so
	// every stored cell round-trips. Returns false when no opposing pair exists.
	// For callers that know an opposing pair is present (indexing a stored
	// pair-table position): set the out squares, asserting one exists.
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
