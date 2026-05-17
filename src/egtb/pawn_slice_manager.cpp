#include "egtb/pawn_slice_manager.h"
#include "egtb/pair_group.h"
#include "egtb/piece_group.h"

#include "chess/chess.h"

#include <algorithm>

namespace {

// Plies-to-promotion. Topo key: pushes strictly reduce one pawn's life, so
// lower-total slices are post-push and must be processed first. This holds for
// the coupled pair too: a pair push moves white up or black down, lowering the
// pair's combined life exactly as a free-pawn push does.
INLINE int pawn_life(Color c, Square sq)
{
	const int r = static_cast<int>(sq_rank(sq));
	return c == WHITE ? (static_cast<int>(RANK_8) - r)
	                  : (r - static_cast<int>(RANK_1));
}

}  // namespace

Pawn_Slice_Manager::Pawn_Slice_Manager()
{
	m_pair_topo_batches.push_back({ 0 });
}

Pawn_Slice_Manager::Pawn_Slice_Manager(const Pair_Group* pair,
                                       const Piece_Group* white_pawns,
                                       const Piece_Group* black_pawns)
{
	m_has_pawns = true;
	m_pair_group = pair;
	m_white_group = white_pawns;
	m_black_group = black_pawns;
	m_pair_table_size  = pair ? pair->table_size() : 1;
	m_white_table_size = white_pawns ? white_pawns->table_size() : 1;
	m_black_table_size = black_pawns ? black_pawns->table_size() : 1;
	m_num_cartesian_slices =
		m_pair_table_size * m_white_table_size * m_black_table_size;

	// Gathers every pawn square of a decomposed slice (pair members first, then
	// free pawns of each color). Used for overlap pruning and life totals.
	auto gather = [&](const Decomposed& d, Square* out) -> size_t {
		size_t n = 0;
		if (m_pair_group)
		{
			out[n++] = m_pair_group->white_square(d.pair_idx);
			out[n++] = m_pair_group->black_square(d.pair_idx);
		}
		if (m_white_group)
			for (const Square s : m_white_group->squares(d.white_idx)) out[n++] = s;
		if (m_black_group)
			for (const Square s : m_black_group->squares(d.black_idx)) out[n++] = s;
		return n;
	};

	auto pair_is_canonical = [&](const Decomposed& d) -> bool {
		if (!m_pair_group) return true;
		const Square pair_w = m_pair_group->white_square(d.pair_idx);
		const Square pair_b = m_pair_group->black_square(d.pair_idx);
		Square wsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
		Square bsq[Piece_Group::MAX_PIECE_GROUP_SIZE + 1];
		size_t nw = 0, nb = 0;
		wsq[nw++] = pair_w;
		bsq[nb++] = pair_b;
		if (m_white_group)
			for (const Square s : m_white_group->squares(d.white_idx)) wsq[nw++] = s;
		if (m_black_group)
			for (const Square s : m_black_group->squares(d.black_idx)) bsq[nb++] = s;
		Square cw = SQ_END, cb = SQ_END;
		Pair_Group::canonical_pair(Const_Span<Square>(wsq, nw),
		                           Const_Span<Square>(bsq, nb), cw, cb);
		return cw == pair_w && cb == pair_b;
	};

	// Assign compact storage ids, skipping overlap (unreachable) pairs and cells
	// whose designated pair is not the canonical opposing pair.
	m_storage_by_cartesian.assign(m_num_cartesian_slices, -1);
	m_cartesian_by_storage.clear();
	m_cartesian_by_storage.reserve(m_num_cartesian_slices);
	Square sqbuf[2 * Piece_Group::MAX_PIECE_GROUP_SIZE + 2];
	for (size_t cart = 0; cart < m_num_cartesian_slices; ++cart)
	{
		Decomposed d;
		d.pair_idx  = static_cast<Pair_Group::Index>(cart % m_pair_table_size);
		const size_t rem = cart / m_pair_table_size;
		d.white_idx = static_cast<Piece_Group::Placement_Index>(rem % m_white_table_size);
		d.black_idx = static_cast<Piece_Group::Placement_Index>(rem / m_white_table_size);

		const size_t n = gather(d, sqbuf);
		bool overlap = false;
		for (size_t i = 0; i < n && !overlap; ++i)
			for (size_t j = i + 1; j < n && !overlap; ++j)
				if (sqbuf[i] == sqbuf[j]) overlap = true;
		if (overlap) continue;
		if (!pair_is_canonical(d)) continue;

		m_storage_by_cartesian[cart] = static_cast<int32_t>(m_cartesian_by_storage.size());
		m_cartesian_by_storage.push_back(static_cast<int32_t>(cart));
	}
	m_num_slices = m_cartesian_by_storage.size();

	// Sort storage ids by total pawn life for topo wavefront order.
	std::vector<std::pair<int, int32_t>> life_of_slice;
	life_of_slice.reserve(m_num_slices);
	for (int32_t sid = 0; sid < static_cast<int32_t>(m_num_slices); ++sid)
	{
		const Decomposed d = decompose(sid);
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
		life_of_slice.emplace_back(total_life, sid);
	}
	std::stable_sort(life_of_slice.begin(), life_of_slice.end());

	// Group into wavefront batches by total_life; keep only smaller-id of
	// each file-mirror pair.
	int prev_life = -1;
	for (const auto& [life, sid] : life_of_slice)
	{
		const int32_t mir = mirror_slice_of(sid);
		if (sid > mir) continue;
		if (life != prev_life)
		{
			m_pair_topo_batches.emplace_back();
			prev_life = life;
		}
		m_pair_topo_batches.back().push_back(sid);
	}
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

	const Pair_Group::Index pair_mir =
		m_pair_group ? m_pair_group->file_mirror(d.pair_idx) : d.pair_idx;
	const auto w_mir = mirror_group_idx(m_white_group, d.white_idx);
	const auto b_mir = mirror_group_idx(m_black_group, d.black_idx);
	return compose(pair_mir, w_mir, b_mir);
}

Pawn_Slice_Manager::Decomposed Pawn_Slice_Manager::decompose(int32_t slice_id) const
{
	ASSERT(slice_id >= 0 && static_cast<size_t>(slice_id) < m_num_slices);
	Decomposed d;
	if (!m_has_pawns) { d.pair_idx = 0; d.white_idx = 0; d.black_idx = 0; return d; }
	const int32_t cart = m_cartesian_by_storage[slice_id];
	d.pair_idx  = static_cast<Pair_Group::Index>(cart % m_pair_table_size);
	const size_t rem = static_cast<size_t>(cart) / m_pair_table_size;
	d.white_idx = static_cast<Piece_Group::Placement_Index>(rem % m_white_table_size);
	d.black_idx = static_cast<Piece_Group::Placement_Index>(rem / m_white_table_size);
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
	const int32_t storage = m_storage_by_cartesian[cart];
	// Overlap pairs have no storage id. Callers come from physical board state
	// or from pre-checked-empty pushes, so this never fires for correct callers.
	ASSERT(storage >= 0);
	return storage;
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
