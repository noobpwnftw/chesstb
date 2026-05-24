#include "egtb/pawn_slice_manager.h"
#include "egtb/piece_group.h"

#include "chess/chess.h"

#include <algorithm>

namespace {

// Plies-to-promotion. Topo key: pushes strictly reduce one pawn's life, so
// lower-total slices are post-push and must be processed first.
INLINE int pawn_life(Color c, Square sq)
{
	const int r = static_cast<int>(sq_rank(sq));
	return c == WHITE ? (static_cast<int>(RANK_8) - r)
	                  : (r - static_cast<int>(RANK_1));
}

}  // namespace

Pawn_Slice_Manager Pawn_Slice_Manager::empty()
{
	Pawn_Slice_Manager m;
	m.m_has_pawns = false;
	m.m_num_slices = 1;
	m.m_pair_topo_batches.push_back({ 0 });
	return m;
}

Pawn_Slice_Manager::Pawn_Slice_Manager(const Piece_Group* white_pawns,
                                       const Piece_Group* black_pawns)
{
	m_has_pawns = true;
	m_white_group = white_pawns;
	m_black_group = black_pawns;
	m_white_table_size = white_pawns ? white_pawns->table_size() : 1;
	m_black_table_size = black_pawns ? black_pawns->table_size() : 1;
	m_num_cartesian_slices = m_white_table_size * m_black_table_size;

	// Assign compact storage ids, skipping overlap (unreachable) pairs.
	m_storage_by_cartesian.assign(m_num_cartesian_slices, -1);
	m_cartesian_by_storage.clear();
	m_cartesian_by_storage.reserve(m_num_cartesian_slices);
	for (size_t cart = 0; cart < m_num_cartesian_slices; ++cart)
	{
		const auto w_idx = static_cast<Piece_Group::Placement_Index>(cart % m_white_table_size);
		const auto b_idx = static_cast<Piece_Group::Placement_Index>(cart / m_white_table_size);
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

	// Sort storage ids by total pawn life for topo wavefront order.
	std::vector<std::pair<int, int32_t>> life_of_slice;
	life_of_slice.reserve(m_num_slices);
	for (int32_t sid = 0; sid < static_cast<int32_t>(m_num_slices); ++sid)
	{
		const Decomposed d = decompose(sid);
		Piece_Group::Placement w_pl, b_pl;
		if (m_white_group) w_pl = m_white_group->squares(d.white_idx);
		if (m_black_group) b_pl = m_black_group->squares(d.black_idx);

		int total_life = 0;
		for (size_t i = 0; i < w_pl.size(); ++i)
			total_life += pawn_life(WHITE, w_pl[i]);
		for (size_t i = 0; i < b_pl.size(); ++i)
			total_life += pawn_life(BLACK, b_pl[i]);
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

	auto mirror_idx = [](const Piece_Group* g, Piece_Group::Placement_Index idx) {
		if (g == nullptr) return idx;
		const auto pl = g->squares(idx);
		const auto mir = pl.with_transformed_squares(
			[](Square s) { return sq_file_mirror(s); });
		return g->compound_index(mir);
	};

	const auto w_mir = mirror_idx(m_white_group, d.white_idx);
	const auto b_mir = mirror_idx(m_black_group, d.black_idx);
	return compose(w_mir, b_mir);
}

Pawn_Slice_Manager::Decomposed Pawn_Slice_Manager::decompose(int32_t slice_id) const
{
	ASSERT(slice_id >= 0 && static_cast<size_t>(slice_id) < m_num_slices);
	Decomposed d;
	if (!m_has_pawns) { d.white_idx = d.black_idx = Piece_Group::Placement_Index(0); return d; }
	const int32_t cart = m_cartesian_by_storage[slice_id];
	d.white_idx = static_cast<Piece_Group::Placement_Index>(cart % m_white_table_size);
	d.black_idx = static_cast<Piece_Group::Placement_Index>(cart / m_white_table_size);
	return d;
}

int32_t Pawn_Slice_Manager::compose(Piece_Group::Placement_Index w,
                                    Piece_Group::Placement_Index b) const
{
	if (!m_has_pawns) return 0;
	const size_t cart = static_cast<size_t>(w) + static_cast<size_t>(b) * m_white_table_size;
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

	std::vector<int32_t> out;

	auto occupied = [&](Square sq) {
		for (size_t i = 0; i < w_pl.size(); ++i) if (w_pl[i] == sq) return true;
		for (size_t i = 0; i < b_pl.size(); ++i) if (b_pl[i] == sq) return true;
		return false;
	};

	auto emit_white_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_w = w_pl.with_moved_square(from, to);
		const auto new_w_idx = m_white_group->compound_index(new_w);
		out.push_back(compose(new_w_idx, d.black_idx));
	};
	auto emit_black_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_b = b_pl.with_moved_square(from, to);
		const auto new_b_idx = m_black_group->compound_index(new_b);
		out.push_back(compose(d.white_idx, new_b_idx));
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

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
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
