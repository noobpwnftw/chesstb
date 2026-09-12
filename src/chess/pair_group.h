#pragma once

#include "chess/bitboard.h"
#include "chess/chess.h"

#include "util/defines.h"

#include <array>
#include <cstdint>

// An opposing pawn pair ('p'): a white and a black pawn on one file, white
// below black, so neither can pass or promote. White ranks 2..6, black 3..7:
// 15 x 8 files = 120 placements; the slice manager folds the mirror on top.
struct Pair_Group
{
	using Index = uint32_t;

	Pair_Group()
	{
		m_inverse.fill(-1);
		size_t n = 0;
		// Layout: file-major; within a file, ascending white rank then black rank.
		for (int f = 0; f < 8; ++f)
		{
			const File file = static_cast<File>(f);
			for (int wr = static_cast<int>(RANK_2); wr <= static_cast<int>(RANK_6); ++wr)
				for (int br = wr + 1; br <= static_cast<int>(RANK_7); ++br)
				{
					const Square w = sq_make(static_cast<Rank>(wr), file);
					const Square b = sq_make(static_cast<Rank>(br), file);
					m_inverse[inv_key(w, b)] = static_cast<int32_t>(n);
					m_white[n] = w;
					m_black[n++] = b;
				}
		}
	}

	static constexpr size_t PLACEMENT_COUNT = 120;

	NODISCARD static constexpr size_t table_size() { return PLACEMENT_COUNT; }

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

	// Pair pawns look like free ones, so the pair is the minimal one by (file,
	// white rank, black rank): on the first file holding one, the lowest white
	// pawn with a black one above, then the lowest such black pawn. False if none.
	static bool canonical_pair(Bitboard white, Bitboard black, Square& out_white, Square& out_black)
	{
		for (File f = FILE_A; f < FILE_END; f = static_cast<File>(f + 1))
		{
			const Bitboard w = white & f, b = black & f;
			if (w && b && w.peek_first_square() < b.peek_last_square())
			{
				out_white = w.peek_first_square();
				out_black = Bitboard(b.bits() & (~uint64_t{0} << out_white)).peek_first_square();
				return true;
			}
		}
		return false;
	}

private:
	NODISCARD static int inv_key(Square w, Square b)
	{
		return static_cast<int>(w) * 64 + static_cast<int>(b);
	}

	std::array<Square, PLACEMENT_COUNT> m_white;
	std::array<Square, PLACEMENT_COUNT> m_black;
	std::array<int32_t, 64 * 64> m_inverse;
};
