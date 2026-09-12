#include "chess/bitboard.h"
#include "chess/pawn_slice_bits.h"
#include "chess/chess.h"
#include "chess/pair_group.h"

#include "egtb/pawn_slice_manager.h"
#include "egtb/piece_group.h"

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

	auto placements_of = [](const Piece_Group* g) {
		std::vector<Bitboard> out;
		for (size_t i = 0; i < (g ? g->table_size() : 1); ++i)
			out.push_back(g ? g->squares(static_cast<Piece_Group::Placement_Index>(i)).occupancy()
			                : Bitboard::make_empty());
		return out;
	};

	// The bitmap is shared by rank (compose) and the reverse storage map.
	// Build it by bitsets, then enumerate its set bits once in storage order.
	m_rank_blocks.assign((m_num_cartesian_slices + 63) / 64, Rank_Block{});
	m_num_slices = 0;
	build_pawn_slice_bits(pair, placements_of(m_white_group), placements_of(m_black_group),
		[&](size_t cart, uint64_t bits) {
			if (!bits) return;
			m_num_slices += popcnt(bits);
			const size_t word = cart >> 6, shift = cart & 63;
			m_rank_blocks[word].bits |= bits << shift;
			if (shift && word + 1 < m_rank_blocks.size())
				m_rank_blocks[word + 1].bits |= bits >> (64 - shift);
		});
	m_cartesian_by_storage.reserve(m_num_slices);
	int32_t running = 0;
	for (size_t word = 0; word < m_rank_blocks.size(); ++word)
	{
		Rank_Block& block = m_rank_blocks[word];
		block.survivors_before = running;
		running += static_cast<int32_t>(popcnt(block.bits));
		uint64_t bits = block.bits;
		while (bits)
			m_cartesian_by_storage.push_back(static_cast<int32_t>(word * 64 + pop_first_bit(bits)));
	}
	ASSERT(static_cast<size_t>(running) == m_num_slices);
	ASSERT(m_cartesian_by_storage.size() == m_num_slices);
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
	Bitboard white = m_white_group ? m_white_group->squares(d.white_idx).occupancy() : Bitboard::make_empty();
	Bitboard black = m_black_group ? m_black_group->squares(d.black_idx).occupancy() : Bitboard::make_empty();
	Square pair_w = SQ_END, pair_b = SQ_END;
	if (m_pair_group)
	{
		white |= m_pair_group->white_square(d.pair_idx);
		black |= m_pair_group->black_square(d.pair_idx);
	}
	white = white.mirror_files();
	black = black.mirror_files();
	if (m_pair_group)
	{
		Pair_Group::canonical_pair(white, black, pair_w, pair_b);
		white ^= pair_w;
		black ^= pair_b;
	}
	return lookup(pair_w, pair_b, white, black);
}

int32_t Pawn_Slice_Manager::lookup(Square pair_white_sq, Square pair_black_sq,
                                   Bitboard white, Bitboard black) const
{
	if (!m_has_pawns) return 0;
	const Pair_Group::Index pair_idx =
		m_pair_group ? m_pair_group->index_of(pair_white_sq, pair_black_sq) : 0;
	const Piece_Group::Placement_Index w_idx = m_white_group ? m_white_group->index_of(white) : 0;
	const Piece_Group::Placement_Index b_idx = m_black_group ? m_black_group->index_of(black) : 0;
	return compose(pair_idx, w_idx, b_idx);
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
	ASSERT(cartesian_survives(cart));
	return storage_of_cartesian(cart);
}

void Pawn_Slice_Manager::push_target_slices(int32_t slice_id, std::vector<int32_t>& out) const
{
	out.clear();
	if (!m_has_pawns) return;
	const Decomposed d = decompose(slice_id);

	Piece_Group::Placement w_pl, b_pl;
	if (m_white_group) w_pl = m_white_group->squares(d.white_idx);
	if (m_black_group) b_pl = m_black_group->squares(d.black_idx);

	const Square pair_w = m_pair_group ? m_pair_group->white_square(d.pair_idx) : SQ_END;
	const Square pair_b = m_pair_group ? m_pair_group->black_square(d.pair_idx) : SQ_END;

	auto occupied = [&](Square sq) {
		if (m_pair_group && (sq == pair_w || sq == pair_b)) return true;
		for (size_t i = 0; i < w_pl.size(); ++i) if (w_pl[i] == sq) return true;
		for (size_t i = 0; i < b_pl.size(); ++i) if (b_pl[i] == sq) return true;
		return false;
	};

	auto emit_white_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_w_idx = m_white_group->compound_index_after_quiet_move(w_pl, from, to);
		out.push_back(compose(d.pair_idx, new_w_idx, d.black_idx));
	};
	auto emit_black_push = [&](Square from, Square to) {
		if (occupied(to)) return;
		const auto new_b_idx = m_black_group->compound_index_after_quiet_move(b_pl, from, to);
		out.push_back(compose(d.pair_idx, d.white_idx, new_b_idx));
	};

	if (m_white_group)
	{
		for (size_t i = 0; i < w_pl.size(); ++i)
		{
			const Square from = w_pl[i];
			const Rank r = sq_rank(from);
			if (r == RANK_7 || r == RANK_8 || r == RANK_1) continue;
			const Square to1 = sq_make(static_cast<Rank>(r + 1), sq_file(from));
			if (occupied(to1)) continue;
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
			w_idx = m_white_group->compound_index_after_quiet_move(
				m_white_group->squares(d.white_idx), from, to);
	}
	else
	{
		if (m_pair_group && from == m_pair_group->black_square(d.pair_idx))
			pair_idx = m_pair_group->index_of(m_pair_group->white_square(d.pair_idx), to);
		else
			b_idx = m_black_group->compound_index_after_quiet_move(
				m_black_group->squares(d.black_idx), from, to);
	}
	return compose(pair_idx, w_idx, b_idx);
}
