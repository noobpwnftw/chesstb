#include "probe/position_index.h"

#include "chess/attack.h"
#include "chess/bitboard.h"
#include "chess/pawn_slice_bits.h"
#include "chess/chess.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/math.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
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

constexpr uint64_t PAWN_SQUARES_BB = 0x00FFFFFFFFFFFF00ull;  // ranks 2-7

// Without a pair only overlaps are cut, so a slice is direct: black's
// placement, then white's colex rank among the pawn squares black leaves free.
// Removing black's squares keeps the remaining ones in order.
NODISCARD size_t free_square_rank(Bitboard white, Bitboard black)
{
	const uint64_t free = PAWN_SQUARES_BB & ~black.bits();
	uint64_t white_bb = white.bits();
	size_t rank = 0;
#if CHESSTB_HAVE_PEXT
	uint64_t bits = _pext_u64(white_bb, free);
	for (size_t k = 1; bits; ++k) rank += BINOMIAL[pop_first_bit(bits)][k];
#else
	for (size_t k = 1; white_bb; ++k)
		rank += BINOMIAL[popcnt(free & ((uint64_t{1} << pop_first_bit(white_bb)) - 1))][k];
#endif
	return rank;
}

#ifdef CHESSTB_INDEX_INVERSE
// The inverse: the n white pawns of that rank.
NODISCARD Bitboard free_square_unrank(size_t rank, size_t n, Bitboard black)
{
	const uint64_t free = PAWN_SQUARES_BB & ~black.bits();
	uint64_t bits = 0;
	size_t hi = popcnt(free);
	for (size_t k = n; k > 0; --k)
	{
		size_t p = hi - 1;
		while (BINOMIAL[p][k] > rank) --p;
		bits |= uint64_t{1} << p;
		rank -= BINOMIAL[p][k];
		hi = p;
	}
	return Bitboard(pdep_u64(bits, free));
}
#endif

// White king squares of the slices: files a-d under the file mirror, the
// a1-d1-d4 triangle under all eight symmetries.
constexpr Bitboard ANCHOR_FILE_MIRROR(0x0F0F0F0F0F0F0F0FULL);
constexpr Bitboard ANCHOR_TRIANGLE(0x00000000080C0E0FULL);
constexpr Bitboard MAIN_DIAG(0x8040201008040201ULL);
constexpr Bitboard ABOVE_DIAG(0x7F3F1F0F07030100ULL);

// The transform mask on every square of `b`: 1 mirrors the files, 2 the
// ranks, 4 then the a1-h8 diagonal.
NODISCARD Bitboard apply_transform(Bitboard b, uint8_t t)
{
	if (t & 1) b = b.mirror_files();
	if (t & 2) b = b.mirror_ranks();
	if (t & 4) b = b.mirror_diag();
	return b;
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
					t[w * STRIDE + b] = std::make_unique<King_Slice_Manager>(w, b);
		return t;
	}();
	const auto& mgr = table[white_rights * STRIDE + black_rights];
	ASSERT(mgr != nullptr);
	return *mgr;
}

}  // namespace

const Pawn_Slice_Manager& pawn_slice_mgr(bool has_pair, size_t white_pawns, size_t black_pawns)
{
	constexpr size_t STRIDE = MAX_TOTAL_PAWNS + 1;
	static std::mutex mu;
	static std::array<std::unique_ptr<const Pawn_Slice_Manager>, 2 * STRIDE * STRIDE> table;
	ASSERT(white_pawns < STRIDE && black_pawns < STRIDE);
	std::lock_guard<std::mutex> lk(mu);
	auto& slot = table[(size_t(has_pair) * STRIDE + white_pawns) * STRIDE + black_pawns];
	if (!slot) slot = std::make_unique<const Pawn_Slice_Manager>(has_pair, white_pawns, black_pawns);
	return *slot;
}

Piece_Group::Piece_Group(Piece pc, size_t count) :
	m_piece(pc),
	m_num_pieces(count),
	m_num_legal_squares(possible_sq_nb(pc)),
	m_first_square(possible_sq(pc, 0)),
	m_table_size(BINOMIAL[m_num_legal_squares][count])
{
}

Piece_Group::Placement_Index Piece_Group::index_of(Bitboard b) const
{
	ASSERT(popcnt(b.bits()) == m_num_pieces);
	uint32_t rank = 0;
	for (size_t k = 1; b; ++k)
		rank += BINOMIAL[b.pop_first_square() - m_first_square][k];
	ASSERT(rank < m_table_size);
	return static_cast<Placement_Index>(rank);
}

Bitboard Piece_Group::squares(Placement_Index idx) const
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

	Bitboard b = Bitboard::make_empty();
	for (size_t i = 0; i < m_num_pieces; ++i)
		b |= static_cast<Square>(m_first_square + pos[i]);
	return b;
}

// Must enumerate exactly as the generator's copy does: the slice ids are stored.
King_Slice_Manager::King_Slice_Manager(size_t white_rights, size_t black_rights) :
	has_castling(true), castling_rights{ white_rights, black_rights },
	m_castling_group(std::make_shared<Castling_Group>(white_rights, black_rights))
{
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, 0, 0 };

	m_free_king_stride = m_castling_group->both_kings_pinned() ? size_t(1) : size_t(SQUARE_NB);
	const size_t n = m_castling_group->table_size();
	castling_slice_lookup.assign(n * m_free_king_stride, SLICE_NONE);

	for (size_t ci = 0; ci < n; ++ci)
	{
		const Castling_Group::Index cidx = static_cast<Castling_Group::Index>(ci);

		std::array<Bitboard, COLOR_NB> rooks{ Bitboard::make_empty(), Bitboard::make_empty() };
		Square pinned_king[COLOR_NB] = { SQ_END, SQ_END };
		Bitboard pinned = Bitboard::make_empty();
		for (const Color c : { WHITE, BLACK })
		{
			if (castling_rights[c] == 0) continue;
			pinned_king[c] = m_castling_group->king_square(cidx, c);
			rooks[c] = m_castling_group->rooks(cidx, c);
			pinned |= square_bb(pinned_king[c]) | rooks[c];
		}

#ifdef CHESSTB_INDEX_INVERSE
		auto emit = [&](Square wk, Square bk, size_t free_slot) {
			castling_slice_lookup[ci * m_free_king_stride + free_slot] = static_cast<int32_t>(num_slices++);
			kings_of_slice.emplace_back(wk, bk);
			castling_rooks_of_slice.push_back(rooks);
		};
#else
		auto emit = [&](Square, Square, size_t free_slot) {
			castling_slice_lookup[ci * m_free_king_stride + free_slot] = static_cast<int32_t>(num_slices++);
		};
#endif

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
			if (king_attacks(ksq).has_square(fk)) continue;
			emit(pinned_color == WHITE ? ksq : fk,
			     pinned_color == WHITE ? fk : ksq,
			     static_cast<size_t>(fk));
		}
	}
}

King_Slice_Manager::King_Slice_Manager(Symmetry_Group sym)
{
	for (auto& e : pair_lookup)
		e = { SLICE_NONE, 0, 0 };
	const Bitboard anchors = sym == Symmetry_Group::DIHEDRAL_8 ? ANCHOR_TRIANGLE : ANCHOR_FILE_MIRROR;

	for (Square wk = SQ_A1; wk < SQ_END; wk = static_cast<Square>(wk + 1))
	{
		if (!anchors.has_square(wk)) continue;

		for (Square bk = SQ_A1; bk < SQ_END; bk = static_cast<Square>(bk + 1))
		{
			if (bk == wk) continue;
			if (king_attacks(wk).has_square(bk)) continue;

			if (sym == Symmetry_Group::DIHEDRAL_8 && MAIN_DIAG.has_square(wk) && ABOVE_DIAG.has_square(bk))
				continue;

			const int32_t sid = static_cast<int32_t>(num_slices++);
#ifdef CHESSTB_INDEX_INVERSE
			kings_of_slice.emplace_back(wk, bk);
#endif

			const bool stab = (sym == Symmetry_Group::DIHEDRAL_8)
			               && MAIN_DIAG.has_square(wk)
			               && MAIN_DIAG.has_square(bk);

			pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)] =
				{ sid, 0, static_cast<uint8_t>(stab) };
		}
	}

	const int n_trans = sym == Symmetry_Group::DIHEDRAL_8 ? 8 : 2;
	for (Square wk = SQ_A1; wk < SQ_END; wk = static_cast<Square>(wk + 1))
	{
		for (Square bk = SQ_A1; bk < SQ_END; bk = static_cast<Square>(bk + 1))
		{
			Pair_Lookup& e = pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
			if (e.slice_id != SLICE_NONE) continue;
			if (wk == bk || king_attacks(wk).has_square(bk)) continue;

			for (uint8_t t = 0; t < n_trans; ++t)
			{
				const Square wk_t = apply_transform(square_bb(wk), t).peek_first_square();
				const Square bk_t = apply_transform(square_bb(bk), t).peek_first_square();
				const auto& look = pair_lookup[static_cast<int>(wk_t) * SQUARE_NB
				                              + static_cast<int>(bk_t)];
				if (look.slice_id != SLICE_NONE
				    && look.transform == 0)
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

Pawn_Slice_Manager::Pawn_Slice_Manager(bool has_pair, size_t white_pawns, size_t black_pawns)
{
	if (has_pair) m_pair_group = std::make_unique<const Pair_Group>();
	if (white_pawns) m_white_group = std::make_unique<const Piece_Group>(piece_make(WHITE, PAWN), white_pawns);
	if (black_pawns) m_black_group = std::make_unique<const Piece_Group>(piece_make(BLACK, PAWN), black_pawns);
	const Pair_Group* pair = m_pair_group.get();
	const Piece_Group* white = m_white_group.get();
	const Piece_Group* black = m_black_group.get();
	m_has_pawns = pair != nullptr || white != nullptr || black != nullptr;
	m_pair_table_size  = pair ? pair->table_size() : 1;
	m_white_table_size = white ? white->table_size() : 1;
	m_black_table_size = black ? black->table_size() : 1;
	m_num_cartesian_slices = m_pair_table_size * m_white_table_size * m_black_table_size;
	if (!pair)
	{
		m_white_free_size = BINOMIAL[PAWN_SQUARE_NB - black_pawns][white_pawns];
		m_num_slices = m_black_table_size * m_white_free_size;
		return;
	}
#ifdef CHESSTB_INDEX_INVERSE
	if (m_pair_table_size > 1)
		m_pair_table_size_div = Divider<uint64_t>(m_pair_table_size);
	if (m_white_table_size > 1)
		m_white_table_size_div = Divider<uint64_t>(m_white_table_size);
#endif

	auto placements_of = [](const Piece_Group* g) {
		std::vector<Bitboard> out;
		for (size_t i = 0; i < (g ? g->table_size() : 1); ++i)
			out.push_back(g ? g->squares(static_cast<Piece_Group::Placement_Index>(i))
			                : Bitboard::make_empty());
		return out;
	};

	// The bitmap ranks cells for lookup (and, for the tools, the
	// reverse storage map). Build it by bitsets, then count the survivors.
	m_rank_blocks.assign((m_num_cartesian_slices + 63) / 64, Rank_Block{});
	m_num_slices = 0;
	build_pawn_slice_bits(pair, placements_of(white), placements_of(black),
		[&](size_t cart, uint64_t bits) {
			if (!bits) return;
			m_num_slices += popcnt(bits);
			const size_t word = cart >> 6, shift = cart & 63;
			m_rank_blocks[word].bits |= bits << shift;
			if (shift && word + 1 < m_rank_blocks.size())
				m_rank_blocks[word + 1].bits |= bits >> (64 - shift);
		});
	int32_t running = 0;
	for (Rank_Block& block : m_rank_blocks)
	{
		block.survivors_before = running;
		running += static_cast<int32_t>(popcnt(block.bits));
	}
	ASSERT(static_cast<size_t>(running) == m_num_slices);
#ifdef CHESSTB_INDEX_INVERSE
	m_cartesian_by_storage.reserve(m_num_slices);
	for (size_t word = 0; word < m_rank_blocks.size(); ++word)
		for (uint64_t bits = m_rank_blocks[word].bits; bits;)
			m_cartesian_by_storage.push_back(static_cast<int32_t>(word * 64 + pop_first_bit(bits)));
	ASSERT(m_cartesian_by_storage.size() == m_num_slices);
#endif
}

#ifdef CHESSTB_INDEX_INVERSE
void Pawn_Slice_Manager::pawns_of(int32_t slice_id, Bitboard& white, Bitboard& black) const
{
	ASSERT(slice_id >= 0 && static_cast<size_t>(slice_id) < m_num_slices);
	white = black = Bitboard::make_empty();
	if (!m_has_pawns) return;
	if (!m_pair_group)
	{
		const size_t b_idx = static_cast<size_t>(slice_id) / m_white_free_size;
		const size_t rank = static_cast<size_t>(slice_id) - b_idx * m_white_free_size;
		if (m_black_group) black = m_black_group->squares(static_cast<Piece_Group::Placement_Index>(b_idx));
		white = free_square_unrank(rank, m_white_group ? m_white_group->size() : 0, black);
		return;
	}
	size_t rem = static_cast<size_t>(m_cartesian_by_storage[slice_id]);
	const size_t q = rem / m_pair_table_size_div;
	const auto pair_idx = static_cast<Pair_Group::Index>(rem - q * m_pair_table_size);
	rem = q;
	const size_t b_idx = m_white_table_size > 1 ? rem / m_white_table_size_div : rem;
	const size_t w_idx = rem - b_idx * m_white_table_size;
	if (m_white_group) white = m_white_group->squares(static_cast<Piece_Group::Placement_Index>(w_idx));
	if (m_black_group) black = m_black_group->squares(static_cast<Piece_Group::Placement_Index>(b_idx));
	ASSERT(!(white & m_pair_group->white_square(pair_idx)) && !(black & m_pair_group->black_square(pair_idx)));
	white |= m_pair_group->white_square(pair_idx);
	black |= m_pair_group->black_square(pair_idx);
}
#endif

int32_t Pawn_Slice_Manager::lookup(Square pair_white_sq, Square pair_black_sq,
                                   Bitboard white, Bitboard black) const
{
	const Piece_Group::Placement_Index b_idx = m_black_group ? m_black_group->index_of(black) : 0;
	if (!m_pair_group)
	{
		const size_t rank = free_square_rank(white, black);
		ASSERT(rank < m_white_free_size);
		return static_cast<int32_t>(b_idx * m_white_free_size + rank);
	}

	// With a pair: the cell's rank among the bitmap's survivors.
	const Piece_Group::Placement_Index w_idx = m_white_group ? m_white_group->index_of(white) : 0;
	const size_t cart = static_cast<size_t>(m_pair_group->index_of(pair_white_sq, pair_black_sq))
	                  + static_cast<size_t>(w_idx) * m_pair_table_size
	                  + static_cast<size_t>(b_idx) * m_pair_table_size * m_white_table_size;
	ASSERT(cart < m_num_cartesian_slices);
	ASSERT(cartesian_survives(cart));
	return storage_of_cartesian(cart);
}

bool Position_Index_Config::try_init()
{
	const auto counts = piece_counts();

	const size_t total_pawns = counts[WHITE_PAWN] + counts[BLACK_PAWN]
	                         + (has_opposing_pair() ? 2 : 0);
	if (total_pawns > MAX_TOTAL_PAWNS)
		return false;

	const Symmetry_Group symmetry = pick_symmetry(*this);
	ASSERT(has_castling() == (symmetry == Symmetry_Group::NONE));
	m_king_slice_manager = has_castling()
		? &castling_slice_mgr(castling_rights(WHITE), castling_rights(BLACK))
	                                    : &slice_mgr_for(symmetry);

	// Pawns are the pawn slice manager's.
	constexpr std::pair<Piece_Type, Piece_Type_Class> TYPES[] = {
		{ QUEEN, QUEENS }, { ROOK, ROOKS }, { BISHOP, BISHOPS }, { KNIGHT, KNIGHTS } };
	for (Color c : { WHITE, BLACK })
		for (const auto& [pt, ptcl] : TYPES)
		{
			const Piece pc = piece_make(c, pt);
			m_groups[make_piece_class(c, ptcl)] =
				counts[pc] ? std::make_unique<const Piece_Group>(pc, counts[pc]) : nullptr;
		}

	m_pawn_slice_manager = &pawn_slice_mgr(has_opposing_pair(), counts[WHITE_PAWN], counts[BLACK_PAWN]);
	m_num_pawn_slices = m_pawn_slice_manager->num_slices();

	m_num_populated_classes = 0;
	size_t w = 1;
	for (Piece_Class i = PIECE_CLASS_START; i < PIECE_CLASS_END; ++i)
	{
		if (m_groups[i] == nullptr) continue;
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
	m_pawn_slice_stride = uint64_t(num_king_slices) * m_within_slice_size;
	if (m_within_slice_size != 0 && m_pawn_slice_stride / m_within_slice_size != num_king_slices)
		return false;

	const uint64_t total = m_num_pawn_slices * m_pawn_slice_stride;
	if (m_pawn_slice_stride != 0 && total / m_pawn_slice_stride != m_num_pawn_slices)
		return false;
	if (total > MAX_NUM_POSITIONS)
		return false;
#ifdef CHESSTB_INDEX_INVERSE
	m_num_positions = total;
#endif
	return true;
}

Position_Index_Config::Position_Index_Config(const Piece_Config& ps) :
	Piece_Config(ps)
{
	if (!try_init())
		throw std::runtime_error("Position_Index_Config: configuration too large to materialize");
}

#ifdef CHESSTB_INDEX_INVERSE
void Position_Index_Config::decompose_board_index(
	Board_Index pos,
	Out_Param<Decomposed_Board_Index> idx) const
{
	uint64_t p = pos;
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
	size_t within = static_cast<size_t>(p);
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

	const King_Slice_Manager& ksm = king_slice_manager();
	if (index.king_slice_id < 0
	    || static_cast<size_t>(index.king_slice_id) >= ksm.num_slices)
		return false;
	const auto [wk, bk] = ksm.kings_of_slice[index.king_slice_id];

	Bitboard white_occ = Bitboard::make_empty();
	Bitboard black_occ = Bitboard::make_empty();

	// `pc` on the squares of `bb`; false if one is taken.
	auto place = [&](Piece pc, Bitboard bb) {
		if (bb & (white_occ | black_occ)) return false;
		board->m_pieces[pc] |= bb;
		(piece_color(pc) == WHITE ? white_occ : black_occ) |= bb;
		return true;
	};
	place(WHITE_KING, square_bb(wk));
	place(BLACK_KING, square_bb(bk));

	Bitboard white_pawns, black_pawns;
	pawn_slice_manager().pawns_of(index.pawn_slice_id, white_pawns, black_pawns);
	if (!place(WHITE_PAWN, white_pawns) || !place(BLACK_PAWN, black_pawns)) return false;

	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Group& g = group(m_populated_classes[i]);
		if (!place(g.piece(), g.squares(index.within[m_populated_classes[i]]))) return false;
	}

	board->m_castling_rooks = Bitboard::make_empty();

	if (has_castling())
	{
		for (const Color cc : { WHITE, BLACK })
		{
			if (castling_rights(cc) == 0) continue;
			const Bitboard rooks = castling_rooks(index.king_slice_id, cc);
			if (!place(piece_make(cc, ROOK), rooks)) return false;
			board->m_castling_rooks |= rooks;
		}
	}

	board->m_pieces[WHITE_OCCUPY] = white_occ;
	board->m_pieces[BLACK_OCCUPY] = black_occ;
	return true;
}
#endif

Board_Index board_index_of_position(
	const Position_Index_Config& cfg,
	const Index_Storage_Layout& layout,
	const Position& pos)
{
	const size_t n = cfg.num_populated_classes();
	const Piece_Class* classes = cfg.populated_classes();

	Bitboard boards[PIECE_CLASS_NB];
	for (size_t i = 0; i < n; ++i)
		boards[classes[i]] = pos.piece_bb(cfg.group(classes[i]).piece());
	Bitboard white_pawns = pos.piece_bb(WHITE_PAWN), black_pawns = pos.piece_bb(BLACK_PAWN);

	const Square wk = pos.king_square(WHITE), bk = pos.king_square(BLACK);
	int32_t king_slice_id;
	Piece_Group::Placement_Index idx[PIECE_CLASS_NB];
	if (!cfg.has_castling())
	{
		// The symmetry that takes the kings to their slice's squares, applied
		// to every man.
		const auto& look = cfg.king_slice_manager().lookup(wk, bk);
		king_slice_id = look.slice_id;
		if (king_slice_id == SLICE_NONE) return BOARD_INDEX_NONE;
		if (look.transform)
		{
			for (size_t i = 0; i < n; ++i)
				boards[classes[i]] = apply_transform(boards[classes[i]], look.transform);
			white_pawns = apply_transform(white_pawns, look.transform);
			black_pawns = apply_transform(black_pawns, look.transform);
		}
		for (size_t i = 0; i < n; ++i)
			idx[classes[i]] = cfg.group(classes[i]).index_of(boards[classes[i]]);

		// Both kings on the diagonal: the a1-h8 mirror keeps their slice, so
		// the men take whichever side ranks lower.
		if (look.has_diag_stabilizer)
		{
			Piece_Group::Placement_Index alt[PIECE_CLASS_NB];
			size_t within_cur = 0, within_alt = 0;
			for (size_t i = 0; i < n; ++i)
			{
				const Piece_Class c = classes[i];
				alt[c] = cfg.group(c).index_of(boards[c].mirror_diag());
				within_cur += cfg.weight(c) * idx[c];
				within_alt += cfg.weight(c) * alt[c];
			}
			if (within_alt < within_cur)
				for (size_t i = 0; i < n; ++i) idx[classes[i]] = alt[classes[i]];
		}
	}
	else
	{
		// A castling rook is not in its class, which holds only the free ones.
		const Bitboard rooks[COLOR_NB] = { pos.castling_rooks(WHITE), pos.castling_rooks(BLACK) };
		for (const Color c : { WHITE, BLACK })
		{
			const Piece_Class rook_class = make_piece_class(c, ROOKS);
			if (cfg.is_populated(rook_class)) boards[rook_class] &= ~rooks[c];
		}
		king_slice_id = cfg.king_slice_manager().castling_slice_of(wk, bk, rooks[WHITE], rooks[BLACK]);
		if (king_slice_id == SLICE_NONE) return BOARD_INDEX_NONE;
		for (size_t i = 0; i < n; ++i)
			idx[classes[i]] = cfg.group(classes[i]).index_of(boards[classes[i]]);
	}

	int32_t pawn_slice_id = 0;
	if (cfg.pawn_slice_manager().has_pawns())
	{
		Square pair_w = SQ_END, pair_b = SQ_END;
		if (cfg.has_opposing_pair())
		{
			Pair_Group::canonical_pair(white_pawns, black_pawns, pair_w, pair_b);
			white_pawns ^= pair_w;
			black_pawns ^= pair_b;
		}
		pawn_slice_id = cfg.pawn_slice_manager().lookup(pair_w, pair_b, white_pawns, black_pawns);
	}

	// Within the slice, the classes in storage order, the first varying fastest.
	size_t within = 0;
	size_t w = 1;
	for (size_t i = 0; i < layout.n; ++i)
	{
		within += w * idx[layout.order[i]];
		w *= layout.radix[i];
	}
	return static_cast<Board_Index>(static_cast<uint64_t>(pawn_slice_id) * cfg.pawn_slice_stride()
	                                + static_cast<uint64_t>(king_slice_id) * cfg.within_slice_size()
	                                + within);
}

#ifdef CHESSTB_INDEX_INVERSE
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
#endif
