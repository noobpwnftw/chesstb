#include "chess/chess.h"
#include "chess/position.h"

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/symmetry.h"
#include "egtb/king_slice_manager.h"
#include "egtb/pawn_slice_manager.h"
#include "egtb/index_permutation_plan.h"

#include "util/defines.h"
#include "util/math.h"

#include "lz4/lz4.h"

#include <atomic>
#include <climits>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> g_interrupt_requested{ false };
}  // namespace

void egtb_request_interrupt() noexcept
{
	g_interrupt_requested.store(true, std::memory_order_relaxed);
}

bool egtb_is_interrupt_requested() noexcept
{
	return g_interrupt_requested.load(std::memory_order_relaxed);
}

// Position and canonical Board_Index helpers.

std::array<Piece_Group::Placement, PIECE_CLASS_NB>
placements_from_position(const Piece_Config_For_Gen& epsi, const Position& pos)
{
	std::array<Piece_Group::Placement, PIECE_CLASS_NB> out{};

	out[WHITE_KINGS].add(pos.king_square(WHITE));
	out[BLACK_KINGS].add(pos.king_square(BLACK));

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		const Piece pc = epsi.group(c).piece();
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}

	// Pawns are not in populated_classes (they live in the pawn-slice id), but
	// canonicalize_placements still needs them to compute orientation. Collect
	// them whenever pawns can be present -- a free-pawn class is populated, OR a
	// frozen pair contributes pawns even with no free-pawn class (e.g. KpKp).
	const bool has_pair = epsi.pair_group() != nullptr;
	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
	{
		if (!epsi.is_populated(c) && !has_pair) continue;
		const Piece pc = (c == WHITE_PAWNS) ? WHITE_PAWN : BLACK_PAWN;
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}
	return out;
}

Board_Index canonical_board_index(
	const Piece_Config_For_Gen& epsi,
	std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements)
{
	canonicalize_placements(inout_param(placements), epsi);
	const Square wk = placements[WHITE_KINGS][0];
	const Square bk = placements[BLACK_KINGS][0];
	const int32_t king_slice_id = epsi.king_slice_manager().lookup(wk, bk).slice_id;
	if (king_slice_id == SLICE_NONE)
		return BOARD_INDEX_NONE;

	Decomposed_Board_Index dix{};
	dix.king_slice_id = king_slice_id;

	// Pawnless: manager has a single slice (id 0); call collapses.
	if (epsi.pawn_slice_manager().has_pawns())
	{
		const auto& w_pl = placements[WHITE_PAWNS];
		const auto& b_pl = placements[BLACK_PAWNS];

		Square pair_w = SQ_END, pair_b = SQ_END;
		Square free_w[Piece_Group::MAX_PIECE_GROUP_SIZE];
		Square free_b[Piece_Group::MAX_PIECE_GROUP_SIZE];
		size_t nfw = 0, nfb = 0;

		if (epsi.pair_group())
		{
			// Identify the pair (the canonical opposing pair) and treat the rest
			// as free pawns. The enumeration pruned cells where this would pick a
			// different pair, so the slice round-trips.
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

		dix.pawn_slice_id = epsi.pawn_slice_manager().lookup_from_squares(
			pair_w, pair_b,
			Const_Span<Square>(free_w, nfw),
			Const_Span<Square>(free_b, nfb));
	}

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		dix.within[c] = epsi.group(c).compound_index(placements[c]);
	}
	return epsi.compose_board_index(dix);
}

Board_Index board_index_of_position(const Piece_Config_For_Gen& epsi, const Position& pos)
{
	auto p = placements_from_position(epsi, pos);
	return canonical_board_index(epsi, p);
}

// Cap-promo sub-config: remove `cap_idx` AND replace `pawn_idx` with `promo_piece`.
// When `break_pair` is set the parent carries a frozen pair that this capture
// breaks (p -> PP): the two former pair pawns join the child as free pawns, just
// as for a non-promoting capture out of a pair material.
static Piece_Config make_cap_promo_sub(const Piece_Config& ps,
                                       size_t cap_idx, size_t pawn_idx,
                                       Piece promo_piece, bool break_pair)
{
	ASSERT(cap_idx != pawn_idx);
	std::vector<Piece> pieces;
	pieces.reserve(ps.num_pieces() + 1);
	for (size_t k = 0; k < ps.num_pieces(); ++k)
	{
		if (k == cap_idx) continue;
		if (k == pawn_idx) pieces.push_back(promo_piece);
		else               pieces.push_back(ps.pieces()[k]);
	}
	if (break_pair)
	{
		pieces.push_back(WHITE_PAWN);
		pieces.push_back(BLACK_PAWN);
	}
	return Piece_Config(Const_Span<Piece>(pieces.data(), pieces.size()));
}

// Material_Key from pieces with their literal colors (no canonicalization).
static Material_Key material_key_of_pieces(Const_Span<Piece> pieces)
{
	Material_Key k;
	for (Piece p : pieces) k.add_piece(p);
	return k;
}

EGTB_Generator::EGTB_Generator(const Piece_Config& ps) :
	m_epsi(ps),
	m_is_symmetric(false)
{
	const auto [mat_key, mir_key] = m_epsi.material_keys();
	m_is_symmetric = (mat_key == mir_key);

	// Insert sub_ps and fill the (mover, captured, promo) slot. `literal_post_pieces`
	// keep their ORIGINAL colors; sub_ps canonicalizes internally. The mirror flag
	// answers: does the literal post-move position need a color-swap to land at
	// sub_ps's stored (canonical) orientation?
	auto resolve_sub = [&](Color mover, Const_Span<Piece> literal_post_pieces,
	                       const Piece_Config& sub_ps) -> Sub_Entry {
		auto [it, _] = m_sub_epsi_by_material.try_emplace(sub_ps.base_material_key(), sub_ps);
		const Piece_Config_For_Gen* sub_epsi = &it->second;
		Material_Key literal_key = material_key_of_pieces(literal_post_pieces);
		// The pair is symmetric in side strength and excluded from the literal
		// pieces, so stamp it onto the literal key to match the sub-config's key.
		if (sub_ps.has_frozen_pair())
			literal_key.add_pair();
		const bool mirr = (literal_key != sub_ps.base_material_key());
		ASSERT(mirr ? (literal_key == sub_ps.material_keys().second)
		            : (literal_key == sub_ps.base_material_key()));
		// Stm flips to opp(mover) post-move; mirror flips it again.
		return Sub_Entry{ sub_epsi, mirr, color_maybe_opp(color_opp(mover), mirr) };
	};

	auto register_move = [&](Color mover, Piece captured, Piece_Type promo,
	                         Const_Span<Piece> literal_post_pieces,
	                         const Piece_Config& sub_ps) {
		const Sub_Entry e = resolve_sub(mover, literal_post_pieces, sub_ps);
		m_sub_epsi_by_move[mover][captured][promo] = e.epsi;
		m_sub_mirror_by_move[mover][captured][promo] = e.mirror;
		m_sub_read_color_by_move[mover][captured][promo] = e.read_color;
	};

	// Captures: mover is opp of the captured side.
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		if (!ps.can_remove_piece(i)) continue;
		const Piece victim = ps.pieces()[i];
		std::vector<Piece> post;
		post.reserve(ps.num_pieces() + 1);
		for (size_t k = 0; k < ps.num_pieces(); ++k)
			if (k != i) post.push_back(ps.pieces()[k]);
		// With a frozen pair, capturing a free piece lands in the full free-pawn
		// material (pair -> PP) whether the captor is a free piece or a pair pawn,
		// so the child carries no pair; the two former pair pawns become free.
		const Piece_Config sub = ps.has_frozen_pair()
			? ps.pair_broken_by_capture(i)
			: ps.with_removed_piece(i);
		if (ps.has_frozen_pair())
		{
			post.push_back(WHITE_PAWN);
			post.push_back(BLACK_PAWN);
		}
		register_move(color_opp(piece_color(victim)), victim, PIECE_TYPE_NONE,
		              Const_Span<Piece>(post.data(), post.size()),
		              sub);
	}

	// Promotions (no capture): each own pawn has 4 variants.
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color mover = piece_color(pawn);
		for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			const Piece promoted = piece_make(mover, promo);
			std::vector<Piece> post;
			post.reserve(ps.num_pieces());
			for (size_t k = 0; k < ps.num_pieces(); ++k)
				post.push_back(k == i ? promoted : ps.pieces()[k]);
			register_move(mover, PIECE_NONE, promo,
			              Const_Span<Piece>(post.data(), post.size()),
			              ps.with_replaced_piece(i, promoted));
		}
	}

	// Capture-promotions.
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color mover = piece_color(pawn);
		for (size_t j = 0; j < ps.num_pieces(); ++j)
		{
			if (i == j) continue;
			const Piece victim = ps.pieces()[j];
			if (piece_color(victim) == mover) continue;
			if (piece_type(victim) == KING) continue;
			for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			{
				const Piece promoted = piece_make(mover, promo);
				std::vector<Piece> post;
				post.reserve(ps.num_pieces() + 1);
				for (size_t k = 0; k < ps.num_pieces(); ++k)
				{
					if (k == j) continue;
					post.push_back(k == i ? promoted : ps.pieces()[k]);
				}
				const bool break_pair = ps.has_frozen_pair();
				if (break_pair)
				{
					post.push_back(WHITE_PAWN);
					post.push_back(BLACK_PAWN);
				}
				register_move(mover, victim, promo,
				              Const_Span<Piece>(post.data(), post.size()),
				              make_cap_promo_sub(ps, /*cap_idx=*/ j, /*pawn_idx=*/ i,
				                                 promoted, break_pair));
			}
		}
	}

	if (ps.has_frozen_pair())
	{
		for (const Color pc : { WHITE, BLACK })
		{
			const Color mover = color_opp(pc);      // the capturing side
			const Color survivor = color_opp(pc);   // the other pair member
			std::vector<Piece> post(ps.pieces().begin(), ps.pieces().end());
			post.push_back(piece_make(survivor, PAWN));
			m_pair_broken_survivor[pc] =
				resolve_sub(mover, Const_Span<Piece>(post.data(), post.size()),
				            ps.pair_broken_survivor(survivor));
		}
	}
}

Board_Index EGTB_Generator::next_quiet_index(const Position_For_Gen& pos_gen, Move move) const
{
	ASSERT(!move.is_promotion());
	ASSERT(!move.is_ep_capture());

	// Caller has already filtered illegal positions.
	const Position& board = pos_gen.board_unchecked();
	const Square from = move.from();
	const Square to   = move.to();
	const Piece mover = board.piece_at(from);
	ASSERT(mover != PIECE_NONE);
	ASSERT(board.is_empty(to));

	auto fallback = [&]() {
		// board_unchecked() above also populated m_placements; reuse them and
		// patch the moved class instead of re-scanning bitboards.
		auto placements = pos_gen.placements_unchecked();
		placements[piece_class(mover)].move_square(from, to);
		return canonical_board_index(m_epsi, placements);
	};

	if (piece_type(mover) == KING) return fallback();

	if (m_epsi.king_slice_manager().slice_has_stabilizer[pos_gen.index().king_slice_id])
		return fallback();

	if (piece_type(mover) == PAWN)
	{
		// Quiet pawn move is a same-file push: file-mirror orientation preserved
		// and kings unchanged, so only pawn_slice_id moves -- and within it, only
		// the mover's pawn group index. Update it incrementally (O(1) via the
		// group's diff-on-move LUT) instead of re-sorting and re-ranking both
		// colors' pawns from their squares.
		Decomposed_Board_Index dix = pos_gen.index();
		dix.pawn_slice_id = m_epsi.pawn_slice_manager().slice_after_pawn_push(
			dix.pawn_slice_id, piece_color(mover), from, to);
		return m_epsi.compose_board_index(dix);
	}

	const Piece_Class cls = piece_class(mover);
	Decomposed_Board_Index dix = pos_gen.index();
	dix.within[cls] = m_epsi.group(cls).compound_index_after_quiet_move(
		dix.within[cls], from, to);
	return m_epsi.compose_board_index(dix);
}

// Decode (captured, promo) for `m` on `p`. PIECE_NONE if not a capture; handles EP.
static std::pair<Piece, Piece_Type> decode_move_kind(Move m, const Position& p)
{
	Piece captured = PIECE_NONE;
	if (m.is_ep_capture())
	{
		const Square cap_sq = sq_make(sq_rank(m.from()), sq_file(m.to()));
		captured = p.piece_at(cap_sq);
	}
	else if (!p.is_empty(m.to()))
	{
		captured = p.piece_at(m.to());
	}
	const Piece_Type promo = m.is_promotion() ? m.promotion() : PIECE_TYPE_NONE;
	return { captured, promo };
}

Board_Index EGTB_Generator::next_sub_index(
	Position_For_Gen& pos_for_gen, Move move,
	Out_Param<Color> sub_color,
	Out_Param<const Piece_Config_For_Gen*> sub_epsi_out) const
{
	// do_move/undo_move on pos_for_gen's own board: pos_for_gen's cached board
	// matches its index again after the undo, so callers see no change.
	Position& p = pos_for_gen.board_unchecked();
	const Color mover = p.turn();
	const auto [captured, promo] = decode_move_kind(move, p);

	const Piece_Config_For_Gen* sub;
	bool mirr;
	Color read_color;

	bool pair_handled = false;
	if (m_epsi.pair_group() && captured != PIECE_NONE)
	{
		const auto pd = m_epsi.pawn_slice_manager().decompose(
			pos_for_gen.index().pawn_slice_id);
		const Square pair_w = m_epsi.pair_group()->white_square(pd.pair_idx);
		const Square pair_b = m_epsi.pair_group()->black_square(pd.pair_idx);

		const Square cap_sq = move.is_ep_capture()
			? sq_make(sq_rank(move.from()), sq_file(move.to()))
			: move.to();

		if (cap_sq == pair_w || cap_sq == pair_b)
		{
			const Color pc = (cap_sq == pair_w) ? WHITE : BLACK;
			const Sub_Entry& e = m_pair_broken_survivor[pc];
			sub = e.epsi; mirr = e.mirror; read_color = e.read_color;
			pair_handled = true;
		}
	}

	if (!pair_handled)
	{
		sub = m_sub_epsi_by_move[mover][captured][promo];
		mirr = m_sub_mirror_by_move[mover][captured][promo];
		read_color = m_sub_read_color_by_move[mover][captured][promo];
	}
	ASSERT(sub != nullptr);

	*sub_epsi_out = sub;
	*sub_color = read_color;

	const Piece captured_by_move = p.do_move(move);

	Board_Index result;
	if (mirr)
	{
		Position swapped;
		swapped.clear();
		Bitboard occ = p.occupied();
		while (occ)
		{
			const Square sq = occ.pop_first_square();
			swapped.put_piece(piece_opp_color(p.piece_at(sq)), sq_rank_mirror(sq));
		}
		swapped.set_turn(color_opp(p.turn()));
		result = board_index_of_position(*sub, swapped);
	}
	else
	{
		result = board_index_of_position(*sub, p);
	}

	p.undo_move(move, captured_by_move);
	return result;
}

std::map<Material_Key, Piece_Config> EGTB_Generator::enumerate_sub_materials(const Piece_Config& ps)
{
	std::map<Material_Key, Piece_Config> out;
	auto add = [&](Piece_Config sub) {
		out.try_emplace(sub.min_material_key(), std::move(sub));
	};
	// For a frozen pair, sub_configs_by_capture already maps free-piece captures
	// to the full free-pawn material (pair -> PP). Only the pair-member-captured
	// child (p -> P) is extra.
	for (const auto& [_p, sub] : ps.sub_configs_by_capture()) add(sub);
	if (ps.has_frozen_pair())
	{
		add(ps.pair_broken_survivor(WHITE));
		add(ps.pair_broken_survivor(BLACK));
	}
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color pc = piece_color(pawn);
		for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			add(ps.with_replaced_piece(i, piece_make(pc, promo)));
		for (size_t j = 0; j < ps.num_pieces(); ++j)
		{
			if (i == j) continue;
			const Piece v = ps.pieces()[j];
			if (piece_color(v) == pc) continue;
			if (piece_type(v) == KING) continue;
			std::vector<Piece> pieces;
			pieces.reserve(ps.num_pieces() + 1);
			for (size_t k = 0; k < ps.num_pieces(); ++k)
			{
				if (k == j) continue;
				pieces.push_back(k == i ? piece_make(pc, QUEEN) : ps.pieces()[k]);
			}
			if (ps.has_frozen_pair())
			{
				pieces.push_back(WHITE_PAWN);
				pieces.push_back(BLACK_PAWN);
			}
			for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			{
				pieces[i < j ? i : i - 1] = piece_make(pc, promo);
				add(Piece_Config(Const_Span<Piece>(pieces.data(), pieces.size())));
			}
		}
	}
	return out;
}

Shared_Board_Index_Iterator EGTB_Generator::make_slice_group_iterator(
	size_t group_id, size_t slices_per_group) const
{
	const size_t wss = m_epsi.within_slice_size();
	const size_t ntotal = m_epsi.num_slices();
	const size_t g_start_slice = group_id * slices_per_group;
	const size_t g_end_slice   = std::min(g_start_slice + slices_per_group, ntotal);
	return Shared_Board_Index_Iterator(
		static_cast<Board_Index>(g_start_slice * wss),
		static_cast<Board_Index>(g_end_slice   * wss),
		CHUNK_SIZE);
}

Working_Set_Estimate compute_working_set(const Piece_Config& ps, bool include_push_in_iter)
{
	Piece_Config_For_Gen epsi(ps);
	Working_Set_Estimate w{};

	w.num_positions     = epsi.num_positions();
	// Both DTC_Final_Entry and DTM_Final_Entry static_assert to a 2-byte
	// representation, so per-cell sizing uses uint16_t directly. Factor of 2
	// = one table per side-to-move.
	w.total_table_bytes = static_cast<size_t>(2) * w.num_positions * sizeof(uint16_t);

	const size_t within = epsi.within_slice_size();
	w.bytes_per_slice = within * sizeof(uint16_t);

	// Mirrors Sliced_EGTB_File_For_Gen::compute_slices_per_group.
	constexpr size_t MIN_GROUP_BYTES = 64ull * 1024ull * 1024ull;
	if (w.bytes_per_slice == 0 || w.bytes_per_slice >= MIN_GROUP_BYTES)
		w.slices_per_group = 1;
	else
		w.slices_per_group = (MIN_GROUP_BYTES + w.bytes_per_slice - 1) / w.bytes_per_slice;

	w.bytes_per_group = w.slices_per_group * w.bytes_per_slice;
	w.num_slices      = within == 0 ? 0 : (w.num_positions / within);
	w.num_groups      = (w.num_slices == 0)
		? 0
		: (w.num_slices + w.slices_per_group - 1) / w.slices_per_group;

	if (w.num_groups == 0) return w;

	const size_t nks = epsi.num_king_slices();
	const size_t spg = w.slices_per_group;
	const auto& psm = epsi.pawn_slice_manager();
	const auto& ksm = epsi.king_slice_manager();

	// opp_groups == me_groups at pair/batch scope (king neighbors stay within
	// [0, nks)); push_groups is the union of me-ranges of push_target pids.
	// Per-group peaks come from sampling one pair per topo batch.
	auto add_pid_range = [&](int32_t p, std::set<size_t>& dst) {
		const size_t base = static_cast<size_t>(p) * nks;
		if (base >= w.num_slices) return;
		const size_t first_g = base / spg;
		const size_t last_g  = (std::min(base + nks, w.num_slices) - 1) / spg;
		for (size_t g = first_g; g <= last_g; ++g) dst.insert(g);
	};

	King_Slice_Manager::Neighbor_List nbrs;

	for (const auto& batch : psm.pair_topo_batches())
	{
		std::set<size_t> batch_me_groups, batch_push_groups;

		// Per-pair peaks (cheap: O(|members| * nks/spg + |targets| * nks/spg) per pair).
		for (int32_t pair_sid : batch)
		{
			const auto members = psm.pair_members(pair_sid);
			std::set<size_t> pair_me, pair_push;
			for (int32_t pid : members)
			{
				add_pid_range(pid, pair_me);
				add_pid_range(pid, batch_me_groups);
				for (int32_t tpid : psm.push_target_slices(pid))
				{
					add_pid_range(tpid, pair_push);
					add_pid_range(tpid, batch_push_groups);
				}
			}
			size_t opp_pair_iter = pair_me.size();
			if (include_push_in_iter)
			{
				std::set<size_t> opp_union = pair_me;
				opp_union.insert(pair_push.begin(), pair_push.end());
				opp_pair_iter = opp_union.size();
			}
			const size_t pair_iter = pair_me.size() + opp_pair_iter;
			const size_t pair_init = pair_me.size() + pair_push.size();
			update_max(w.peak_pair_iter_groups, pair_iter);
			update_max(w.peak_pair_init_groups, pair_init);
		}

		size_t opp_batch_iter = batch_me_groups.size();
		if (include_push_in_iter)
		{
			std::set<size_t> opp_union = batch_me_groups;
			opp_union.insert(batch_push_groups.begin(), batch_push_groups.end());
			opp_batch_iter = opp_union.size();
		}
		const size_t batch_iter = batch_me_groups.size() + opp_batch_iter;
		const size_t batch_init = batch_me_groups.size() + batch_push_groups.size();
		update_max(w.peak_batch_iter_groups, batch_iter);
		update_max(w.peak_batch_init_groups, batch_init);

		// Per-group peak via a single sample pair from this batch.
		if (batch.empty()) continue;
		const auto sample_members = psm.pair_members(batch.front());

		// Bucket in-pair slices by group, accumulate opp/push, then take max.
		std::map<size_t, std::pair<std::set<size_t>, std::set<size_t>>> by_group;
		for (int32_t pid : sample_members)
		{
			const size_t base = static_cast<size_t>(pid) * nks;
			if (base >= w.num_slices) continue;
			const size_t kid_max = std::min(nks, w.num_slices - base);
			const auto targets = psm.push_target_slices(pid);
			for (size_t kid = 0; kid < kid_max; ++kid)
			{
				const size_t g = (base + kid) / spg;
				auto& bucket = by_group[g];
				ksm.neighbors(static_cast<int32_t>(kid), nbrs);
				for (int32_t pp : sample_members)
				{
					bucket.first.insert((static_cast<size_t>(pp) * nks + kid) / spg);
					for (int32_t k : nbrs)
						bucket.first.insert((static_cast<size_t>(pp) * nks + static_cast<size_t>(k)) / spg);
				}
				for (int32_t tpid : targets)
					bucket.second.insert((static_cast<size_t>(tpid) * nks + kid) / spg);
			}
		}
		for (const auto& [g, sets] : by_group)
		{
			size_t opp_iter = sets.first.size();
			if (include_push_in_iter)
			{
				std::set<size_t> opp_union = sets.first;
				opp_union.insert(sets.second.begin(), sets.second.end());
				opp_iter = opp_union.size();
			}
			const size_t iter_g = 1 + opp_iter;
			const size_t init_g = 1 + sets.second.size();
			update_max(w.peak_per_group_iter_groups, iter_g);
			update_max(w.peak_per_group_init_groups, init_g);
		}
	}

	return w;
}

void EGTB_Generator::set_active_fusion(const Pawn_Slice_Manager& psm,
                                       const std::vector<int32_t>& fusion)
{
	m_active_pawn_slices.clear();
	for (int32_t pair_sid : fusion)
	{
		const auto m = psm.pair_members(pair_sid);
		m_active_pawn_slices.insert(m_active_pawn_slices.end(), m.begin(), m.end());
	}
	std::sort(m_active_pawn_slices.begin(), m_active_pawn_slices.end());
	m_active_pawn_slices.erase(
		std::unique(m_active_pawn_slices.begin(), m_active_pawn_slices.end()),
		m_active_pawn_slices.end());

	m_targets_by_pid.assign(m_epsi.num_pawn_slices(), {});
	for (int32_t pid : m_active_pawn_slices)
		m_targets_by_pid[static_cast<size_t>(pid)] = psm.push_target_slices(pid);
}

template <typename EntryT, typename... OtherEntryTs>
void EGTB_Generator::refresh_active_metadata(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl)
{
	const size_t nps = m_epsi.num_pawn_slices();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = tbl.slices_per_group();
	const size_t ngroups = tbl.num_groups();

	m_pid_in_pair.assign(nps, 0);
	for (int32_t pid : m_active_pawn_slices)
		m_pid_in_pair[static_cast<size_t>(pid)] = 1;

	std::vector<uint8_t> in_pair(ngroups, 0);
	for (int32_t pid : m_active_pawn_slices)
	{
		const size_t base = static_cast<size_t>(pid) * nks;
		for (size_t k = 0; k < nks; ++k)
			in_pair[(base + k) / spg] = 1;
	}

	m_pair_group_ids.clear();
	m_pair_group_ids.reserve(ngroups);
	for (size_t g = 0; g < ngroups; ++g)
		if (in_pair[g]) m_pair_group_ids.push_back(g);
}

template <typename EntryT, typename... OtherEntryTs>
std::vector<std::vector<int32_t>>
EGTB_Generator::compute_fusion_groups(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl,
                                      const std::vector<int32_t>& batch,
                                      size_t resident_layers, bool include_push,
                                      size_t num_workers) const
{
	if (batch.empty()) return {};
	if (m_paging_budget_bytes == 0) return { batch };

	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = tbl.slices_per_group();
	// Eviction compresses one single-layer group; residency spans resident_layers.
	const size_t layer_bytes = spg * m_epsi.within_slice_size() * sizeof(EntryT);
	const size_t bytes_per_group = layer_bytes * resident_layers;
	const size_t reserve = num_workers * spill_compress_scratch_bytes(layer_bytes);
	const size_t effective_budget = m_paging_budget_bytes > reserve + bytes_per_group
		? m_paging_budget_bytes - reserve : bytes_per_group;
	const size_t budget_groups =
		std::max<size_t>(1, effective_budget / bytes_per_group);

	auto pair_groups = [&](int32_t pair_sid) -> std::set<size_t> {
		std::set<size_t> g;
		auto add_pid_range = [&](int32_t pid) {
			const size_t base = static_cast<size_t>(pid) * nks;
			for (size_t k = 0; k < nks; ++k)
				g.insert((base + k) / spg);
		};
		for (int32_t pid : psm.pair_members(pair_sid))
		{
			add_pid_range(pid);
			if (include_push)
				for (int32_t tpid : psm.push_target_slices(pid))
					add_pid_range(tpid);
		}
		return g;
	};

	std::vector<std::vector<int32_t>> fusions;
	fusions.emplace_back();
	std::set<size_t> covered;

	for (int32_t pair_sid : batch)
	{
		const auto pg = pair_groups(pair_sid);
		size_t added = 0;
		for (size_t g : pg) if (!covered.count(g)) ++added;

		if (!fusions.back().empty() && covered.size() + added > budget_groups)
		{
			fusions.emplace_back();
			covered.clear();
		}
		for (size_t g : pg) covered.insert(g);
		fusions.back().push_back(pair_sid);
	}
	if (fusions.back().empty()) fusions.pop_back();
	return fusions;
}

template <typename EntryT, typename... OtherEntryTs>
void EGTB_Generator::apply_working_set(
	In_Out_Param<Thread_Pool> thread_pool,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
	const std::vector<uint8_t>& needed_w,
	const std::vector<uint8_t>& needed_b)
{
	using Table = Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>;
	struct Group_Ref { Table* tbl; size_t group_id; };

	Table* tbls[COLOR_NB] = { w_tbl, b_tbl };
	const std::vector<uint8_t>* needed[COLOR_NB] = { &needed_w, &needed_b };

	const size_t ng = w_tbl->num_groups();
	const size_t num_layers = needed_w.size() / ng;

	const size_t bytes_per_group =
		w_tbl->within_slice_size() * w_tbl->slices_per_group() * sizeof(EntryT);

	++m_paging_tick;

	std::vector<Group_Ref> loads;
	size_t live_groups = 0;
	for (Color c : { WHITE, BLACK })
	{
		const uint8_t* need = needed[c]->data();
		for (size_t layer = 0; layer < num_layers; ++layer)
		{
			Table& tbl = tbls[c][layer];
			ASSERT(tbl.num_groups() == ng);
			const uint8_t* need_layer = need + layer * ng;
			for (size_t group = 0; group < ng; ++group)
			{
				const bool resident = tbl.is_group_resident(group);
				if (need_layer[group])
					tbl.set_last_used(group, m_paging_tick);
				if (need_layer[group] && !resident)
					loads.push_back({ &tbl, group });
				if (resident || need_layer[group])
					++live_groups;
			}
		}
	}

	std::vector<Group_Ref> evicts;
	if (m_paging_budget_bytes > 0)
	{
		// Reserve per-worker compress scratch (eviction runs one save_group_raw per
		// worker below); floor keeps room for one group.
		const size_t reserve =
			thread_pool->num_workers() * spill_compress_scratch_bytes(bytes_per_group);
		const size_t effective_budget = m_paging_budget_bytes > reserve + bytes_per_group
			? m_paging_budget_bytes - reserve : bytes_per_group;
		size_t future_bytes = live_groups * bytes_per_group;
		if (future_bytes > effective_budget)
		{
			struct Candidate { Table* tbl; size_t group; bool dirty; bool exempt; };
			std::vector<Candidate> victims;
			for (Color c : { WHITE, BLACK })
			{
				const uint8_t* need = needed[c]->data();
				for (size_t layer = 0; layer < num_layers; ++layer)
				{
					Table& tbl = tbls[c][layer];
					const uint8_t* need_layer = need + layer * ng;
					const bool exempt = (num_layers > 1 && layer == 0);
					for (size_t group = 0; group < ng; ++group)
						if (!need_layer[group] && tbl.is_group_resident(group))
							victims.push_back({ &tbl, group, tbl.is_group_dirty(group), exempt });
				}
			}
			std::sort(victims.begin(), victims.end(),
				[](const Candidate& lhs, const Candidate& rhs) {
					if (lhs.exempt != rhs.exempt)
						return !lhs.exempt;
					if (lhs.dirty != rhs.dirty)
						return !lhs.dirty;
					return lhs.tbl->last_used(lhs.group) < rhs.tbl->last_used(rhs.group);
				});
			for (const Candidate& victim : victims)
			{
				if (future_bytes <= effective_budget) break;
				evicts.push_back({ victim.tbl, victim.group });
				future_bytes -= bytes_per_group;
			}
		}
	}

	if (evicts.empty() && loads.empty()) return;

	const size_t n = std::max(evicts.size(), loads.size());
	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_all_threads([&](size_t) {
		for (;;)
		{
			const size_t idx = next.fetch_add(1);
			if (idx >= n) return;
			if (idx < evicts.size())
				evicts[idx].tbl->evict_group(evicts[idx].group_id);
			if (idx < loads.size())
				loads[idx].tbl->load_group(loads[idx].group_id);
		}
	});
}

template <typename EntryT, typename... OtherEntryTs>
Block_Source make_entry_block_source(
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& src,
	Save_Group_Cache<EntryT, OtherEntryTs...>& cache,
	Color color,
	Index_Permutation_Plan perm_plan,
	size_t block_size,
	size_t entry_bytes)
{
	constexpr size_t kEntry = sizeof(EntryT);
	ASSERT(block_size % entry_bytes == 0);
	const size_t source_block_bytes = block_size * kEntry / entry_bytes;
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_entries = src.num_slices() * within;
	const size_t source_total_bytes = total_entries * kEntry;
	const size_t output_total_bytes = total_entries * entry_bytes;
	return Block_Source{
		output_total_bytes,
		[&src, &cache, color, within, spg, source_block_bytes, source_total_bytes, perm_plan](size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t> {
			const size_t block_off = block_id * source_block_bytes;
			const size_t this_block = std::min(source_block_bytes, source_total_bytes - block_off);
			ASSERT(scratch.size() >= this_block);
			ASSERT(block_off % kEntry == 0);
			ASSERT(this_block % kEntry == 0);

			const size_t storage_entry_off = block_off / kEntry;
			const size_t entry_cnt = this_block / kEntry;

			const size_t first_g = (storage_entry_off / within) / spg;
			const size_t last_g  = (entry_cnt == 0 ? first_g
			                                       : ((storage_entry_off + entry_cnt - 1) / within) / spg);
			Pinned_Group_Range<EntryT, OtherEntryTs...> pin(cache, color, first_g, last_g);

			for (size_t done = 0; done < entry_cnt; ++done)
			{
				const size_t storage_idx = storage_entry_off + done;
				const size_t logical_idx = storage_index_to_logical_index(perm_plan, storage_idx);
				const auto e = src.template view_at<EntryT>(static_cast<Board_Index>(logical_idx));
				std::memcpy(scratch.data() + done * kEntry, &e, kEntry);
			}

			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

// DTC/DTM gen tables expose both Final and Intermediate views; DTM50 layers
// are single-variant Final.
template void EGTB_Generator::refresh_active_metadata<DTC_Final_Entry, DTC_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&);
template void EGTB_Generator::refresh_active_metadata<DTM_Final_Entry, DTM_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&);
template void EGTB_Generator::refresh_active_metadata<DTM50_Final_Entry, DTM50_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&);

template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTC_Final_Entry, DTC_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, bool, size_t) const;
template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTM_Final_Entry, DTM_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, bool, size_t) const;
template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTM50_Final_Entry, DTM50_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, bool, size_t) const;

template void EGTB_Generator::apply_working_set<DTC_Final_Entry, DTC_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);
template void EGTB_Generator::apply_working_set<DTM_Final_Entry, DTM_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);
template void EGTB_Generator::apply_working_set<DTM50_Final_Entry, DTM50_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);

template Block_Source make_entry_block_source<DTC_Final_Entry, DTC_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&, Save_Group_Cache<DTC_Final_Entry, DTC_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);
template Block_Source make_entry_block_source<DTM_Final_Entry, DTM_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&, Save_Group_Cache<DTM_Final_Entry, DTM_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);
template Block_Source make_entry_block_source<DTM50_Final_Entry, DTM50_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, Save_Group_Cache<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);

// Raw-byte group I/O for paging spill files: LZ4-compressed dump/restore of a
// single resident group, framed by a magic-tagged header.
namespace {

void decompress_bytes_into(const uint8_t* src, size_t src_size,
                           uint8_t* dst, size_t want)
{
	const int ret = LZ4_decompress_safe(
		reinterpret_cast<const char*>(src),
		reinterpret_cast<char*>(dst),
		static_cast<int>(src_size),
		static_cast<int>(want));
	if (ret <= 0 || static_cast<size_t>(ret) != want)
		throw std::runtime_error("Slice LZ4 decompress failed");
}

struct Spill_Header
{
	uint64_t magic;
	uint64_t uncompressed_size;
	uint64_t compressed_size;
};
static_assert(sizeof(Spill_Header) == 24);

}  // namespace

// Groups above LZ4_MAX_INPUT_SIZE are chunked, each chunk framed by its own header.
void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic)
{
	std::ofstream fp(path, std::ios::binary | std::ios::trunc);
	if (!fp) throw std::runtime_error("Could not open spill file for write: " + path.string());

	std::vector<uint8_t> out;
	for (size_t off = 0; off < bytes; off += LZ4_MAX_INPUT_SIZE)
	{
		const size_t remain = bytes - off;
		const int src_size = narrowing_static_cast<int>(remain < LZ4_MAX_INPUT_SIZE ? remain : LZ4_MAX_INPUT_SIZE);
		out.resize(static_cast<size_t>(LZ4_compressBound(src_size)));
		const int n = LZ4_compress_default(
			reinterpret_cast<const char*>(data + off),
			reinterpret_cast<char*>(out.data()),
			src_size,
			narrowing_static_cast<int>(out.size()));
		if (n <= 0) throw std::runtime_error("LZ4 compress failed for spill file: " + path.string());

		Spill_Header hdr{};
		hdr.magic = magic;
		hdr.uncompressed_size = static_cast<uint64_t>(src_size);
		hdr.compressed_size = static_cast<uint64_t>(n);

		fp.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
		fp.write(reinterpret_cast<const char*>(out.data()), n);
		if (!fp) throw std::runtime_error("Write error on spill file: " + path.string());
	}
}

size_t spill_compress_scratch_bytes(size_t bytes_per_group)
{
	const size_t chunk = bytes_per_group < static_cast<size_t>(LZ4_MAX_INPUT_SIZE)
		? bytes_per_group : static_cast<size_t>(LZ4_MAX_INPUT_SIZE);
	return static_cast<size_t>(LZ4_compressBound(narrowing_static_cast<int>(chunk)));
}

void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic)
{
	std::ifstream fp(path, std::ios::binary);
	if (!fp) throw std::runtime_error("Could not open spill file for read: " + path.string());

	std::vector<uint8_t> buf;
	size_t off = 0;
	while (off < bytes)
	{
		Spill_Header hdr{};
		fp.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
		if (!fp) throw std::runtime_error("Spill file truncated header: " + path.string());
		if (hdr.magic != expected_magic)
			throw std::runtime_error("Spill file magic mismatch: " + path.string());
		if (hdr.uncompressed_size == 0 || hdr.uncompressed_size > bytes - off)
			throw std::runtime_error("Spill file size mismatch: " + path.string());

		buf.resize(hdr.compressed_size);
		fp.read(reinterpret_cast<char*>(buf.data()), buf.size());
		if (!fp) throw std::runtime_error("Spill file truncated payload: " + path.string());

		decompress_bytes_into(buf.data(), buf.size(), data + off, hdr.uncompressed_size);
		off += hdr.uncompressed_size;
	}
}
