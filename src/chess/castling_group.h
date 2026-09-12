#pragma once

#include "chess/chess.h"

#include "util/defines.h"
#include "util/span.h"

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

	// One side's pinned men: its king's file, and its castling rooks' files
	// ascending. Only the first `rights(c)` rook entries are meaningful.
	struct Side_Placement
	{
		File king = FILE_END;
		std::array<File, MAX_RIGHTS> rook{ FILE_END, FILE_END };
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
	NODISCARD bool holds_rights(Color c) const { return m_rights[c] > 0; }
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
		ASSERT(holds_rights(c));
		const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
		const size_t within = (c == WHITE)
			? static_cast<size_t>(idx) / b_span
			: static_cast<size_t>(idx) % b_span;
		return m_side[c].placements[within];
	}

	NODISCARD Index index_of(File white_king, Const_Span<File> white_rooks,
	                         File black_king, Const_Span<File> black_rooks) const
	{
		const File king[COLOR_NB] = { white_king, black_king };
		const Const_Span<File>* rooks[COLOR_NB] = { &white_rooks, &black_rooks };

		size_t within[COLOR_NB] = { 0, 0 };
		for (const Color c : { WHITE, BLACK })
		{
			if (m_rights[c] == 0) continue;
			if (rooks[c]->size() != m_rights[c]) return INDEX_NONE;
			const int32_t i = m_side[c].index_of(make_side_placement(king[c], *rooks[c]));
			if (i < 0) return INDEX_NONE;
			within[c] = static_cast<size_t>(i);
		}
		const size_t b_span = std::max<size_t>(m_side[BLACK].size(), 1);
		return static_cast<Index>(within[WHITE] * b_span + within[BLACK]);
	}

	NODISCARD static Side_Placement make_side_placement(File king_file,
	                                                    Const_Span<File> rook_files)
	{
		Side_Placement p;
		p.king = king_file;
		for (size_t i = 0; i < rook_files.size() && i < MAX_RIGHTS; ++i)
			p.rook[i] = rook_files[i];
		if (p.rook[1] != FILE_END && p.rook[1] < p.rook[0])
			std::swap(p.rook[0], p.rook[1]);
		return p;
	}

	NODISCARD Square king_square(Index idx, Color c) const
	{
		return sq_make(castling_home_rank(c), side(idx, c).king);
	}

	NODISCARD Square rook_square(Index idx, Color c, size_t which) const
	{
		ASSERT(which < m_rights[c]);
		return sq_make(castling_home_rank(c), side(idx, c).rook[which]);
	}

	// Kingside in the FEN sense: this rook stands toward the h-file.
	NODISCARD bool h_side(Index idx, Color c, size_t which) const
	{
		const Side_Placement& p = side(idx, c);
		return p.rook[which] > p.king;
	}

private:
	// One side's placements, enumerated king file first then rook files, all
	// ascending. Empty for a side holding no rights.
	struct Side_Table
	{
		std::vector<Side_Placement> placements;
		std::array<int32_t, 9 * 9 * 9> inverse{};

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
							add({ k, { r, FILE_END } });
				}
				else
				{
					for (File lo = FILE_A; lo < k; ++lo)
						for (File hi = static_cast<File>(k + 1); hi < FILE_END; ++hi)
							add({ k, { lo, hi } });
				}
			}
		}

		NODISCARD int32_t index_of(const Side_Placement& p) const
		{
			return inverse[key(p)];
		}

	private:
		// (king, rook, rook) packed base-9 over FILE_A..FILE_END. FILE_END is the
		// absent second rook of a one-right side, and keys a slot build never
		// fills, so an unplaced man reads back as INDEX_NONE.
		NODISCARD static size_t key(const Side_Placement& p)
		{
			auto f = [](File x) {
				ASSERT(x >= FILE_A && x <= FILE_END);
				return static_cast<size_t>(x == FILE_END ? 8 : x);
			};
			return (f(p.king) * 9 + f(p.rook[0])) * 9 + f(p.rook[1]);
		}

		void add(const Side_Placement& p)
		{
			inverse[key(p)] = static_cast<int32_t>(placements.size());
			placements.push_back(p);
		}
	};

	std::array<size_t, COLOR_NB> m_rights;
	Side_Table m_side[COLOR_NB];
};
