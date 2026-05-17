#include "egtb/king_slice_manager.h"
#include "egtb/symmetry.h"

#include "chess/chess.h"
#include "chess/attack.h"
#include "chess/bitboard.h"

#include <algorithm>

namespace {

INLINE bool sq_on_main_diag(Square sq)
{
	return static_cast<int>(sq_file(sq)) == static_cast<int>(sq_rank(sq));
}

INLINE bool kings_adjacent(Square a, Square b)
{
	return king_attacks(a).has_square(b);
}

INLINE Const_Span<Square> anchor_legal_squares(Symmetry_Group sym)
{
	if (sym == Symmetry_Group::DIHEDRAL_8)
		return Const_Span<Square>(ANCHOR_TRIANGLE_SQUARES, std::size(ANCHOR_TRIANGLE_SQUARES));
	return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES, std::size(ANCHOR_FILE_MIRROR_SQUARES));
}

INLINE bool is_anchor_legal(Symmetry_Group sym, Square wk)
{
	const auto sqs = anchor_legal_squares(sym);
	for (size_t i = 0; i < sqs.size(); ++i)
		if (sqs[i] == wk) return true;
	return false;
}

INLINE int num_transforms(Symmetry_Group sym)
{
	return sym == Symmetry_Group::DIHEDRAL_8 ? 8 : 2;
}

}  // namespace

King_Slice_Manager::King_Slice_Manager(Symmetry_Group s) : sym(s)
{
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, Symmetry_Transform::IDENTITY, 0 };

	// Pass 1: enumerate canonical pairs. WK in fundamental domain, kings not
	// adjacent, and (DIHEDRAL_8 + WK on main diag) tiebreak picks smaller BK
	// vs diag-mirror(BK).
	for (Square wk = SQ_A1; wk < SQ_END; wk = static_cast<Square>(wk + 1))
	{
		if (!is_anchor_legal(sym, wk)) continue;

		for (Square bk = SQ_A1; bk < SQ_END; bk = static_cast<Square>(bk + 1))
		{
			if (bk == wk) continue;
			if (kings_adjacent(wk, bk)) continue;

			if (sym == Symmetry_Group::DIHEDRAL_8 && sq_on_main_diag(wk))
			{
				const Square bk_d = sq_diag_mirror(bk);
				if (bk_d != bk && static_cast<int>(bk) > static_cast<int>(bk_d))
					continue;
			}

			const int32_t sid = static_cast<int32_t>(kings_of_slice.size());
			kings_of_slice.emplace_back(wk, bk);

			const bool stab = (sym == Symmetry_Group::DIHEDRAL_8)
			               && sq_on_main_diag(wk)
			               && sq_on_main_diag(bk);
			slice_has_stabilizer.push_back(stab ? 1 : 0);

			pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)] =
				{ sid, Symmetry_Transform::IDENTITY, static_cast<uint8_t>(stab) };
		}
	}
	num_slices = kings_of_slice.size();

	const int n_trans = num_transforms(sym);
	for (Square wk = SQ_A1; wk < SQ_END; wk = static_cast<Square>(wk + 1))
	{
		for (Square bk = SQ_A1; bk < SQ_END; bk = static_cast<Square>(bk + 1))
		{
			Pair_Lookup& e = pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
			if (e.slice_id != SLICE_NONE) continue;
			if (wk == bk || kings_adjacent(wk, bk)) continue;

			for (int t_id = 0; t_id < n_trans; ++t_id)
			{
				const Symmetry_Transform t = static_cast<Symmetry_Transform>(t_id);
				const Square wk_t = apply_transform(wk, t);
				const Square bk_t = apply_transform(bk, t);
				const auto& look = pair_lookup[static_cast<int>(wk_t) * SQUARE_NB + static_cast<int>(bk_t)];
				if (look.slice_id != SLICE_NONE && look.transform == Symmetry_Transform::IDENTITY)
				{
					e.slice_id = look.slice_id;
					e.transform = t;
					e.has_diag_stabilizer = look.has_diag_stabilizer;
					break;
				}
			}
		}
	}
}

void King_Slice_Manager::neighbors(int32_t slice_id, Neighbor_List& out) const
{
	out.clear();
	if (slice_id < 0 || static_cast<size_t>(slice_id) >= num_slices) return;

	const auto [wk, bk] = kings_of_slice[slice_id];

	// WK moved (BK fixed): WK predecessor is any king-attack square of wk.
	const Bitboard wk_neighbors = king_attacks(wk);
	for (Square wk_pre = SQ_A1; wk_pre < SQ_END; wk_pre = static_cast<Square>(wk_pre + 1))
	{
		if (!wk_neighbors.has_square(wk_pre)) continue;
		const int32_t sid = lookup(wk_pre, bk).slice_id;
		if (sid != SLICE_NONE && sid != slice_id)
			out.push_back(sid);
	}

	const Bitboard bk_neighbors = king_attacks(bk);
	for (Square bk_pre = SQ_A1; bk_pre < SQ_END; bk_pre = static_cast<Square>(bk_pre + 1))
	{
		if (!bk_neighbors.has_square(bk_pre)) continue;
		const int32_t sid = lookup(wk, bk_pre).slice_id;
		if (sid != SLICE_NONE && sid != slice_id)
			out.push_back(sid);
	}

	std::sort(out.begin(), out.end());
	const auto last = std::unique(out.begin(), out.end());
	out.resize(static_cast<size_t>(last - out.begin()));
}
