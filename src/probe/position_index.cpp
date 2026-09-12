#include "probe/position_index.h"

#include "chess/attack.h"
#include "chess/bitboard.h"
#include "chess/chess.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/math.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::array<std::array<uint32_t, 8>, 65> BINOMIAL = []() {
	std::array<std::array<uint32_t, 8>, 65> b{};
	for (size_t k = 0; k <= 64; ++k)
	{
		b[k][0] = 1;
		for (size_t n = 1; n <= 7; ++n)
			b[k][n] = (n > k) ? 0 : b[k - 1][n - 1] + b[k - 1][n];
	}
	return b;
}();

constexpr Square ANCHOR_FILE_MIRROR_SQUARES[] = {
	SQ_A1, SQ_B1, SQ_C1, SQ_D1,
	SQ_A2, SQ_B2, SQ_C2, SQ_D2,
	SQ_A3, SQ_B3, SQ_C3, SQ_D3,
	SQ_A4, SQ_B4, SQ_C4, SQ_D4,
	SQ_A5, SQ_B5, SQ_C5, SQ_D5,
	SQ_A6, SQ_B6, SQ_C6, SQ_D6,
	SQ_A7, SQ_B7, SQ_C7, SQ_D7,
	SQ_A8, SQ_B8, SQ_C8, SQ_D8,
};

constexpr Square ANCHOR_TRIANGLE_SQUARES[] = {
	SQ_A1, SQ_B1, SQ_C1, SQ_D1,
	SQ_B2, SQ_C2, SQ_D2,
	SQ_C3, SQ_D3,
	SQ_D4,
};

NODISCARD Square apply_transform(Square sq, Symmetry_Transform t)
{
	int f = sq_file(sq);
	int r = sq_rank(sq);
	const uint8_t bits = static_cast<uint8_t>(t);
	if (bits & 1) f = 7 - f;
	if (bits & 2) r = 7 - r;
	if (bits & 4) std::swap(f, r);
	return sq_make(static_cast<Rank>(r), static_cast<File>(f));
}

Square apply_diag_transform(Square sq)
{
	return apply_transform(sq, Symmetry_Transform::DIAG);
}

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
			return Const_Span<Square>(ANCHOR_TRIANGLE_SQUARES,
			                          std::size(ANCHOR_TRIANGLE_SQUARES));
		case Symmetry_Group::FILE_MIRROR:
			return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES,
			                          std::size(ANCHOR_FILE_MIRROR_SQUARES));
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

const King_Slice_Manager& slice_mgr_for(Symmetry_Group sym)
{
	static const King_Slice_Manager file_mirror(Symmetry_Group::FILE_MIRROR);
	static const King_Slice_Manager dihedral_8(Symmetry_Group::DIHEDRAL_8);
	ASSERT(sym != Symmetry_Group::NONE);
	return sym == Symmetry_Group::DIHEDRAL_8 ? dihedral_8 : file_mirror;
}

NODISCARD const King_Slice_Manager& castling_slice_mgr(size_t white_rights,
                                                       size_t black_rights)
{
	constexpr size_t STRIDE = Castling_Group::MAX_RIGHTS + 1;
	static const auto table = [] {
		std::array<std::unique_ptr<const King_Slice_Manager>, STRIDE * STRIDE> t;
		for (size_t w = 0; w < STRIDE; ++w)
			for (size_t b = 0; b < STRIDE; ++b)
				if (w + b > 0)
					t[w * STRIDE + b] = std::make_unique<King_Slice_Manager>(
						Symmetry_Group::NONE, w, b);
		return t;
	}();
	const auto& mgr = table[white_rights * STRIDE + black_rights];
	ASSERT(mgr != nullptr);
	return *mgr;
}

void apply_transform_to_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Position_Index_Config& cfg,
	Symmetry_Transform t)
{
	if (t == Symmetry_Transform::IDENTITY) return;

	auto xform = [t](Square s) { return apply_transform(s, t); };

	(*placements)[WHITE_KINGS] = (*placements)[WHITE_KINGS].with_transformed_squares(xform);
	(*placements)[BLACK_KINGS] = (*placements)[BLACK_KINGS].with_transformed_squares(xform);

	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
		if ((*placements)[c].size() > 0)
			(*placements)[c] = (*placements)[c].with_transformed_squares(xform);

	for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
	{
		const Piece_Class c = cfg.populated_classes()[i];
		(*placements)[c] = (*placements)[c].with_transformed_squares(xform);
	}
}

void canonicalize_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Position_Index_Config& cfg)
{
	ASSERT(!cfg.king_slice_manager().has_castling);

	const Square wk = (*placements)[WHITE_KINGS][0];
	const Square bk = (*placements)[BLACK_KINGS][0];
	const auto& lookup = cfg.king_slice_manager().lookup(wk, bk);

	if (lookup.slice_id == SLICE_NONE) return;

	apply_transform_to_placements(placements, cfg, lookup.transform);

	if (lookup.has_diag_stabilizer)
	{
		size_t within_cur = 0;
		size_t within_alt = 0;
		for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
		{
			const Piece_Class c = cfg.populated_classes()[i];
			const auto& cur = (*placements)[c];
			const auto alt = cur.with_transformed_squares(apply_diag_transform);
			within_cur += cfg.weight(c) * cfg.group(c).compound_index(cur);
			within_alt += cfg.weight(c) * cfg.group(c).compound_index(alt);
		}

		if (within_alt < within_cur)
		{
			for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
			{
				const Piece_Class c = cfg.populated_classes()[i];
				(*placements)[c] =
					(*placements)[c].with_transformed_squares(apply_diag_transform);
			}
		}
	}
}

std::array<Piece_Group::Placement, PIECE_CLASS_NB>
placements_from_position(const Position_Index_Config& cfg, const Position& pos)
{
	std::array<Piece_Group::Placement, PIECE_CLASS_NB> out{};

	out[WHITE_KINGS].add(pos.king_square(WHITE));
	out[BLACK_KINGS].add(pos.king_square(BLACK));

	for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
	{
		const Piece_Class c = cfg.populated_classes()[i];
		const Piece pc = cfg.group(c).piece();
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}

	const bool has_pair = cfg.pair_group() != nullptr;
	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
	{
		if (!cfg.is_populated(c) && !has_pair) continue;
		const Piece pc = (c == WHITE_PAWNS) ? WHITE_PAWN : BLACK_PAWN;
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}
	return out;
}

}  // namespace

Piece_Group::Piece_Group(Piece pc, size_t count) :
	m_piece(pc),
	m_num_pieces(count)
{
	std::vector<Square> ls;
	const size_t n = possible_sq_nb(pc);
	ls.reserve(n);
	for (size_t i = 0; i < n; ++i)
		ls.push_back(possible_sq(pc, i));
	std::sort(ls.begin(), ls.end());
	ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
	m_num_legal_squares = ls.size();

	std::fill(std::begin(m_sq_to_pos), std::end(m_sq_to_pos), int8_t(-1));
	for (size_t i = 0; i < m_num_legal_squares; ++i)
	{
		m_pos_to_sq[i] = ls[i];
		m_sq_to_pos[ls[i]] = static_cast<int8_t>(i);
	}

	m_table_size = BINOMIAL[m_num_legal_squares][count];
}

Piece_Group::Placement_Index
Piece_Group::compound_index(const Placement& list) const
{
	ASSERT(list.size() == m_num_pieces);
	uint32_t rank = 0;
	for (size_t i = 0; i < m_num_pieces; ++i)
	{
		const int p = m_sq_to_pos[list[i]];
		ASSERT(p >= 0);
		rank += BINOMIAL[static_cast<size_t>(p)][i + 1];
	}
	ASSERT(rank < m_table_size);
	return static_cast<Placement_Index>(rank);
}

Piece_Group::Placement Piece_Group::squares(Placement_Index idx) const
{
	ASSERT(idx < m_table_size);

	size_t pos[MAX_PIECE_GROUP_SIZE]{};
	uint32_t rank = idx;
	size_t hi = m_num_legal_squares;
	for (size_t k = m_num_pieces; k > 0; --k)
	{
		size_t p = hi - 1;
		while (BINOMIAL[p][k] > rank)
		{
			ASSERT(p > 0);
			--p;
		}
		pos[k - 1] = p;
		rank -= BINOMIAL[p][k];
		hi = p;
	}

	Placement pl;
	for (size_t i = 0; i < m_num_pieces; ++i)
		pl.add_unsorted(m_pos_to_sq[pos[i]]);
	return pl;
}

// Must enumerate exactly as the generator's copy does: the slice ids are stored.
King_Slice_Manager::King_Slice_Manager(Symmetry_Group s,
                                       size_t white_rights, size_t black_rights) :
	sym(s), has_castling(true), castling_rights{ white_rights, black_rights },
	m_castling_group(std::make_shared<Castling_Group>(white_rights, black_rights))
{
	ASSERT(s == Symmetry_Group::NONE);
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, Symmetry_Transform::IDENTITY, 0 };

	m_free_king_stride = m_castling_group->both_kings_pinned() ? size_t(1) : size_t(SQUARE_NB);
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

King_Slice_Manager::King_Slice_Manager(Symmetry_Group s) : sym(s)
{
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, Symmetry_Transform::IDENTITY, 0 };

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
				const auto t = static_cast<Symmetry_Transform>(t_id);
				const Square wk_t = apply_transform(wk, t);
				const Square bk_t = apply_transform(bk, t);
				const auto& look = pair_lookup[static_cast<int>(wk_t) * SQUARE_NB
				                              + static_cast<int>(bk_t)];
				if (look.slice_id != SLICE_NONE
				    && look.transform == Symmetry_Transform::IDENTITY)
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

Pawn_Slice_Manager::Pawn_Slice_Manager(
	const Pair_Group* pair,
	const Piece_Group* white_pawns,
	const Piece_Group* black_pawns)
{
	m_has_pawns = pair != nullptr || white_pawns != nullptr || black_pawns != nullptr;
	m_pair_group = pair;
	m_white_group = white_pawns;
	m_black_group = black_pawns;
	m_pair_table_size  = pair ? pair->table_size() : 1;
	m_white_table_size = white_pawns ? white_pawns->table_size() : 1;
	m_black_table_size = black_pawns ? black_pawns->table_size() : 1;
	m_num_cartesian_slices = m_pair_table_size * m_white_table_size * m_black_table_size;
	if (m_pair_table_size > 1)
		m_pair_table_size_div = Divider<uint64_t>(m_pair_table_size);
	if (m_white_table_size > 1)
		m_white_table_size_div = Divider<uint64_t>(m_white_table_size);

	auto placements_of = [](const Piece_Group* g,
	                        std::vector<Piece_Group::Placement>& out_pl,
	                        std::vector<Bitboard>& out_occ) {
		if (g == nullptr)
		{
			out_pl.emplace_back();
			out_occ.push_back(Bitboard::make_empty());
			return;
		}
		out_pl.reserve(g->table_size());
		out_occ.reserve(g->table_size());
		for (size_t i = 0; i < g->table_size(); ++i)
		{
			out_pl.push_back(g->squares(static_cast<Piece_Group::Placement_Index>(i)));
			Bitboard occ = Bitboard::make_empty();
			const auto& pl = out_pl.back();
			for (size_t k = 0; k < pl.size(); ++k) occ |= square_bb(pl[k]);
			out_occ.push_back(occ);
		}
	};
	std::vector<Piece_Group::Placement> white_pl, black_pl;
	std::vector<Bitboard> white_occ, black_occ, pair_occ;
	placements_of(m_white_group, white_pl, white_occ);
	placements_of(m_black_group, black_pl, black_occ);
	if (m_pair_group)
		for (size_t i = 0; i < m_pair_table_size; ++i)
		{
			const auto pi = static_cast<Pair_Group::Index>(i);
			pair_occ.push_back(square_bb(m_pair_group->white_square(pi))
			                 | square_bb(m_pair_group->black_square(pi)));
		}
	else
		pair_occ.push_back(Bitboard::make_empty());

	// The designated pair of a cell must be the canonical opposing pair over all
	// its pawns, or the cell is a duplicate of the one that designates that pair.
	auto pair_is_canonical = [&](size_t p, size_t w, size_t b) -> bool {
		const auto pi = static_cast<Pair_Group::Index>(p);
		const Square pair_w = m_pair_group->white_square(pi);
		const Square pair_b = m_pair_group->black_square(pi);
		Square wsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
		Square bsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
		size_t nw = 0, nb = 0;
		wsq[nw++] = pair_w;
		bsq[nb++] = pair_b;
		for (size_t k = 0; k < white_pl[w].size(); ++k) wsq[nw++] = white_pl[w][k];
		for (size_t k = 0; k < black_pl[b].size(); ++k) bsq[nb++] = black_pl[b][k];
		Square cw = SQ_END, cb = SQ_END;
		Pair_Group::canonical_pair(Const_Span<Square>(wsq, nw),
		                           Const_Span<Square>(bsq, nb), cw, cb);
		return cw == pair_w && cb == pair_b;
	};

	// Mark the cells that earn a storage id, skipping overlap (unreachable) cells
	// and cells whose designated pair is not the canonical one. Storage ids are
	// ranks in ascending cartesian order, so the walk has to follow that order:
	// the pair digit is least significant, then white, then black.
	m_rank_blocks.assign((m_num_cartesian_slices + 63) / 64, Rank_Block{});
	m_cartesian_by_storage.clear();
	if (m_pair_group == nullptr && m_white_group != nullptr && m_black_group != nullptr)
		m_cartesian_by_storage.reserve(
			m_black_table_size * BINOMIAL[possible_sq_nb(WHITE_PAWN) - m_black_group->size()]
			                             [m_white_group->size()]);
	else
		m_cartesian_by_storage.reserve(m_num_cartesian_slices);

	size_t cart = 0;
	for (size_t b = 0; b < m_black_table_size; ++b)
	{
		for (size_t w = 0; w < m_white_table_size; ++w)
		{
			if (white_occ[w] & black_occ[b])
			{
				cart += m_pair_table_size;
				continue;
			}
			const Bitboard free_occ = white_occ[w] | black_occ[b];
			for (size_t p = 0; p < m_pair_table_size; ++p, ++cart)
			{
				if (pair_occ[p] & free_occ) continue;
				if (m_pair_group && !pair_is_canonical(p, w, b)) continue;
				m_rank_blocks[cart >> 6].bits |= uint64_t{1} << (cart & 63);
				m_cartesian_by_storage.push_back(static_cast<int32_t>(cart));
			}
		}
	}
	ASSERT(cart == m_num_cartesian_slices);
	m_num_slices = m_cartesian_by_storage.size();

	int32_t running = 0;
	for (Rank_Block& blk : m_rank_blocks)
	{
		blk.survivors_before = running;
		running += static_cast<int32_t>(popcnt(blk.bits));
	}
	ASSERT(static_cast<size_t>(running) == m_num_slices);
}

Pawn_Slice_Manager::Decomposed
Pawn_Slice_Manager::decompose(int32_t slice_id) const
{
	ASSERT(slice_id >= 0 && static_cast<size_t>(slice_id) < m_num_slices);
	Decomposed d;
	if (!m_has_pawns) { d.pair_idx = 0; d.white_idx = 0; d.black_idx = 0; return d; }
	size_t rem = static_cast<size_t>(m_cartesian_by_storage[slice_id]);
	if (m_pair_table_size > 1)
	{
		const size_t q = rem / m_pair_table_size_div;
		d.pair_idx = static_cast<Pair_Group::Index>(rem - q * m_pair_table_size);
		rem = q;
	}
	else
	{
		d.pair_idx = 0;
	}
	if (m_white_table_size > 1)
	{
		const size_t q = rem / m_white_table_size_div;
		d.white_idx = static_cast<Piece_Group::Placement_Index>(rem - q * m_white_table_size);
		rem = q;
	}
	else
	{
		d.white_idx = 0;
	}
	d.black_idx = static_cast<Piece_Group::Placement_Index>(rem);
	return d;
}

int32_t Pawn_Slice_Manager::compose(
	Pair_Group::Index pair,
	Piece_Group::Placement_Index w,
	Piece_Group::Placement_Index b) const
{
	if (!m_has_pawns) return 0;
	const size_t cart = static_cast<size_t>(pair)
	                  + static_cast<size_t>(w) * m_pair_table_size
	                  + static_cast<size_t>(b) * m_pair_table_size * m_white_table_size;
	ASSERT(cart < m_num_cartesian_slices);
	ASSERT(cartesian_survives(cart));
	return storage_of_cartesian(cart);
}

int32_t Pawn_Slice_Manager::lookup_from_squares(
	Square pair_white_sq, Square pair_black_sq,
	Const_Span<Square> white_pawn_squares,
	Const_Span<Square> black_pawn_squares) const
{
	if (!m_has_pawns) return 0;

	const Pair_Group::Index pair_idx =
		m_pair_group ? m_pair_group->index_of(pair_white_sq, pair_black_sq) : 0;

	Piece_Group::Placement_Index w_idx = 0;
	Piece_Group::Placement_Index b_idx = 0;

	if (m_white_group)
	{
		Piece_Group::Placement pl;
		for (size_t i = 0; i < white_pawn_squares.size(); ++i)
			pl.add(white_pawn_squares[i]);
		w_idx = m_white_group->compound_index(pl);
	}
	if (m_black_group)
	{
		Piece_Group::Placement pl;
		for (size_t i = 0; i < black_pawn_squares.size(); ++i)
			pl.add(black_pawn_squares[i]);
		b_idx = m_black_group->compound_index(pl);
	}
	return compose(pair_idx, w_idx, b_idx);
}

bool Position_Index_Config::try_init()
{
	const auto counts = piece_counts();

	const size_t total_pawns = counts[WHITE_PAWN] + counts[BLACK_PAWN]
	                         + (has_opposing_pair() ? 2 : 0);
	if (total_pawns > MAX_TOTAL_PAWNS)
		return false;

	for (size_t i = 0; i < PIECE_NB; ++i)
		m_piece_counts_cached[i] = static_cast<int8_t>(counts[i]);
	const Symmetry_Group symmetry = pick_symmetry(*this);
	ASSERT(has_castling() == (symmetry == Symmetry_Group::NONE));
	m_king_slice_manager = has_castling()
		? &castling_slice_mgr(castling_rights(WHITE), castling_rights(BLACK))
	                                    : &slice_mgr_for(symmetry);

	for (size_t i = 0; i < PIECE_CLASS_NB; ++i)
	{
		m_groups_owned[i].reset();
		m_groups[i] = nullptr;
	}

	auto make_group = [&](Piece_Class pclass, Piece pc, size_t count) {
		m_groups_owned[pclass] = std::make_unique<Piece_Group>(pc, count);
		m_groups[pclass] = m_groups_owned[pclass].get();
	};

	for (Color c : { WHITE, BLACK })
	{
		for (Piece_Type pt : { QUEEN, ROOK, BISHOP, KNIGHT, PAWN })
		{
			const Piece pc = piece_make(c, pt);
			const size_t n = counts[pc];
			if (n == 0) continue;
			const Piece_Type_Class ptcl =
				pt == QUEEN  ? QUEENS  :
				pt == ROOK   ? ROOKS   :
				pt == BISHOP ? BISHOPS :
				pt == KNIGHT ? KNIGHTS :
				                PAWNS;
			const Piece_Class pcl = make_piece_class(c, ptcl);
			make_group(pcl, pc, n);
		}
	}

	if (has_opposing_pair())
	{
		m_pair_group = std::make_unique<Pair_Group>();
		m_piece_counts_cached[WHITE_PAWN] += 1;
		m_piece_counts_cached[BLACK_PAWN] += 1;
	}

	for (const Color c : { WHITE, BLACK })
		m_piece_counts_cached[piece_make(c, ROOK)] += static_cast<int8_t>(castling_rights(c));

	m_pawn_slice_manager = std::make_unique<Pawn_Slice_Manager>(
		pair_group(),
		m_groups[WHITE_PAWNS],
		m_groups[BLACK_PAWNS]);
	m_num_pawn_slices = m_pawn_slice_manager->num_slices();

	m_num_populated_classes = 0;
	size_t w = 1;
	for (Piece_Class i = PIECE_CLASS_START; i < PIECE_CLASS_END; ++i)
	{
		if (m_groups[i] == nullptr) continue;
		if (i == WHITE_PAWNS || i == BLACK_PAWNS) continue;
		m_populated_classes[m_num_populated_classes++] = i;
		m_weights[i] = w;
		const size_t group_size = m_groups[i]->table_size();
		const size_t next_w = w * group_size;
		if (group_size != 0 && next_w / group_size != w)
			return false;
		if (next_w > MAX_NUM_POSITIONS)
			return false;
		w = next_w;
	}
	m_within_slice_size = w;

	const size_t num_king_slices = m_king_slice_manager->num_slices;
	m_pawn_slice_stride = num_king_slices * m_within_slice_size;
	if (m_within_slice_size != 0 && m_pawn_slice_stride / m_within_slice_size != num_king_slices)
		return false;

	const size_t total = m_num_pawn_slices * m_pawn_slice_stride;
	if (m_pawn_slice_stride != 0 && total / m_pawn_slice_stride != m_num_pawn_slices)
		return false;
	if (total > MAX_NUM_POSITIONS)
		return false;
	m_num_positions = total;
	return true;
}

Position_Index_Config::Position_Index_Config(const Piece_Config& ps) :
	Piece_Config(ps)
{
	if (!try_init())
		throw std::runtime_error("Position_Index_Config: configuration too large to materialize");
}

void Position_Index_Config::decompose_board_index(
	Board_Index pos,
	Out_Param<Decomposed_Board_Index> idx) const
{
	size_t p = static_cast<size_t>(pos);
	if (m_num_pawn_slices > 1)
	{
		const size_t q = p / m_pawn_slice_stride;
		idx->pawn_slice_id = static_cast<int32_t>(q);
		p -= q * m_pawn_slice_stride;
	}
	else
	{
		idx->pawn_slice_id = 0;
	}
	if (m_within_slice_size > 1)
	{
		const size_t q = p / m_within_slice_size;
		idx->king_slice_id = static_cast<int32_t>(q);
		p -= q * m_within_slice_size;
	}
	else
	{
		idx->king_slice_id = static_cast<int32_t>(p);
		p = 0;
	}
	size_t within = p;
	idx->within.fill(Piece_Group::Placement_Index(0));
	for (ptrdiff_t i = m_num_populated_classes - 1; i >= 1; --i)
	{
		const Piece_Class ix = m_populated_classes[i];
		ASSERT(m_weights[ix] > 1);
		const size_t group_idx = within / m_weights[ix];
		within -= group_idx * m_weights[ix];
		idx->within[ix] = static_cast<Piece_Group::Placement_Index>(group_idx);
	}
	if (m_num_populated_classes > 0)
	{
		idx->within[m_populated_classes[0]] =
			static_cast<Piece_Group::Placement_Index>(within);
	}
}

bool Position_Index_Config::fill_board(
	const Decomposed_Board_Index& index,
	Out_Param<Position> board) const
{
	std::memset(board->m_pieces, 0, sizeof(board->m_pieces));
	std::memset(board->m_squares, 0, sizeof(board->m_squares));

	const King_Slice_Manager& ksm = king_slice_manager();
	if (index.king_slice_id < 0
	    || static_cast<size_t>(index.king_slice_id) >= ksm.num_slices)
		return false;
	const auto [wk, bk] = ksm.kings_of_slice[index.king_slice_id];

	Bitboard white_occ = Bitboard::make_empty();
	Bitboard black_occ = Bitboard::make_empty();

	const Bitboard wk_bb = square_bb(wk);
	const Bitboard bk_bb = square_bb(bk);
	board->m_squares[wk] = WHITE_KING;
	board->m_squares[bk] = BLACK_KING;
	board->m_pieces[WHITE_KING] = wk_bb;
	board->m_pieces[BLACK_KING] = bk_bb;
	white_occ |= wk_bb;
	black_occ |= bk_bb;

	const Pawn_Slice_Manager& psm = pawn_slice_manager();
	if (psm.has_pawns())
	{
		const auto pd = psm.decompose(index.pawn_slice_id);
		auto place_pawns = [&](Piece_Class c, Piece_Group::Placement_Index idx,
		                       Bitboard& color_occ) -> bool {
			if (!is_populated(c)) return true;
			const Piece_Group& g = group(c);
			const auto& placement = g.squares(idx);
			const Piece pc = g.piece();
			Bitboard bb = Bitboard::make_empty();
			for (size_t k = 0; k < placement.size(); ++k)
			{
				const Square sq = placement[k];
				if (!board->is_empty(sq)) return false;
				board->m_squares[sq] = pc;
				bb |= square_bb(sq);
			}
			board->m_pieces[pc] = bb;
			color_occ |= bb;
			return true;
		};
		if (!place_pawns(WHITE_PAWNS, pd.white_idx, white_occ)) return false;
		if (!place_pawns(BLACK_PAWNS, pd.black_idx, black_occ)) return false;

		if (m_pair_group)
		{
			const Square pw = m_pair_group->white_square(pd.pair_idx);
			const Square pb = m_pair_group->black_square(pd.pair_idx);
			if (!board->is_empty(pw) || !board->is_empty(pb)) return false;
			board->m_squares[pw] = WHITE_PAWN;
			board->m_squares[pb] = BLACK_PAWN;
			const Bitboard pwbb = square_bb(pw);
			const Bitboard pbbb = square_bb(pb);
			board->m_pieces[WHITE_PAWN] |= pwbb;
			board->m_pieces[BLACK_PAWN] |= pbbb;
			white_occ |= pwbb;
			black_occ |= pbbb;
		}
	}

	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Class c = m_populated_classes[i];
		const Piece_Group& g = group(c);
		const auto& placement = g.squares(index.within[c]);
		const Piece pc = g.piece();
		Bitboard bb = Bitboard::make_empty();
		for (size_t k = 0; k < placement.size(); ++k)
		{
			const Square sq = placement[k];
			if (!board->is_empty(sq)) return false;
			board->m_squares[sq] = pc;
			bb |= square_bb(sq);
		}
		board->m_pieces[pc] = bb;
		Bitboard& color_occ = (piece_color(pc) == WHITE) ? white_occ : black_occ;
		color_occ |= bb;
	}

	board->m_castling = NO_CASTLING;

	if (has_castling())
	{
		const auto& ksm = *m_king_slice_manager;
		const Square wk = ksm.kings_of_slice[index.king_slice_id].first;
		const Square bk = ksm.kings_of_slice[index.king_slice_id].second;
		for (const Color cc : { WHITE, BLACK })
		{
			if (castling_rights(cc) == 0) continue;
			const Piece rook = piece_make(cc, ROOK);
			const Square castling_king = (cc == WHITE) ? wk : bk;
			for (const Square rook_sq : castling_rook_squares(index.king_slice_id, cc))
			{
				if (!board->is_empty(rook_sq)) return false;
				const Bitboard rook_bb = square_bb(rook_sq);
				board->m_squares[rook_sq] = rook;
				board->m_pieces[rook] |= rook_bb;
				((cc == WHITE) ? white_occ : black_occ) |= rook_bb;
				board->set_castling_right(cc, sq_file(rook_sq) > sq_file(castling_king),
				                          rook_sq);
			}
		}
	}

	board->m_pieces[WHITE_OCCUPY] = white_occ;
	board->m_pieces[BLACK_OCCUPY] = black_occ;
	board->m_occupied = white_occ | black_occ;
	std::memcpy(board->m_piece_counts, m_piece_counts_cached,
	            sizeof(board->m_piece_counts));
	return true;
}

Board_Index board_index_of_position(
	const Position_Index_Config& cfg,
	const Index_Storage_Layout& layout,
	const Position& pos)
{
	auto placements = placements_from_position(cfg, pos);
	const bool has_castling = cfg.has_castling();
	if (!has_castling)
		canonicalize_placements(inout_param(placements), cfg);

	const Square wk = placements[WHITE_KINGS][0];
	const Square bk = placements[BLACK_KINGS][0];

	Square rooks[COLOR_NB][Castling_Group::MAX_RIGHTS];
	size_t num_rooks[COLOR_NB] = { 0, 0 };
	if (has_castling)
	{
		for (const Color c : { WHITE, BLACK })
		{
			for (const bool h_side : { false, true })
				if (pos.can_castle(c, h_side))
				{
					if (num_rooks[c] == Castling_Group::MAX_RIGHTS) return BOARD_INDEX_NONE;
					rooks[c][num_rooks[c]++] = pos.castling_rook_square(c, h_side);
				}
			if (num_rooks[c] == 2 && rooks[c][1] < rooks[c][0])
				std::swap(rooks[c][0], rooks[c][1]);
			if (num_rooks[c] != cfg.castling_rights(c)) return BOARD_INDEX_NONE;
			if (num_rooks[c] == 0) continue;
			const Piece_Class rook_class = make_piece_class(c, ROOKS);
			if (cfg.is_populated(rook_class))
				for (size_t i = 0; i < num_rooks[c]; ++i)
					placements[rook_class].remove_square(rooks[c][i]);
			else
				placements[rook_class].clear();
		}
	}

	const int32_t king_slice_id = has_castling
		? cfg.king_slice_manager().castling_slice_of(
			wk, bk,
			Const_Span<Square>(rooks[WHITE], num_rooks[WHITE]),
			Const_Span<Square>(rooks[BLACK], num_rooks[BLACK]))
		: cfg.king_slice_manager().lookup(wk, bk).slice_id;
	if (king_slice_id == SLICE_NONE)
		return BOARD_INDEX_NONE;

	Decomposed_Board_Index dix;
	dix.king_slice_id = king_slice_id;

	if (cfg.pawn_slice_manager().has_pawns())
	{
		const auto& w_pl = placements[WHITE_PAWNS];
		const auto& b_pl = placements[BLACK_PAWNS];

		Square pair_w = SQ_END, pair_b = SQ_END;
		Square free_w[Piece_Group::MAX_PIECE_GROUP_SIZE];
		Square free_b[Piece_Group::MAX_PIECE_GROUP_SIZE];
		size_t nfw = 0, nfb = 0;

		if (cfg.pair_group())
		{
			Pair_Group::canonical_pair(
				Const_Span<Square>(w_pl.begin(), w_pl.size()),
				Const_Span<Square>(b_pl.begin(), b_pl.size()), pair_w, pair_b);
			for (size_t i = 0; i < w_pl.size(); ++i)
				if (w_pl[i] != pair_w) free_w[nfw++] = w_pl[i];
			for (size_t i = 0; i < b_pl.size(); ++i)
				if (b_pl[i] != pair_b) free_b[nfb++] = b_pl[i];
		}
		else
		{
			for (size_t i = 0; i < w_pl.size(); ++i) free_w[nfw++] = w_pl[i];
			for (size_t i = 0; i < b_pl.size(); ++i) free_b[nfb++] = b_pl[i];
		}

		dix.pawn_slice_id = cfg.pawn_slice_manager().lookup_from_squares(
			pair_w, pair_b,
			Const_Span<Square>(free_w, nfw),
			Const_Span<Square>(free_b, nfb));
	}
	else
	{
		dix.pawn_slice_id = 0;
	}

	for (size_t i = 0; i < cfg.num_populated_classes(); ++i)
	{
		const Piece_Class c = cfg.populated_classes()[i];
		dix.within[c] = cfg.group(c).compound_index(placements[c]);
	}
	return cfg.compose_board_index(dix, layout);
}

bool position_from_index(
	const Position_Index_Config& cfg,
	Board_Index idx,
	Color turn,
	Out_Param<Position> pos)
{
	Decomposed_Board_Index d;
	cfg.decompose_board_index(idx, out_param(d));

	if (!cfg.fill_board(d, pos))
		return false;
	pos->set_turn(turn);
	return true;
}
