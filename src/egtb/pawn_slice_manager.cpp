#include "egtb/pawn_slice_manager.h"
#include "egtb/pair_group.h"
#include "egtb/piece_group.h"

#include "chess/bitboard.h"
#include "chess/chess.h"

#include "util/intrin.h"

#include <algorithm>

namespace {

// Plies-to-promotion. Topo key: pushes strictly reduce one pawn's life, so
// lower-total slices are post-push and must be processed first. This holds for
// the coupled pair too: a pair push moves white up or black down, lowering the
// pair's combined life exactly as a free-pawn push does.
int pawn_life(Color c, Square sq)
{
	const int r = static_cast<int>(sq_rank(sq));
	return c == WHITE ? (static_cast<int>(RANK_8) - r)
	                  : (r - static_cast<int>(RANK_1));
}

}  // namespace

Pawn_Slice_Manager::Pawn_Slice_Manager(const Pair_Group* pair,
                                       const Piece_Group* white_pawns,
                                       const Piece_Group* black_pawns) :
	m_pair_topo_batches(Topo_Batch_Builder{ this })
{
	m_has_pawns = pair != nullptr || white_pawns != nullptr || black_pawns != nullptr;
	m_pair_group = pair;
	m_white_group = white_pawns;
	m_black_group = black_pawns;
	m_pair_table_size  = pair ? pair->table_size() : 1;
	m_white_table_size = white_pawns ? white_pawns->table_size() : 1;
	m_black_table_size = black_pawns ? black_pawns->table_size() : 1;
	m_num_cartesian_slices =
		m_pair_table_size * m_white_table_size * m_black_table_size;
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
			for (const Square s : out_pl.back()) occ |= square_bb(s);
			out_occ.push_back(occ);
		}
	};
	std::vector<Piece_Group::Placement> white_pl, black_pl;
	std::vector<Bitboard> white_occ, black_occ, pair_occ;
	placements_of(m_white_group, white_pl, white_occ);
	placements_of(m_black_group, black_pl, black_occ);
	if (m_pair_group)
		for (size_t i = 0; i < m_pair_table_size; ++i)
			pair_occ.push_back(square_bb(m_pair_group->white_square(static_cast<Pair_Group::Index>(i)))
			                 | square_bb(m_pair_group->black_square(static_cast<Pair_Group::Index>(i))));
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
		for (const Square s : white_pl[w]) wsq[nw++] = s;
		for (const Square s : black_pl[b]) bsq[nb++] = s;
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
	// Without a pair, survival is pure disjointness and every black placement
	// leaves the same number of white ones, so the exact count is closed form.
	if (m_pair_group == nullptr && m_white_group != nullptr && m_black_group != nullptr)
		m_cartesian_by_storage.reserve(
			m_black_table_size * binomial(possible_sq_nb(WHITE_PAWN) - m_black_group->size(),
			                              m_white_group->size()));
	else
		m_cartesian_by_storage.reserve(m_num_cartesian_slices);

	size_t cart = 0;
	for (size_t b = 0; b < m_black_table_size; ++b)
	{
		for (size_t w = 0; w < m_white_table_size; ++w)
		{
			// Free pawns of the two colors collide: no pair placement rescues the
			// cell, so skip its whole run of pair cells at once.
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

	// Prefix the per-block survivor counts, so a storage id needs only the one
	// block holding its cell.
	int32_t running = 0;
	for (Rank_Block& blk : m_rank_blocks)
	{
		blk.survivors_before = running;
		running += static_cast<int32_t>(popcnt(blk.bits));
	}
	ASSERT(static_cast<size_t>(running) == m_num_slices);
}

std::vector<std::vector<int32_t>> Pawn_Slice_Manager::Topo_Batch_Builder::operator()() const
{
	return psm->build_pair_topo_batches();
}

int Pawn_Slice_Manager::slice_life(int32_t slice_id) const
{
	const Decomposed d = decompose(slice_id);
	int total_life = 0;
	if (m_pair_group)
	{
		total_life += pawn_life(WHITE, m_pair_group->white_square(d.pair_idx));
		total_life += pawn_life(BLACK, m_pair_group->black_square(d.pair_idx));
	}
	if (m_white_group)
		for (const Square s : m_white_group->squares(d.white_idx))
			total_life += pawn_life(WHITE, s);
	if (m_black_group)
		for (const Square s : m_black_group->squares(d.black_idx))
			total_life += pawn_life(BLACK, s);
	return total_life;
}

std::vector<std::vector<int32_t>> Pawn_Slice_Manager::build_pair_topo_batches() const
{
	std::vector<std::vector<int32_t>> out;

	// Wavefront batches: storage ids grouped by total pawn life, batches in
	// ascending life order and ids ascending within each batch. Only the smaller
	// id of each file-mirror pair is kept. Pawns live on ranks 2-7, so one pawn
	// is at most RANK_8 - RANK_2 pushes from promoting and the total over the
	// pawn cap indexes a bucket directly.
	constexpr int MAX_PAWN_LIFE = static_cast<int>(RANK_8) - static_cast<int>(RANK_2);
	constexpr int MAX_SLICE_LIFE = MAX_PAWN_LIFE * static_cast<int>(MAX_TOTAL_PAWNS);
	std::vector<std::vector<int32_t>> slices_by_life(MAX_SLICE_LIFE + 1);
	for (int32_t sid = 0; sid < static_cast<int32_t>(m_num_slices); ++sid)
	{
		if (sid > mirror_slice_of(sid)) continue;

		const int total_life = slice_life(sid);
		ASSERT(total_life >= 0 && total_life <= MAX_SLICE_LIFE);
		slices_by_life[total_life].push_back(sid);
	}
	for (auto& batch : slices_by_life)
		if (!batch.empty())
			out.push_back(std::move(batch));
	return out;
}

int32_t Pawn_Slice_Manager::mirror_slice_of(int32_t slice_id) const
{
	if (!m_has_pawns) return slice_id;
	const Decomposed d = decompose(slice_id);

	auto mirror_group_idx = [](const Piece_Group* g, Piece_Group::Placement_Index idx) {
		if (g == nullptr) return idx;
		const auto pl = g->squares(idx);
		const auto mir = pl.with_transformed_squares(
			[](Square s) { return sq_file_mirror(s); });
		return g->compound_index(mir);
	};

	if (!m_pair_group)
		return compose(d.pair_idx,
		               mirror_group_idx(m_white_group, d.white_idx),
		               mirror_group_idx(m_black_group, d.black_idx));

	// The pair slot is positional -- the opposing pair minimal by (file,
	// white_rank, black_rank) -- and file mirror reverses file order, so the
	// mirror of this cell's pair need not be the mirrored cell's pair. With
	// pawns on both sides two opposing pairs can exist, and mirroring swaps
	// which one wins: pair a2/a3 with free b2/b3 mirrors to h2/h3 with g2/g3,
	// where g, not h, is minimal. Re-derive from the mirrored squares the same
	// way canonical_board_index does; mirroring the designated pair instead
	// composes a cartesian cell the enumeration pruned, which has no storage id.
	Square wsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
	Square bsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
	size_t nw = 0, nb = 0;
	wsq[nw++] = sq_file_mirror(m_pair_group->white_square(d.pair_idx));
	bsq[nb++] = sq_file_mirror(m_pair_group->black_square(d.pair_idx));
	if (m_white_group)
		for (const Square s : m_white_group->squares(d.white_idx))
			wsq[nw++] = sq_file_mirror(s);
	if (m_black_group)
		for (const Square s : m_black_group->squares(d.black_idx))
			bsq[nb++] = sq_file_mirror(s);

	Square pair_w = SQ_END, pair_b = SQ_END;
	Pair_Group::canonical_pair(Const_Span<Square>(wsq, nw),
	                           Const_Span<Square>(bsq, nb), pair_w, pair_b);

	Square free_w[Piece_Group::MAX_PIECE_GROUP_SIZE];
	Square free_b[Piece_Group::MAX_PIECE_GROUP_SIZE];
	size_t nfw = 0, nfb = 0;
	for (size_t i = 0; i < nw; ++i) if (wsq[i] != pair_w) free_w[nfw++] = wsq[i];
	for (size_t i = 0; i < nb; ++i) if (bsq[i] != pair_b) free_b[nfb++] = bsq[i];

	return lookup_from_squares(pair_w, pair_b,
	                           Const_Span<Square>(free_w, nfw),
	                           Const_Span<Square>(free_b, nfb));
}

Pawn_Slice_Manager::Decomposed Pawn_Slice_Manager::decompose(int32_t slice_id) const
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

int32_t Pawn_Slice_Manager::compose(Pair_Group::Index pair,
                                    Piece_Group::Placement_Index w,
                                    Piece_Group::Placement_Index b) const
{
	if (!m_has_pawns) return 0;
	const size_t cart = static_cast<size_t>(pair)
	                  + static_cast<size_t>(w) * m_pair_table_size
	                  + static_cast<size_t>(b) * m_pair_table_size * m_white_table_size;
	ASSERT(cart < m_num_cartesian_slices);
	// Overlap pairs have no storage id. Callers come from physical board state
	// or from pre-checked-empty pushes, so this never fires for correct callers.
	ASSERT(cartesian_survives(cart));
	return storage_of_cartesian(cart);
}

std::vector<int32_t> Pawn_Slice_Manager::push_target_slices(int32_t slice_id) const
{
	if (!m_has_pawns) return {};
	const Decomposed d = decompose(slice_id);

	Piece_Group::Placement w_pl, b_pl;
	if (m_white_group) w_pl = m_white_group->squares(d.white_idx);
	if (m_black_group) b_pl = m_black_group->squares(d.black_idx);

	const Square pair_w = m_pair_group ? m_pair_group->white_square(d.pair_idx) : SQ_END;
	const Square pair_b = m_pair_group ? m_pair_group->black_square(d.pair_idx) : SQ_END;

	std::vector<int32_t> out;

	auto occupied = [&](Square sq) {
		if (m_pair_group && (sq == pair_w || sq == pair_b)) return true;
		for (size_t i = 0; i < w_pl.size(); ++i) if (w_pl[i] == sq) return true;
		for (size_t i = 0; i < b_pl.size(); ++i) if (b_pl[i] == sq) return true;
		return false;
	};

	auto emit_white_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_w_idx = m_white_group->compound_index_after_quiet_move(d.white_idx, from, to);
		out.push_back(compose(d.pair_idx, new_w_idx, d.black_idx));
	};
	auto emit_black_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_b_idx = m_black_group->compound_index_after_quiet_move(d.black_idx, from, to);
		out.push_back(compose(d.pair_idx, d.white_idx, new_b_idx));
	};

	if (m_white_group)
	{
		for (size_t i = 0; i < w_pl.size(); ++i)
		{
			const Square from = w_pl[i];
			const Rank r = sq_rank(from);
			if (r == RANK_7 || r == RANK_8 || r == RANK_1) continue;  // promo or impossible
			const Square to1 = sq_make(static_cast<Rank>(r + 1), sq_file(from));
			if (occupied(to1)) continue;  // blocks double too
			emit_white_push(from, to1);
			if (r == RANK_2)
			{
				const Square to2 = sq_make(RANK_4, sq_file(from));
				emit_white_push(from, to2);
			}
		}
	}
	if (m_black_group)
	{
		for (size_t i = 0; i < b_pl.size(); ++i)
		{
			const Square from = b_pl[i];
			const Rank r = sq_rank(from);
			if (r == RANK_2 || r == RANK_1 || r == RANK_8) continue;
			const Square to1 = sq_make(static_cast<Rank>(r - 1), sq_file(from));
			if (occupied(to1)) continue;
			emit_black_push(from, to1);
			if (r == RANK_7)
			{
				const Square to2 = sq_make(RANK_5, sq_file(from));
				emit_black_push(from, to2);
			}
		}
	}

	if (m_pair_group)
	{
		auto emit_pair_white = [&](Square to) {
			if (occupied(to)) return;
			const auto np = m_pair_group->index_of(to, pair_b);
			out.push_back(compose(np, d.white_idx, d.black_idx));
		};
		auto emit_pair_black = [&](Square to) {
			if (occupied(to)) return;
			const auto np = m_pair_group->index_of(pair_w, to);
			out.push_back(compose(np, d.white_idx, d.black_idx));
		};

		const Rank wr = sq_rank(pair_w);
		if (wr != RANK_7 && wr != RANK_8 && wr != RANK_1)
		{
			const Square to1 = sq_make(static_cast<Rank>(wr + 1), sq_file(pair_w));
			if (!occupied(to1))
			{
				emit_pair_white(to1);
				if (wr == RANK_2)
					emit_pair_white(sq_make(RANK_4, sq_file(pair_w)));
			}
		}
		const Rank br = sq_rank(pair_b);
		if (br != RANK_2 && br != RANK_1 && br != RANK_8)
		{
			const Square to1 = sq_make(static_cast<Rank>(br - 1), sq_file(pair_b));
			if (!occupied(to1))
			{
				emit_pair_black(to1);
				if (br == RANK_7)
					emit_pair_black(sq_make(RANK_5, sq_file(pair_b)));
			}
		}
	}

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
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

int32_t Pawn_Slice_Manager::slice_after_pawn_push(
	int32_t slice_id, Color mover, Square from, Square to) const
{
	if (!m_has_pawns) return 0;
	const Decomposed d = decompose(slice_id);
	Pair_Group::Index pair_idx = d.pair_idx;
	Piece_Group::Placement_Index w_idx = d.white_idx;
	Piece_Group::Placement_Index b_idx = d.black_idx;

	if (mover == WHITE)
	{
		if (m_pair_group && from == m_pair_group->white_square(d.pair_idx))
			pair_idx = m_pair_group->index_of(to, m_pair_group->black_square(d.pair_idx));
		else
			w_idx = m_white_group->compound_index_after_quiet_move(d.white_idx, from, to);
	}
	else
	{
		if (m_pair_group && from == m_pair_group->black_square(d.pair_idx))
			pair_idx = m_pair_group->index_of(m_pair_group->white_square(d.pair_idx), to);
		else
			b_idx = m_black_group->compound_index_after_quiet_move(d.black_idx, from, to);
	}
	return compose(pair_idx, w_idx, b_idx);
}
