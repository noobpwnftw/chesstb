#pragma once

#include "chess/bitboard.h"
#include "chess/chess.h"

#include "util/defines.h"

#include <array>
#include <vector>

// Joint placement of the men standing rights pin: each unmoved king and the
// rooks it could still castle with. 56 per rights-holding side, the joint table
// their product. The enumeration order is baked into stored king-slice ids.
struct Castling_Group
{
	using Index = int32_t;
	static constexpr Index INDEX_NONE = -1;
	static constexpr size_t MAX_RIGHTS = 2;
	// A side's castling rooks as a mask over the files of its home rank.
	static constexpr size_t ROOK_MASKS = size_t(1) << FILE_NB;

	// One side's pinned men on its home rank: its king's file, and its castling
	// rooks as a mask of files.
	struct Side_Placement
	{
		File king = FILE_END;
		uint8_t rooks = 0;
	};

	Castling_Group(size_t white_rights, size_t black_rights) :
		m_rights{ white_rights, black_rights }
	{
		ASSERT(white_rights <= MAX_RIGHTS && black_rights <= MAX_RIGHTS);
		ASSERT(white_rights + black_rights > 0);
		for (const Color c : { WHITE, BLACK })
			m_side[c].build(m_rights[c]);
	}

	NODISCARD size_t rights(Color c) const { return m_rights[c]; }
	NODISCARD bool both_kings_pinned() const
	{
		return m_rights[WHITE] > 0 && m_rights[BLACK] > 0;
	}

	NODISCARD size_t table_size() const
	{
		return std::max<size_t>(m_side[WHITE].size(), 1)
		     * std::max<size_t>(m_side[BLACK].size(), 1);
	}

	// Index order: white's placement outermost, black's within it. Nothing
	// depends on the order beyond it being stable -- but it must stay stable,
	// since stored king-slice ids are built from it.
	NODISCARD const Side_Placement& side(Index idx, Color c) const
	{
		ASSERT(idx >= 0 && static_cast<size_t>(idx) < table_size());
		ASSERT(m_rights[c] > 0);
		const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
		const size_t within = (c == WHITE)
			? static_cast<size_t>(idx) / b_span
			: static_cast<size_t>(idx) % b_span;
		return m_side[c].placements[within];
	}

	// The placement of each side's king file and rook mask, INDEX_NONE if they
	// are not one; a side without rights is ignored.
	NODISCARD Index index_of(File white_king, uint8_t white_rooks,
	                         File black_king, uint8_t black_rooks) const
	{
		const File king[COLOR_NB] = { white_king, black_king };
		const uint8_t rooks[COLOR_NB] = { white_rooks, black_rooks };

		size_t within[COLOR_NB] = { 0, 0 };
		for (const Color c : { WHITE, BLACK })
		{
			if (m_rights[c] == 0) continue;
			const int32_t i = m_side[c].inverse[king[c] * ROOK_MASKS + rooks[c]];
			if (i < 0) return INDEX_NONE;
			within[c] = static_cast<size_t>(i);
		}
		const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
		return static_cast<Index>(within[WHITE] * b_span + within[BLACK]);
	}

	NODISCARD Square king_square(Index idx, Color c) const
	{
		return sq_make(castling_home_rank(c), side(idx, c).king);
	}

	// The castling rooks' squares.
	NODISCARD Bitboard rooks(Index idx, Color c) const
	{
		return Bitboard(uint64_t{ side(idx, c).rooks } << (8 * castling_home_rank(c)));
	}

private:
	// One side's placements, enumerated king file first then rook files, all
	// ascending, and their index by king file * ROOK_MASKS + rook mask. Empty for a
	// side holding no rights.
	struct Side_Table
	{
		std::vector<Side_Placement> placements;
		std::array<int32_t, FILE_NB * ROOK_MASKS> inverse{};

		NODISCARD size_t size() const { return placements.size(); }

		void build(size_t num_rights)
		{
			inverse.fill(INDEX_NONE);
			if (num_rights == 0) return;

			for (File k = FILE_A; k < FILE_END; ++k)
			{
				if (num_rights == 1)
				{
					for (File r = FILE_A; r < FILE_END; ++r)
						if (r != k)
							add({ k, static_cast<uint8_t>(1 << r) });
				}
				else
				{
					for (File lo = FILE_A; lo < k; ++lo)
						for (File hi = static_cast<File>(k + 1); hi < FILE_END; ++hi)
							add({ k, static_cast<uint8_t>((1 << lo) | (1 << hi)) });
				}
			}
		}

		void add(const Side_Placement& p)
		{
			inverse[p.king * ROOK_MASKS + p.rooks] = static_cast<int32_t>(placements.size());
			placements.push_back(p);
		}
	};

	std::array<size_t, COLOR_NB> m_rights;
	Side_Table m_side[COLOR_NB];
};
