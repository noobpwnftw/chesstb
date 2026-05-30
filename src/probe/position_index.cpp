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

constexpr std::array<std::array<uint64_t, 8>, 65> BINOMIAL = []() {
	std::array<std::array<uint64_t, 8>, 65> b{};
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

NODISCARD INLINE Square apply_transform(Square sq, Symmetry_Transform t)
{
	int f = sq_file(sq);
	int r = sq_rank(sq);
	const uint8_t bits = static_cast<uint8_t>(t);
	if (bits & 1) f = 7 - f;
	if (bits & 2) r = 7 - r;
	if (bits & 4) std::swap(f, r);
	return sq_make(static_cast<Rank>(r), static_cast<File>(f));
}

INLINE Square apply_diag_transform(Square sq)
{
	return apply_transform(sq, Symmetry_Transform::DIAG);
}

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
		return Const_Span<Square>(ANCHOR_TRIANGLE_SQUARES,
		                          std::size(ANCHOR_TRIANGLE_SQUARES));
	return Const_Span<Square>(ANCHOR_FILE_MIRROR_SQUARES,
	                          std::size(ANCHOR_FILE_MIRROR_SQUARES));
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

const King_Slice_Manager& slice_mgr_for(Symmetry_Group sym)
{
	static const King_Slice_Manager file_mirror(Symmetry_Group::FILE_MIRROR);
	static const King_Slice_Manager dihedral_8(Symmetry_Group::DIHEDRAL_8);
	return sym == Symmetry_Group::DIHEDRAL_8 ? dihedral_8 : file_mirror;
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
		if (cfg.is_populated(c))
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
	const Square wk = (*placements)[WHITE_KINGS][0];
	const Square bk = (*placements)[BLACK_KINGS][0];
	const auto& lookup = cfg.slice_manager().lookup(wk, bk);

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

	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
	{
		if (!cfg.is_populated(c)) continue;
		const Piece pc = cfg.group(c).piece();
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
	uint64_t rank = 0;
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
	uint64_t rank = idx;
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
	const Piece_Group* white_pawns,
	const Piece_Group* black_pawns)
{
	m_has_pawns = true;
	m_white_group = white_pawns;
	m_black_group = black_pawns;
	m_white_table_size = white_pawns ? white_pawns->table_size() : 1;
	m_black_table_size = black_pawns ? black_pawns->table_size() : 1;
	m_num_cartesian_slices = m_white_table_size * m_black_table_size;

	m_storage_by_cartesian.assign(m_num_cartesian_slices, -1);
	m_cartesian_by_storage.clear();
	m_cartesian_by_storage.reserve(m_num_cartesian_slices);
	for (size_t cart = 0; cart < m_num_cartesian_slices; ++cart)
	{
		const auto w_idx = static_cast<Piece_Group::Placement_Index>(
			cart % m_white_table_size);
		const auto b_idx = static_cast<Piece_Group::Placement_Index>(
			cart / m_white_table_size);
		Piece_Group::Placement w_pl, b_pl;
		if (m_white_group) w_pl = m_white_group->squares(w_idx);
		if (m_black_group) b_pl = m_black_group->squares(b_idx);

		bool overlap = false;
		for (size_t i = 0; i < w_pl.size() && !overlap; ++i)
			for (size_t j = 0; j < b_pl.size() && !overlap; ++j)
				if (w_pl[i] == b_pl[j]) overlap = true;
		if (overlap) continue;

		m_storage_by_cartesian[cart] = static_cast<int32_t>(m_cartesian_by_storage.size());
		m_cartesian_by_storage.push_back(static_cast<int32_t>(cart));
	}
	m_num_slices = m_cartesian_by_storage.size();
}

Pawn_Slice_Manager::Decomposed
Pawn_Slice_Manager::decompose(int32_t slice_id) const
{
	ASSERT(slice_id >= 0 && static_cast<size_t>(slice_id) < m_num_slices);
	Decomposed d;
	if (!m_has_pawns)
	{
		d.white_idx = d.black_idx = Piece_Group::Placement_Index(0);
		return d;
	}
	const int32_t cart = m_cartesian_by_storage[slice_id];
	d.white_idx = static_cast<Piece_Group::Placement_Index>(cart % m_white_table_size);
	d.black_idx = static_cast<Piece_Group::Placement_Index>(cart / m_white_table_size);
	return d;
}

int32_t Pawn_Slice_Manager::compose(
	Piece_Group::Placement_Index w,
	Piece_Group::Placement_Index b) const
{
	if (!m_has_pawns) return 0;
	const size_t cart = static_cast<size_t>(w) + static_cast<size_t>(b) * m_white_table_size;
	ASSERT(cart < m_num_cartesian_slices);
	const int32_t storage = m_storage_by_cartesian[cart];
	ASSERT(storage >= 0);
	return storage;
}

int32_t Pawn_Slice_Manager::lookup_from_squares(
	Const_Span<Square> white_pawn_squares,
	Const_Span<Square> black_pawn_squares) const
{
	if (!m_has_pawns) return 0;

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
	return compose(w_idx, b_idx);
}

bool Position_Index_Config::try_init()
{
	const auto counts = piece_counts();
	for (size_t i = 0; i < PIECE_NB; ++i)
		m_piece_counts_cached[i] = static_cast<int8_t>(counts[i]);
	const Symmetry_Group symmetry = pick_symmetry(*this);
	m_king_slice_manager = &slice_mgr_for(symmetry);

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

	const bool has_pawns = counts[WHITE_PAWN] > 0 || counts[BLACK_PAWN] > 0;
	if (has_pawns)
	{
		m_pawn_slice_manager = std::make_unique<Pawn_Slice_Manager>(
			m_groups[WHITE_PAWNS],
			m_groups[BLACK_PAWNS]);
	}
	else
	{
		m_pawn_slice_manager = std::make_unique<Pawn_Slice_Manager>();
	}
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

	const King_Slice_Manager& sm = slice_manager();
	if (index.king_slice_id < 0
	    || static_cast<size_t>(index.king_slice_id) >= sm.num_slices)
		return false;
	const auto [wk, bk] = sm.kings_of_slice[index.king_slice_id];

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
	canonicalize_placements(inout_param(placements), cfg);

	const Square wk = placements[WHITE_KINGS][0];
	const Square bk = placements[BLACK_KINGS][0];
	const int32_t king_slice_id = cfg.slice_manager().lookup(wk, bk).slice_id;
	if (king_slice_id == SLICE_NONE)
		return BOARD_INDEX_NONE;

	Decomposed_Board_Index dix{};
	dix.king_slice_id = king_slice_id;

	if (cfg.pawn_slice_manager().has_pawns())
	{
		const auto& w_pl = placements[WHITE_PAWNS];
		const auto& b_pl = placements[BLACK_PAWNS];
		dix.pawn_slice_id = cfg.pawn_slice_manager().lookup_from_squares(
			Const_Span<Square>(w_pl.begin(), w_pl.size()),
			Const_Span<Square>(b_pl.begin(), b_pl.size()));
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
