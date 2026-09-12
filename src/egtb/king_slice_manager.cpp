#include "egtb/king_slice_manager.h"

#include "chess/chess.h"
#include "chess/attack.h"
#include "chess/bitboard.h"

#include "util/fixed_vector.h"

#include <algorithm>

namespace {

bool sq_on_main_diag(Square sq)
{
	return static_cast<int>(sq_file(sq)) == static_cast<int>(sq_rank(sq));
}

bool kings_adjacent(Square a, Square b)
{
	return king_attacks(a).has_square(b);
}

Const_Span<Square> anchor_legal_squares(Symmetry_Group sym)
{
	switch (sym)
	{
		case Symmetry_Group::DIHEDRAL_8:
			return Const_Span<Square>(ANCHOR_TRIANGLE_SQUARES, std::size(ANCHOR_TRIANGLE_SQUARES));
		case Symmetry_Group::FILE_MIRROR:
			return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES, std::size(ANCHOR_FILE_MIRROR_SQUARES));
		case Symmetry_Group::NONE:
			break;
	}
	ASSUME(false);
	return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES, size_t(0));
}

bool is_anchor_legal(Symmetry_Group sym, Square wk)
{
	const auto sqs = anchor_legal_squares(sym);
	for (size_t i = 0; i < sqs.size(); ++i)
		if (sqs[i] == wk) return true;
	return false;
}

int num_transforms(Symmetry_Group sym)
{
	switch (sym)
	{
		case Symmetry_Group::DIHEDRAL_8:  return 8;
		case Symmetry_Group::FILE_MIRROR: return 2;
		case Symmetry_Group::NONE:        break;
	}
	ASSUME(false);
	return 1;
}

}  // namespace

King_Slice_Manager::King_Slice_Manager(Symmetry_Group s,
                                       size_t white_rights, size_t black_rights) :
	sym(s), has_castling(true), castling_rights{ white_rights, black_rights },
	m_castling_group(std::make_unique<Castling_Group>(white_rights, black_rights))
{
	ASSERT(s == Symmetry_Group::NONE);
	m_free_king_stride = m_castling_group->both_kings_pinned() ? size_t(1) : size_t(SQUARE_NB);
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, Symmetry_Transform::IDENTITY, 0 };
	build_castling_slices();
	build_castling_neighbor_table();
}

void King_Slice_Manager::build_castling_slices()
{
	const size_t n = m_castling_group->table_size();
	castling_slice_lookup.assign(n * m_free_king_stride, SLICE_NONE);

	for (size_t ci = 0; ci < n; ++ci)
	{
		const Castling_Group::Index cidx = static_cast<Castling_Group::Index>(ci);

		Slice_Rooks rooks{{ { SQ_END, SQ_END }, { SQ_END, SQ_END } }};
		Square pinned_king[COLOR_NB] = { SQ_END, SQ_END };
		Bitboard pinned = Bitboard::make_empty();
		for (const Color c : { WHITE, BLACK })
		{
			if (castling_rights[c] == 0) continue;
			pinned_king[c] = m_castling_group->king_square(cidx, c);
			pinned |= square_bb(pinned_king[c]);
			for (size_t i = 0; i < castling_rights[c]; ++i)
			{
				rooks[c][i] = m_castling_group->rook_square(cidx, c, i);
				pinned |= square_bb(rooks[c][i]);
			}
		}

		auto emit = [&](Square wk, Square bk, size_t free_slot) {
			const int32_t sid = static_cast<int32_t>(kings_of_slice.size());
			kings_of_slice.emplace_back(wk, bk);
			castling_rooks_of_slice.push_back(rooks);
			castling_index_of_slice.push_back(cidx);
			slice_has_stabilizer.push_back(0);
			castling_slice_lookup[ci * m_free_king_stride + free_slot] = sid;
		};

		if (m_castling_group->both_kings_pinned())
		{
			emit(pinned_king[WHITE], pinned_king[BLACK], 0);
			continue;
		}

		const Color pinned_color = (castling_rights[WHITE] > 0) ? WHITE : BLACK;
		const Square ksq = pinned_king[pinned_color];
		for (Square fk = SQ_A1; fk < SQ_END; fk = static_cast<Square>(fk + 1))
		{
			if (pinned.has_square(fk)) continue;
			if (kings_adjacent(ksq, fk)) continue;
			emit(pinned_color == WHITE ? ksq : fk,
			     pinned_color == WHITE ? fk : ksq,
			     static_cast<size_t>(fk));
		}
	}
	num_slices = kings_of_slice.size();
}

void King_Slice_Manager::build_castling_neighbor_table()
{
	m_neighbor_off.assign(num_slices + 1, 0);
	m_neighbor_data.clear();
	if (m_castling_group->both_kings_pinned())
		return;
	m_neighbor_data.reserve(num_slices * 8);

	const Color free_color = (castling_rights[WHITE] > 0) ? BLACK : WHITE;
	Fixed_Vector<int32_t, 16> out;
	for (size_t sid = 0; sid < num_slices; ++sid)
	{
		out.clear();
		const auto [wk, bk] = kings_of_slice[sid];
		const Square fk = (free_color == WHITE) ? wk : bk;
		const int32_t self = static_cast<int32_t>(sid);

		Bitboard pres = king_attacks(fk);
		while (pres)
		{
			const Square s = pres.pop_first_square();
			const int32_t n = castling_slice_with_free_king(self, s);
			if (n != SLICE_NONE && n != self) out.push_back(n);
		}

		std::sort(out.begin(), out.end());
		const auto last = std::unique(out.begin(), out.end());
		m_neighbor_data.insert(m_neighbor_data.end(), out.begin(), last);
		m_neighbor_off[sid + 1] = static_cast<uint32_t>(m_neighbor_data.size());
	}
}

King_Slice_Manager::King_Slice_Manager(Symmetry_Group s) : sym(s)
{
	ASSERT(s != Symmetry_Group::NONE);
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

	build_neighbor_table();
}

void King_Slice_Manager::build_neighbor_table()
{
	m_neighbor_off.assign(num_slices + 1, 0);
	m_neighbor_data.clear();
	m_neighbor_data.reserve(num_slices * 8);

	// Bound: 8 king-attack squares per color, two colors.
	Fixed_Vector<int32_t, 16> out;
	for (size_t sid = 0; sid < num_slices; ++sid)
	{
		out.clear();
		const auto [wk, bk] = kings_of_slice[sid];
		const int32_t self = static_cast<int32_t>(sid);

		// One king steps, the other stays: a predecessor sits on any king-attack
		// square of the mover.
		const Bitboard wk_pres = king_attacks(wk);
		for (Square s = SQ_A1; s < SQ_END; s = static_cast<Square>(s + 1))
		{
			if (!wk_pres.has_square(s)) continue;
			const int32_t n = lookup(s, bk).slice_id;
			if (n != SLICE_NONE && n != self) out.push_back(n);
		}
		const Bitboard bk_pres = king_attacks(bk);
		for (Square s = SQ_A1; s < SQ_END; s = static_cast<Square>(s + 1))
		{
			if (!bk_pres.has_square(s)) continue;
			const int32_t n = lookup(wk, s).slice_id;
			if (n != SLICE_NONE && n != self) out.push_back(n);
		}

		std::sort(out.begin(), out.end());
		const auto last = std::unique(out.begin(), out.end());
		m_neighbor_data.insert(m_neighbor_data.end(), out.begin(), last);
		m_neighbor_off[sid + 1] = static_cast<uint32_t>(m_neighbor_data.size());
	}
}
