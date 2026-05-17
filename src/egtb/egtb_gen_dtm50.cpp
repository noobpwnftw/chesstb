#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_compress.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/defines.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"
#include "util/progress_bar.h"
#include "util/utility.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

DTM50_Generator::DTM50_Generator(
	const Piece_Config& ps,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTM50_Table>(ps, tmp_dir))
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	// Budget is "unbounded" only if it fits the save-time peak (all HMC_COUNT
	// layers × 2 colors).
	const size_t bytes_per_color =
		m_table->m_dtm[WHITE][0].num_slices()
		* m_table->m_dtm[WHITE][0].within_slice_size()
		* sizeof(DTM50_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB * DTM50_HMC_COUNT;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;
}

DTM50_Final_Entry DTM50_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move) const
{
	Color sub_color;
	const Piece_Config_For_Gen* sub_epsi = nullptr;
	const Board_Index sub_idx = next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

	if (sub_epsi == nullptr) return DTM50_Final_Entry::make_draw();
	auto it = m_sub_dtm_by_material.find(sub_epsi->min_material_key());
	if (it == m_sub_dtm_by_material.end()) return DTM50_Final_Entry::make_draw();
	return it->second->read(sub_color, sub_idx);
}

DTM50_Final_Entry DTM50_Generator::read_post_move_dtm(Position_For_Gen& pos_gen, Move move, uint16_t hmc) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);

	const Color mover = parent.turn();
	const Color opp = color_opp(mover);
	const Board_Index post_idx = next_quiet_index(pos_gen, move);

	// Pawn push → opp[0]; non-pawn quiet → opp[k+1]; k=99 targets virtual
	// hmc=100 (50MR draw unless mate). See header for build-order argument.
	const bool is_pawn_push = piece_type(parent.piece_at(move.from())) == PAWN;
	if (is_pawn_push)
		return read_dtm50<DTM50_Final_Entry>(opp, 0, post_idx);

	const uint16_t target_hmc = static_cast<uint16_t>(hmc + 1);
	if (target_hmc >= DTM50_HMC_COUNT)
	{
		Position child = parent;
		(void)child.do_move(move);
		if (!child.is_in_check(child.turn())) return DTM50_Final_Entry::make_draw();
		Move_List ml;
		child.gen_pseudo_legal_moves(out_param(ml));
		const Position::Legality ctx = child.legality_context();
		for (size_t i = 0; i < ml.size(); ++i)
			if (child.is_pseudo_legal_move_legal(ml[i], ctx)) return DTM50_Final_Entry::make_draw();
		return DTM50_Final_Entry::make_loss(0);
	}
	return read_dtm50<DTM50_Final_Entry>(opp, target_hmc, post_idx);
}

namespace {

// True if `a` is strictly better for the mover: highest class, then shorter
// WIN / longer LOSS.
INLINE bool dtm_better_for_mover(DTM50_Final_Entry a, DTM50_Final_Entry b)
{
	auto rank = [](DTM50_Final_Entry e) -> int {
		if (e.is_illegal()) return -1;
		if (e.is_win())     return 2;
		if (e.is_loss())    return 0;
		return 1;  // DRAW
	};
	const int ra = rank(a), rb = rank(b);
	if (ra != rb) return ra > rb;
	if (a.is_win())  return a.value() < b.value();
	if (a.is_loss()) return a.value() > b.value();
	return false;
}

struct DTM50_Entry_Builder
{
	uint16_t best_win_dtm  = std::numeric_limits<uint16_t>::max();
	uint16_t best_loss_dtm = 0;
	bool saw_win  = false;
	bool saw_draw = false;

	INLINE DTM50_Entry_Builder& operator+=(DTM50_Final_Entry child_e)
	{
		if (child_e.is_illegal()) return *this;
		if (child_e.is_loss())
		{
			saw_win = true;
			update_min(best_win_dtm, static_cast<uint16_t>(child_e.value() + 1));
		}
		else if (child_e.is_win())
		{
			update_max(best_loss_dtm, static_cast<uint16_t>(child_e.value() + 1));
		}
		else
		{
			saw_draw = true;
		}
		return *this;
	}

	INLINE DTM50_Entry_Builder& operator+=(const DTM50_Entry_Builder& o)
	{
		if (o.saw_win) { saw_win = true; update_min(best_win_dtm, o.best_win_dtm); }
		if (o.saw_draw) saw_draw = true;
		update_max(best_loss_dtm, o.best_loss_dtm);
		return *this;
	}

	INLINE DTM50_Entry_Builder& operator+=(DTM50_Intermediate_Entry c)
	{
		if (c.is_win())       { saw_win = true; update_min(best_win_dtm, static_cast<uint16_t>(c.value())); }
		else if (c.is_loss())   update_max(best_loss_dtm, static_cast<uint16_t>(c.value()));
		else                    saw_draw = true;
		return *this;
	}

	template <class EntryT>
	INLINE operator EntryT() const
	{
		static_assert(std::is_same_v<EntryT, DTM50_Final_Entry> ||
		              std::is_same_v<EntryT, DTM50_Intermediate_Entry>);
		if (saw_win)           return EntryT::make_win(best_win_dtm);
		if (saw_draw)          return EntryT::make_draw();
		if (best_loss_dtm > 0) return EntryT::make_loss(best_loss_dtm);
		return EntryT::make_draw();
	}
};

}  // namespace

DTM50_Final_Entry DTM50_Generator::effective_opp_dtm_after_dp(Position_For_Gen& pos_gen, Move dp_move, uint16_t hmc) const
{
	const DTM50_Final_Entry no_ep = read_post_move_dtm(pos_gen, dp_move, hmc);

	// do_move/undo_move on pos_gen's own board: pos_gen's cached board matches
	// its index again after the undo, so callers see no change.
	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const Piece captured_by_dp = p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	DTM50_Final_Entry best_ep_for_opp = DTM50_Final_Entry::make_loss(0);  // worst for opp; bumped on first EP
	bool any_ep = false;

	std::optional<Position_For_Gen> p_gen_for_ep;

	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(push_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square own_pawn_sq = sq_make(opp_ep_rank, static_cast<File>(f));
		if (p.piece_at(own_pawn_sq) != piece_make(opp, PAWN)) continue;
		const Square ep_to_sq = sq_make(ep_target_rank, push_file);
		if (p.piece_at(ep_to_sq) != PIECE_NONE) continue;
		const Move ep_move = Move::make_ep_capture(own_pawn_sq, ep_to_sq);
		if (!p.is_pseudo_legal_move_legal(ep_move)) continue;

		if (!p_gen_for_ep)
		{
			const Board_Index child_idx = board_index_of_position(m_epsi, p);
			p_gen_for_ep.emplace(m_epsi, child_idx, opp);
		}
		// Post-EP DTM is from new STM (mover) — invert class and +1 ply for opp's EP.
		const DTM50_Final_Entry after_ep = read_sub_tb(*p_gen_for_ep, ep_move);
		DTM50_Final_Entry opp_at_pre_ep;
		if (after_ep.is_illegal())          continue;
		else if (after_ep.is_win())         opp_at_pre_ep = DTM50_Final_Entry::make_loss(after_ep.value() + 1);
		else if (after_ep.is_loss())        opp_at_pre_ep = DTM50_Final_Entry::make_win(after_ep.value() + 1);
		else                                opp_at_pre_ep = DTM50_Final_Entry::make_draw();

		if (!any_ep || dtm_better_for_mover(opp_at_pre_ep, best_ep_for_opp))
			best_ep_for_opp = opp_at_pre_ep;
		any_ep = true;
	}

	p.undo_move(dp_move, captured_by_dp);

	if (!any_ep) return no_ep;
	return dtm_better_for_mover(best_ep_for_opp, no_ep) ? best_ep_for_opp : no_ep;
}

DTM50_Final_Entry DTM50_Generator::make_initial_entry(Position_For_Gen& pos_gen, uint16_t hmc)
{
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM50_Final_Entry::make_illegal();

	Position& pos = pos_gen.board_unchecked();

	DTM50_Entry_Builder inv;
	DTM50_Entry_Builder dep;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	bool any_legal = false;
	bool any_invariant = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;
		const bool invariant = is_pawn || !pos.is_empty(m.to());
		const DTM50_Final_Entry child_e = is_pawn && is_pawn_double_push(m)
			? effective_opp_dtm_after_dp(pos_gen, m, hmc)
			: read_post_move_dtm(pos_gen, m, hmc);
		if (invariant)
		{
			inv += child_e;
			any_invariant = true;
		}
		else
			dep += child_e;
	}

	if (!any_legal)
		return ctx.in_check
			? DTM50_Final_Entry::make_loss(0)
			: DTM50_Final_Entry::make_draw();

	if (any_invariant)
	{
		write_dtm50<DTM50_Intermediate_Entry>(pos.turn(), 0, pos_gen.board_index(), inv);
		dep += inv;
	}
	return dep;
}

DTM50_Final_Entry DTM50_Generator::make_layer_entry(Position_For_Gen& pos_gen, DTM50_Intermediate_Entry inv, uint16_t hmc) const
{
	Position& pos = pos_gen.board_unchecked();

	DTM50_Entry_Builder dep;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	bool any_legal = false;
	bool any_invariant = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;
		const bool invariant = is_pawn || !pos.is_empty(m.to());
		if (invariant)
		{
			any_invariant = true;
			continue;
		}
		dep += read_post_move_dtm(pos_gen, m, hmc);
	}

	if (!any_legal)
		return ctx.in_check
			? DTM50_Final_Entry::make_loss(0)
			: DTM50_Final_Entry::make_draw();

	if (any_invariant) dep += inv;
	return dep;
}

void DTM50_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	constexpr uint16_t hmc = DTM50_HMC_COUNT - 1;
	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	// Slice topology is layer-independent; any layer's metadata works.
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;
	const auto& ksm = m_epsi.king_slice_manager();
	const bool has_pawns = psm.has_pawns();

	const auto& targets_by_pid = m_targets_by_pid;

	// Working set per group: [hmc] holds only group g (the write target — the
	// first layer's quiet moves target virtual hmc=100, so nothing in [hmc] is
	// read back), and [0] holds the pawn-push read closure (king-neighbor /
	// push-target groups; a push can re-canonicalise into a neighbor king-slice).
	// needs[B] == needs[W].
	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		// [hmc] write target: group g only. [0] pawn-push read closure (g stays set).
		m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
		m_scratch_need[BLACK].assign(ngroups * DTM50_HMC_COUNT, 0);
		auto* need_w0 = m_scratch_need[WHITE].data();
		auto* need_wh = m_scratch_need[WHITE].data() + static_cast<size_t>(hmc) * ngroups;
		auto* need_b0 = m_scratch_need[BLACK].data();
		auto* need_bh = m_scratch_need[BLACK].data() + static_cast<size_t>(hmc) * ngroups;
		if (4 * ngroups * spg * m_epsi.within_slice_size() * sizeof(DTM50_Final_Entry) <= m_paging_budget_bytes)
		{
			std::fill_n(need_w0, ngroups, 1);
			std::fill_n(need_wh, ngroups, 1);
			std::fill_n(need_b0, ngroups, 1);
			std::fill_n(need_bh, ngroups, 1);
		}
		else if ((2 * ngroups + 2) * spg * m_epsi.within_slice_size() * sizeof(DTM50_Final_Entry) <= m_paging_budget_bytes)
		{
			need_wh[g] = 1;
			need_bh[g] = 1;
			std::fill_n(need_w0, ngroups, 1);
			std::fill_n(need_b0, ngroups, 1);
		}
		else
		{
			need_wh[g] = 1;
			need_bh[g] = 1;
			need_w0[g] = 1;
			const size_t g_start = g * spg;
			const size_t g_end   = std::min(g_start + spg, ntotal);
			for (int32_t pid : m_active_pawn_slices)
			{
				const size_t pid_base = static_cast<size_t>(pid) * nks;
				const size_t s_lo = std::max(g_start, pid_base);
				const size_t s_hi = std::min(g_end,   pid_base + nks);
				if (s_lo >= s_hi) continue;
				for (size_t s = s_lo; s < s_hi; ++s)
				{
					const int32_t kid = static_cast<int32_t>(s - pid_base);
					ksm.neighbors(kid, m_scratch_nbrs);
					for (int32_t pp : m_active_pawn_slices)
					{
						const size_t same = static_cast<size_t>(pp) * nks + static_cast<size_t>(kid);
						need_w0[same / spg] = 1;
						for (int32_t k : m_scratch_nbrs)
						{
							const size_t neigh = static_cast<size_t>(pp) * nks + static_cast<size_t>(k);
							need_w0[neigh / spg] = 1;
						}
					}
					if (has_pawns)
					{
						for (int32_t tpid : targets_by_pid[static_cast<size_t>(pid)])
						{
							const size_t target =
								static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
							need_w0[target / spg] = 1;
						}
					}
				}
			}
			std::copy_n(need_w0, ngroups, need_b0);
		}
		apply_working_set(thread_pool,
			&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK]);
	};

	const size_t wss = m_epsi.within_slice_size();
	size_t total_indices = 0;
	for (size_t g : m_pair_group_ids)
	{
		const size_t g_start_slice = g * spg;
		const size_t g_end_slice = std::min(g_start_slice + spg, ntotal);
		total_indices += (g_end_slice - g_start_slice) * wss;
	}
	const size_t PRINT_PERIOD = thread_pool->num_workers() * (1 << 20);
	Concurrent_Progress_Bar progress_bar(total_indices, PRINT_PERIOD, "init_entries");

	for (size_t g : m_pair_group_ids)
	{
		page_in_for_init_group(g);

		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			size_t local_progress = 0;
			const auto& slice_has_stab = m_epsi.king_slice_manager().slice_has_stabilizer;
			for (const Board_Index idx : group_it.indices())
			{
				if (++local_progress % PROGRESS_BAR_UPDATE_PERIOD == 0)
					progress_bar += PROGRESS_BAR_UPDATE_PERIOD;
				const size_t pid_of_idx = m_epsi.pawn_slice_of(idx);
				if (!pid_in_pair[pid_of_idx])
				{
					prev = BOARD_INDEX_NONE;
					continue;
				}

				if (prev != BOARD_INDEX_NONE
					&& static_cast<size_t>(idx) == static_cast<size_t>(prev) + 1)
					++pos_gen;
				else
					pos_gen.set_board_index(idx);
				prev = idx;

				if (!pos_gen.is_legal())
				{
					write_dtm50(WHITE, 0, idx, DTM50_Final_Entry::make_illegal());
					write_dtm50(BLACK, 0, idx, DTM50_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board_unchecked());
					if (canon != idx)
					{
						write_dtm50(WHITE, 0, idx, DTM50_Final_Entry::make_illegal());
						write_dtm50(BLACK, 0, idx, DTM50_Final_Entry::make_illegal());
						continue;
					}
				}
				else
				{
					ASSERT(board_index_of_position(m_epsi, pos_gen.board_unchecked()) == idx);
				}
				for (Color us : { WHITE, BLACK })
				{
					pos_gen.set_turn(us);
					const auto e = make_initial_entry(pos_gen, hmc);
					// Mark the legal cell per color; lower layers visit only their
					// color's marked chunks.
					if (!e.is_illegal())
					{
						write_dtm50(us, hmc, idx, e);
						mark_iter(us, idx, m_table->m_dtm[us][0]);
					}
					else
						write_dtm50(us, 0, idx, e);
				}
			}
		});
	}

	progress_bar.set_finished();
}

template <Color me>
void DTM50_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                        size_t group_id, uint16_t hmc)
{
	if (m_paging_budget_bytes == 0) return;
	constexpr Color opp = (me == WHITE) ? BLACK : WHITE;
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& ksm = m_epsi.king_slice_manager();
	const auto& psm = m_epsi.pawn_slice_manager();
	const bool has_pawns = psm.has_pawns();

	m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
	m_scratch_need[BLACK].assign(ngroups * DTM50_HMC_COUNT, 0);
	auto* need_me0  = m_scratch_need[me].data();
	auto* need_me   = m_scratch_need[me].data() + static_cast<size_t>(hmc) * ngroups;
	auto* need_opp0 = m_scratch_need[opp].data();
	auto* need_opp  = m_scratch_need[opp].data() + static_cast<size_t>(hmc + 1) * ngroups;

	if (4 * ngroups * spg * m_epsi.within_slice_size() * sizeof(DTM50_Final_Entry) <= m_paging_budget_bytes)
	{
		std::fill_n(need_me0,  ngroups, 1);
		std::fill_n(need_me,   ngroups, 1);
		std::fill_n(need_opp0, ngroups, 1);
		std::fill_n(need_opp,  ngroups, 1);
	}
	else if ((2 * ngroups + 2) * spg * m_epsi.within_slice_size() * sizeof(DTM50_Final_Entry) <= m_paging_budget_bytes)
	{
		need_me0[group_id] = 1;
		need_me[group_id]  = 1;
		std::fill_n(need_opp0, ngroups, 1);
		std::fill_n(need_opp,  ngroups, 1);
	}
	else
	{
		need_me0[group_id] = 1;
		need_me[group_id]  = 1;
		const size_t g_start = group_id * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		for (int32_t pid : m_active_pawn_slices)
		{
			const size_t pid_base = static_cast<size_t>(pid) * nks;
			const size_t s_lo = std::max(g_start, pid_base);
			const size_t s_hi = std::min(g_end,   pid_base + nks);
			if (s_lo >= s_hi) continue;
			for (size_t s = s_lo; s < s_hi; ++s)
			{
				const int32_t kid = static_cast<int32_t>(s - pid_base);
				ksm.neighbors(kid, m_scratch_nbrs);
				for (int32_t pp : m_active_pawn_slices)
				{
					need_opp[(static_cast<size_t>(pp) * nks + static_cast<size_t>(kid)) / spg] = 1;
					for (int32_t k : m_scratch_nbrs)
						need_opp[(static_cast<size_t>(pp) * nks + static_cast<size_t>(k)) / spg] = 1;
				}
				if (has_pawns)
					for (int32_t tpid : m_targets_by_pid[static_cast<size_t>(pid)])
						need_opp[(static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid)) / spg] = 1;
			}
		}
		std::copy_n(need_opp, ngroups, need_opp0);
	}

	apply_working_set(thread_pool,
		&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
		m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

template <Color stm>
void DTM50_Generator::build_layer(In_Out_Param<Thread_Pool> thread_pool, uint16_t hmc)
{
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const auto& pid_in_pair = m_pid_in_pair;

	for (size_t g : m_pair_group_ids)
	{
		if (m_iter_groups[stm][g] == 0) continue;

		page_in_for_group<stm>(thread_pool, g, hmc);

		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> bool {
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
			Board_Index prev = BOARD_INDEX_NONE;
			bool any_legal_local = false;
			for (auto [chunk_start, chunk_end] : group_it.chunks())
			{
				const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
				if (!m_iter_chunks[stm][cid]) continue;
				bool any_legal = false;
				for (Board_Index idx = chunk_start; idx != chunk_end;
				     idx = static_cast<Board_Index>(static_cast<size_t>(idx) + 1))
				{
					const size_t pid_of_idx = m_epsi.pawn_slice_of(idx);
					if (!pid_in_pair[pid_of_idx]) continue;

					// hmc=0 doubles as the ILLEGAL marker (legality is hmc-invariant)
					// and the cached invariant summary.
					const DTM50_Final_Entry fe = read_dtm50<DTM50_Final_Entry>(stm, 0, idx);
					if (fe.is_illegal()) continue;
					const auto& ie = reinterpret_cast<const DTM50_Intermediate_Entry&>(fe);
					// Legal: advance pos_gen lazily, only for classified cells.
					if (prev != BOARD_INDEX_NONE
						&& static_cast<size_t>(idx) == static_cast<size_t>(prev) + 1)
						++pos_gen;
					else
						pos_gen.set_board_index(idx);
					prev = idx;

					write_dtm50(stm, hmc, idx, make_layer_entry(pos_gen, ie, hmc));
					any_legal = true;
				}
				if (!any_legal
					&& static_cast<size_t>(chunk_end) - static_cast<size_t>(chunk_start) == CHUNK_SIZE)
					m_iter_chunks[stm][cid] = 0;
				any_legal_local |= any_legal;
			}
			return any_legal_local;
		});
		bool any_legal_group = false;
		for (bool r : rets) any_legal_group |= r;
		if (!any_legal_group)
			m_iter_groups[stm][g] = 0;
	}
}

void DTM50_Generator::gen(
	std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<DTM50_Final_Entry>>> sub_dtm,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_dtm_by_material = std::move(sub_dtm);

	const auto ckpt_path = paths.dtm50_checkpoint_path(m_epsi);
	int64_t resume_batch_idx = -1;
	int64_t resume_fusion_idx = -1;
	int64_t resume_hmc = -1;
	{
		Checkpoint_File ckpt{};
		if (read_checkpoint(ckpt_path, &ckpt))
		{
			resume_batch_idx = static_cast<int64_t>(ckpt.batch_idx);
			resume_fusion_idx = static_cast<int64_t>(ckpt.fusion_idx);
			resume_hmc = static_cast<int64_t>(ckpt.hmc);
		}
		else
		{
			std::atomic<size_t> next(0);
			thread_pool->run_sync_task_on_all_threads([&](size_t) {
				for (;;)
				{
					const size_t h = next.fetch_add(1, std::memory_order_relaxed);
					if (h >= DTM50_HMC_COUNT) return;
					m_table->m_dtm[WHITE][h].remove_disk_files();
					m_table->m_dtm[BLACK][h].remove_disk_files();
				}
			});
		}
	}
	remove_checkpoint(ckpt_path);

	// Outer = topo batch / fusion, inner = hmc 99..0; see header for the
	// read-dependency argument.
	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtm[WHITE][0], batch, 2, true);
		total_fusions += fusions.size();
		if (pawnful)
		{
			std::printf("  batch %zu/%zu (%zu pairs in %zu fusion%s)\n",
				bi + 1, batches.size(), batch.size(),
				fusions.size(), fusions.size() == 1 ? "" : "s");
			std::fflush(stdout);
		}
		for (size_t fi = 0; fi < fusions.size(); ++fi)
		{
			const bool is_resume_fusion =
				static_cast<int64_t>(bi) == resume_batch_idx &&
				static_cast<int64_t>(fi) == resume_fusion_idx;
			if (static_cast<int64_t>(bi) == resume_batch_idx &&
			    static_cast<int64_t>(fi) < resume_fusion_idx)
				continue;

			const auto& fusion = fusions[fi];

			set_active_fusion(psm, fusion);

			init_iter_state(
				m_table->m_dtm[WHITE][0].num_groups(),
				m_table->m_dtm[WHITE][0].num_entries());
			if (is_resume_fusion && resume_hmc < static_cast<int64_t>(DTM50_HMC_COUNT) - 1)
				seed_iter_groups();

			for (uint16_t hmc = DTM50_HMC_COUNT; hmc-- > 0; )
			{
				// Resume fusion: layers above resume_hmc are already done.
				if (is_resume_fusion && static_cast<int64_t>(hmc) > resume_hmc) continue;

				try
				{
					if (egtb_is_interrupt_requested())
						throw DTM50_Interrupted{ 0, 0, hmc };

					// Unbounded budget: pre-load every group of the read layers
					// (cur + opp[0] + opp[k+1]). Bounded mode pages per group.
					if (m_paging_budget_bytes == 0)
					{
						const size_t ng = m_table->m_dtm[WHITE][hmc].num_groups();
						std::vector<uint8_t> all_needed(ng, 1);
						apply_working_set(thread_pool,
							&m_table->m_dtm[WHITE][hmc], &m_table->m_dtm[BLACK][hmc],
							all_needed, all_needed);
						apply_working_set(thread_pool,
							&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
							all_needed, all_needed);
						if (hmc + 1u < DTM50_HMC_COUNT)
						{
							apply_working_set(thread_pool,
								&m_table->m_dtm[WHITE][hmc + 1], &m_table->m_dtm[BLACK][hmc + 1],
								all_needed, all_needed);
						}
					}

					refresh_active_metadata(m_table->m_dtm[WHITE][0]);

					if (hmc == DTM50_HMC_COUNT - 1)
					{
						init_entries(thread_pool);
					}
					else
					{
						std::printf("  build layer %2u\r", hmc);
						std::fflush(stdout);
						build_layer<WHITE>(thread_pool, hmc);
						build_layer<BLACK>(thread_pool, hmc);
					}
				}
				catch (const DTM50_Interrupted& e)
				{
					for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
					{
						m_table->m_dtm[WHITE][h].evict_all();
						m_table->m_dtm[BLACK][h].evict_all();
					}
					Checkpoint_File ckpt{};
					ckpt.batch_idx = static_cast<uint32_t>(bi);
					ckpt.fusion_idx = static_cast<uint32_t>(fi);
					ckpt.hmc = e.hmc;
					write_checkpoint(ckpt_path, ckpt);
					std::printf("\n  interrupted: checkpoint written (bi=%zu, fi=%zu, hmc=%u)\n",
						bi, fi, e.hmc);
					std::fflush(stdout);
					throw;
				}
			}
		}
	}

	m_sub_dtm_by_material.clear();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (%zu hmc layers): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

// save_to_disk: pack all 101 layers (unbounded-DTM base + 100 hmc) into one
// .lzdtm50 plus a .info sidecar (hmc=0 stats). Design rationale (2-bit state,
// draw-end hint, uniform-skip sentinel, DRAW excluded from rank table) lives
// in README "Pack layout".
//
// File layout:
//   uint32  magic = EGTB_Magic::DTM50_MAGIC
//   uint32  key_and_table_num
//   per-color header:
//     uint8  flag (SINGULAR bit set ⇒ singular-WDL color)
//     if SINGULAR: uint8 0
//     else:
//       uint32 index_perm   (populated-class storage-order permutation index)
//       uint8  entry_bytes  (1 or 2)
//       uint32 block_positions
//       uint64 block_cnt
//       uint32 tail_positions
//       uint64 data_size
//       uint16 num_ranks
//       uint16 rank_to_value[num_ranks]    W/L storage values, frequency-sorted
//   per-color offset section (non-singular): delta-coded succinct index
//     [u8 log2_bu, sample_width, offset_width, usz_width]
//     Mono_Uint_Vec blob over (block_cnt+1) cumulative offsets
//     Min0_Uint_Vec blob over block_cnt usz values
//     skip-block sentinel: get2(i)[0] == get2(i)[1].
//   align 64
//   per-color compressed data (non-singular), ceil64-aligned tail
//   end-checksum (8 bytes, xxhash with EGTB_CHECKSUM_INIT_VALUE)
//
// Per-block uncompressed payload (LZMA-compressed before write):
//   uint32 num_positions
//   uint32 num_single, num_double, num_multi       (state==01/10/11 counts;
//                                                   num_const = np - others)
//   uint32 single_stream_bytes, double_stream_bytes
//   uint8  state_bits[ceil(np*2/8)]                2 bpp: 00 CONST 01 SINGLE
//                                                          10 DOUBLE 11 MULTI
//   uint8  const_stream[num_const * eb]            ILLEGAL emits last_legal_rank
//   uint8  single_hints[ceil(num_single/8)]        bit set ⇒ trailing rank is
//                                                  DRAW (omitted from payload)
//   uint8  single_stream[...]   per SINGLE: uint8 h|(draw_end ? 0x80 : 0),
//                                           rank r0, [rank r1 if !draw_end]
//   uint8  double_hints[ceil(num_double/8)]        bit set ⇒ r2 is DRAW
//   uint8  double_stream[...]   per DOUBLE: uint8 h1, uint8 h2|(draw_end?0x80:0),
//                                           rank r0, r1, [r2 if !draw_end]
//   align 4
//   uint32 multi_dir[num_multi + 1]                cumulative byte offsets
//   uint8  multi_stream[...]    per MULTI: uint8 k|(draw_end ? 0x80 : 0),
//                                          uint8 cp_bitmap[16] (101 bits used),
//                                          rank r0..r_{k-1}  (k-1 if draw_end)

struct DTM50_Rank_Table
{
	static constexpr size_t LUT_SIZE = Value_Histogram::HIST_BINS;
	static constexpr uint16_t NO_RANK = Value_Rank_Table::NO_RANK;
	uint8_t entry_bytes = 1;
	std::vector<uint16_t> rank_to_value;
	std::array<uint16_t, LUT_SIZE> value_to_rank{};

	void reset()
	{
		entry_bytes = 1;
		rank_to_value.clear();
		value_to_rank.fill(NO_RANK);
	}

	NODISCARD uint16_t rank_of(uint16_t value) const
	{
		ASSERT(value < LUT_SIZE);
		ASSERT(value_to_rank[value] != NO_RANK);
		return value_to_rank[value];
	}
};

INLINE void write_rank_bytes(std::vector<uint8_t>& dst, uint16_t r, uint8_t eb)
{
	if (eb == 1) {
		dst.push_back(static_cast<uint8_t>(r));
	} else {
		dst.push_back(static_cast<uint8_t>(r & 0xFF));
		dst.push_back(static_cast<uint8_t>(r >> 8));
	}
}

// Per-position state across the 101 pack layers: 00=CONST, 01=SINGLE (1 transition),
// 10=DOUBLE (2), 11=MULTI (3+). ILLEGAL is folded into CONST as last_legal_rank
// to keep LZMA runs long.
//
// Draw-end hint: trailing-DRAW is the dominant pattern when W/L runs out of
// 50MR. Non-CONST states carry a hint bit; when set, the final rank is omitted
// and the decoder synthesizes DRAW (stored=0). Hint bit lives in MSB of the
// final h byte (SINGLE/DOUBLE) or of the change-count byte (MULTI; k fits 7 bits).
// DRAW is excluded from the rank table (NO_RANK sentinel) unless LOSS-in-0
// independently needs storage 0.
//
// Random access: state_bits popcount → j-th SINGLE/DOUBLE; single_hints/
// double_hints walked to compute the variable byte offset. MULTI uses
// multi_dir since k varies.
struct RS_Block_Encoder
{
	// NO_RANK doubles as the DRAW sentinel here. Monotonicity (DTM rises then
	// flips to DRAW once) means DRAW only appears as cr[k-1], or as cr[0] when
	// k == 1.
	static constexpr uint16_t DRAW_SENTINEL = DTM50_Rank_Table::NO_RANK;

	const DTM50_Rank_Table* ranks = nullptr;
	std::vector<uint8_t> state_bits;        // 2 bits/position
	std::vector<uint8_t> const_stream;      // 1 rank/position in state==CONST
	std::vector<uint8_t> single_hints;      // 1 bit per SINGLE position
	std::vector<uint8_t> single_stream;     // variable: 2 B or 1+2·eb B
	std::vector<uint8_t> double_hints;      // 1 bit per DOUBLE position
	std::vector<uint8_t> double_stream;     // variable: 2+2·eb B or 2+3·eb B
	std::vector<uint32_t> multi_dir;        // cumulative byte offsets
	std::vector<uint8_t> multi_stream;      // [k|hint, 16B bm, (k-hint) ranks]
	std::vector<uint8_t> out;

	NODISCARD Const_Span<uint8_t> encode(const uint16_t* values, size_t num_positions)
	{
		ASSERT(ranks != nullptr);
		const uint8_t eb = ranks->entry_bytes;
		const size_t sb_bytes = (num_positions * 2 + 7) / 8;
		state_bits.assign(sb_bytes, 0);
		const_stream.clear();
		single_hints.clear();
		single_stream.clear();
		double_hints.clear();
		double_stream.clear();
		multi_dir.clear();
		multi_dir.push_back(0);
		multi_stream.clear();

		std::array<uint16_t, DTM50_PACK_LAYERS> cp{};
		std::array<uint16_t, DTM50_PACK_LAYERS> cr{};

		uint16_t last_legal_rank = 0;  // any rank works; ILLEGAL never decodes
		uint32_t num_single = 0;
		uint32_t num_double = 0;

		for (size_t i = 0; i < num_positions; ++i)
		{
			// ILLEGAL is hmc-invariant: chess legality doesn't depend on the
			// clock, so a cell is ILLEGAL at all 101 pack layers or none.
			if (values[i] == DTM_Final_Entry::ILLEGAL_VAL)
			{
				// CONST (state 00). Emit last_legal_rank to extend LZMA runs.
				write_rank_bytes(const_stream, last_legal_rank, eb);
				continue;
			}

			size_t k = 0;
			uint16_t prev = ranks->value_to_rank[values[0 * num_positions + i]];
			cp[k] = 0; cr[k] = prev; ++k;
			for (size_t h = 1; h < DTM50_PACK_LAYERS; ++h)
			{
				const uint16_t r = ranks->value_to_rank[values[h * num_positions + i]];
				if (r != prev) { cp[k] = static_cast<uint16_t>(h); cr[k] = r; prev = r; ++k; }
			}

			if (k == 1)
			{
				// CONST. DRAW emits placeholder rank 0; probe's wdl=DRAW path
				// ignores the rank lookup.
				const uint16_t r = (cr[0] == DRAW_SENTINEL) ? 0u : cr[0];
				last_legal_rank = r;
				write_rank_bytes(const_stream, r, eb);
			}
			else if (k == 2)
			{
				set_state(i, 1);
				const uint16_t h1 = cp[1];
				ASSERT(h1 >= 1 && h1 <= DTM50_PACK_LAYERS - 1);
				ASSERT(cr[0] != DRAW_SENTINEL);  // monotonicity: DRAW is terminal
				const bool draw_end = (cr[1] == DRAW_SENTINEL);
				push_bit(single_hints, num_single, draw_end);
				single_stream.push_back(
					static_cast<uint8_t>(h1 | (draw_end ? 0x80u : 0u)));
				write_rank_bytes(single_stream, cr[0], eb);
				if (!draw_end) write_rank_bytes(single_stream, cr[1], eb);
				++num_single;
				last_legal_rank = draw_end ? cr[0] : cr[1];
			}
			else if (k == 3)
			{
				set_state(i, 2);
				const uint16_t h1 = cp[1], h2 = cp[2];
				ASSERT(h1 >= 1 && h2 >= 2 && h1 < h2 && h2 <= DTM50_PACK_LAYERS - 1);
				ASSERT(cr[0] != DRAW_SENTINEL && cr[1] != DRAW_SENTINEL);
				const bool draw_end = (cr[2] == DRAW_SENTINEL);
				push_bit(double_hints, num_double, draw_end);
				double_stream.push_back(static_cast<uint8_t>(h1));
				double_stream.push_back(
					static_cast<uint8_t>(h2 | (draw_end ? 0x80u : 0u)));
				write_rank_bytes(double_stream, cr[0], eb);
				write_rank_bytes(double_stream, cr[1], eb);
				if (!draw_end) write_rank_bytes(double_stream, cr[2], eb);
				++num_double;
				last_legal_rank = draw_end ? cr[1] : cr[2];
			}
			else
			{
				set_state(i, 3);
				const bool draw_end = (cr[k - 1] == DRAW_SENTINEL);
				for (size_t j = 0; j + 1 < k; ++j)
					ASSERT(cr[j] != DRAW_SENTINEL);
				const size_t base = multi_stream.size();
				multi_stream.push_back(
					static_cast<uint8_t>(k | (draw_end ? 0x80u : 0u)));
				uint8_t bm[16] = { 0 };
				for (size_t j = 0; j < k; ++j)
				{
					const uint16_t h = cp[j];
					bm[h / 8] |= static_cast<uint8_t>(1u << (h % 8));
				}
				multi_stream.insert(multi_stream.end(), bm, bm + 16);
				const size_t to_write = draw_end ? (k - 1) : k;
				for (size_t j = 0; j < to_write; ++j)
					write_rank_bytes(multi_stream, cr[j], eb);
				multi_dir.push_back(static_cast<uint32_t>(multi_stream.size() - base));
				last_legal_rank = draw_end ? cr[k - 2] : cr[k - 1];
			}
		}

		for (size_t i = 1; i < multi_dir.size(); ++i)
			multi_dir[i] += multi_dir[i - 1];

		const uint32_t np32 = static_cast<uint32_t>(num_positions);
		const uint32_t num_multi = static_cast<uint32_t>(multi_dir.size() - 1);
		const size_t sh_bytes = (num_single + 7) / 8;
		const size_t dh_bytes = (num_double + 7) / 8;
		ASSERT(single_hints.size() == sh_bytes);
		ASSERT(double_hints.size() == dh_bytes);

		const uint32_t ss_bytes32 = static_cast<uint32_t>(single_stream.size());
		const uint32_t ds_bytes32 = static_cast<uint32_t>(double_stream.size());

		size_t multi_dir_off =
			4 + 4 + 4 + 4 + 4 + 4                   // np, ns, nd, nm, ss_bytes, ds_bytes
			+ sb_bytes                              // state_bits (2 bpp)
			+ const_stream.size()                   // const_stream
			+ sh_bytes + single_stream.size()       // SINGLE: hint bitmap + variable payload
			+ dh_bytes + double_stream.size();      // DOUBLE: hint bitmap + variable payload
		multi_dir_off += (4 - (multi_dir_off & 3)) & 3;

		size_t total =
			multi_dir_off
			+ multi_dir.size() * 4                  // multi_dir (cumulative offsets)
			+ multi_stream.size();                  // multi_stream
		total += (4 - (total & 3)) & 3;

		out.resize(total);
		Serial_Memory_Writer w{ Span<uint8_t>(out) };
		w.write<uint32_t>(np32);
		w.write<uint32_t>(num_single);
		w.write<uint32_t>(num_double);
		w.write<uint32_t>(num_multi);
		w.write<uint32_t>(ss_bytes32);
		w.write<uint32_t>(ds_bytes32);
		w.write(Const_Span<uint8_t>(state_bits.data(), sb_bytes));
		if (!const_stream.empty())
			w.write(Const_Span<uint8_t>(const_stream.data(), const_stream.size()));
		if (sh_bytes != 0)
			w.write(Const_Span<uint8_t>(single_hints.data(), sh_bytes));
		if (!single_stream.empty())
			w.write(Const_Span<uint8_t>(single_stream.data(), single_stream.size()));
		if (dh_bytes != 0)
			w.write(Const_Span<uint8_t>(double_hints.data(), dh_bytes));
		if (!double_stream.empty())
			w.write(Const_Span<uint8_t>(double_stream.data(), double_stream.size()));
		ASSERT(w.num_bytes_written() <= multi_dir_off);
		w.zero_align(4);
		w.write(Const_Span<uint8_t>(
			reinterpret_cast<const uint8_t*>(multi_dir.data()),
			multi_dir.size() * 4));
		if (!multi_stream.empty())
			w.write(Const_Span<uint8_t>(multi_stream.data(), multi_stream.size()));
		w.zero_align(4);
		ASSERT(w.num_bytes_written() == total);
		return Const_Span<uint8_t>(out.data(), out.size());
	}

private:
	INLINE void set_state(size_t pos, uint8_t s)
	{
		const size_t bit_off = pos * 2;
		state_bits[bit_off / 8] |= static_cast<uint8_t>((s & 3u) << (bit_off % 8));
	}

	static INLINE void push_bit(std::vector<uint8_t>& bm, uint32_t bit_idx, bool v)
	{
		if ((bit_idx % 8) == 0) bm.push_back(0);
		if (v) bm.back() |= static_cast<uint8_t>(1u << (bit_idx % 8));
	}
};

struct DTM50_Compressed_Color
{
	bool is_singular = false;
	DTM50_Rank_Table ranks;
	uint32_t block_positions = 0;
	uint64_t block_cnt = 0;
	uint32_t tail_positions = 0;
	Compressed_Block_Store compressed_blocks;
	// Plain LZMA has no end-marker, so the decoder needs each block's usz.
	std::vector<uint64_t> usizes;
	size_t total_compressed_size = 0;
};

using DTM50_Save_Cache = Save_Group_Cache<DTM50_Final_Entry, DTM50_Intermediate_Entry>;
using DTM50_Pinned_Range = Pinned_Group_Range<DTM50_Final_Entry, DTM50_Intermediate_Entry>;

INLINE size_t dtm50_table_idx_of(Color c, size_t h)
{
	return static_cast<size_t>(c) * DTM50_HMC_COUNT + h;
}

// h=0 feeds EGTB_Info (the canonical reset-50MR view); other layers only
// contribute to the rank score. Returns false iff no W/L storage values appear.
NODISCARD bool gather_dtm50_info(
	const Piece_Config_For_Gen& epsi,
	DTM50_Table& table,
	DTM50_Save_Cache& cache,
	const DTM_File_For_Probe& dtm,
	Color color,
	size_t num_positions,
	size_t positions_per_group,
	In_Out_Param<Thread_Pool> thread_pool,
	size_t max_workers,
	EGTB_Info& info,
	DTM50_Rank_Table& ranks)
{
	const size_t ng = table.m_dtm[color][0].num_groups();

	std::array<std::array<uint8_t, DTM50_Rank_Table::LUT_SIZE>, DTM50_PACK_LAYERS> per_layer_seen{};

	struct Seen_Pair
	{
		std::array<uint8_t, DTM50_Rank_Table::LUT_SIZE> base{};
		std::array<uint8_t, DTM50_Rank_Table::LUT_SIZE> layer0{};
	};

	const auto bins = gather_egtb_info_parallel<Seen_Pair>(
		thread_pool, epsi, cache, dtm50_table_idx_of(color, 0), color,
		num_positions, max_workers, info,
		[&](Seen_Pair& s, size_t idx, const DTM50_Final_Entry& e) {
			// DRAW/ILLEGAL are routed via the WDL companion; the rank table only
			// needs W/L storage values (LOSS-in-0 still adds storage 0 normally).
			if (!e.is_draw())
				s.layer0[dtm50_value_for_storage(e)] = 1;
			if (!e.is_illegal())
			{
				const DTM_Final_Entry d0 =
					dtm.read(color, static_cast<Board_Index>(idx));
				if (!d0.is_draw())
					s.base[dtm_value_for_storage(d0)] = 1;
			}
		});

	for (const Seen_Pair& s : bins)
		for (size_t v = 0; v < DTM50_Rank_Table::LUT_SIZE; ++v)
		{
			if (s.base[v])   per_layer_seen[0][v] = 1;
			if (s.layer0[v]) per_layer_seen[1][v] = 1;
		}

	const size_t n = DTM50_HMC_COUNT - 1;
	const size_t capped_n = (max_workers == 0) ? n : std::min(n, max_workers);
	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), capped_n));
	std::atomic<size_t> next(1);
	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t /*worker_id*/) {
		for (;;)
		{
			const size_t h = next.fetch_add(1, std::memory_order_relaxed);
			if (h >= DTM50_HMC_COUNT) return;
			auto& seen = per_layer_seen[h + 1];
			const size_t ti = dtm50_table_idx_of(color, h);
			for (size_t g = 0; g < ng; ++g)
			{
				DTM50_Pinned_Range pin(cache, ti, g, g);
				const size_t p_lo = g * positions_per_group;
				const size_t p_hi = std::min(p_lo + positions_per_group, num_positions);
				for (size_t p = p_lo; p < p_hi; ++p)
				{
					const auto e = table.read<DTM50_Final_Entry>(color, static_cast<uint16_t>(h), static_cast<Board_Index>(p));
					if (!e.is_draw())
						seen[dtm50_value_for_storage(e)] = 1;
				}
			}
		}
	});

	// Values present in more layers get smaller ranks (LZMA bias).
	std::array<uint64_t, DTM50_Rank_Table::LUT_SIZE> score{};
	for (size_t lp = 0; lp < DTM50_PACK_LAYERS; ++lp)
		for (size_t v = 0; v < DTM50_Rank_Table::LUT_SIZE; ++v)
			if (per_layer_seen[lp][v]) ++score[v];

	// No W/L values anywhere → caller marks color singular-DRAW.
	std::vector<uint16_t> values;
	values.reserve(64);
	for (size_t v = 0; v < DTM50_Rank_Table::LUT_SIZE; ++v)
		if (score[v] != 0) values.push_back(static_cast<uint16_t>(v));
	if (values.empty()) return false;

	// Frequency sort; DRAW is intentionally absent (NO_RANK sentinel).
	std::sort(values.begin(), values.end(),
		[&](uint16_t a, uint16_t b) {
			if (score[a] != score[b]) return score[a] > score[b];
			return a < b;
		});

	ranks.reset();
	ranks.rank_to_value = std::move(values);
	ranks.entry_bytes = (ranks.rank_to_value.size() <= 256) ? 1 : 2;
	for (size_t i = 0; i < ranks.rank_to_value.size(); ++i)
		ranks.value_to_rank[ranks.rank_to_value[i]] = static_cast<uint16_t>(i);
	return true;
}

// Per-block pin loop holds one layer-range at a time; peak pin-count is
// effective_workers × group_range (matches DTM/DTC under tight budgets).
void save_compress_dtm50(
	In_Out_Param<Thread_Pool> thread_pool,
	DTM50_Table& table,
	DTM50_Save_Cache& cache,
	const DTM_File_For_Probe& dtm,
	Color color,
	size_t num_positions,
	uint32_t block_size,
	uint32_t index_perm,
	size_t max_workers,
	std::filesystem::path spill_path,
	DTM50_Compressed_Color& out)
{
	const size_t bp = block_size / out.ranks.entry_bytes;
	const size_t bcnt = ceil_div(num_positions, bp);
	const size_t tail = num_positions - (bcnt - 1) * bp;

	const size_t pool_workers = thread_pool->num_workers();
	const size_t effective_workers = (max_workers == 0)
		? pool_workers : std::min(max_workers, pool_workers);

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8;
	const size_t print_period = ceil_div(PRINT_PERIOD_BYTES * effective_workers, static_cast<size_t>(block_size));
	Concurrent_Progress_Bar progress_bar(bcnt, print_period,
		std::string("save_compress_dtm50 ") + std::to_string(static_cast<int>(color)));

	out.block_positions = narrowing_static_cast<uint32_t>(bp);
	out.block_cnt = bcnt;
	out.tail_positions = (tail == bp) ? 0u : narrowing_static_cast<uint32_t>(tail);
	out.compressed_blocks = Compressed_Block_Store(std::move(spill_path), bcnt, static_cast<size_t>(block_size));
	out.usizes.resize(bcnt);

	auto& src = table.m_dtm[color][0];
	auto group_id_of_pos = [&src](size_t p) {
		return src.group_id_of(src.slice_id_of(p));
	};

	const auto perm_plan = make_index_permutation_plan(table.m_epsi, index_perm);

	std::atomic<size_t> next_block_id(0);

	thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
		if (thread_id >= effective_workers) return;

		std::vector<uint16_t> chunk(bp * DTM50_PACK_LAYERS);
		std::vector<size_t> logical_pos(bp);
		RS_Block_Encoder encoder;
		encoder.ranks = &out.ranks;
		LZMA_Compress_Helper lzma;

		for (;;)
		{
			const size_t b = next_block_id.fetch_add(1, std::memory_order_relaxed);
			if (b >= bcnt) return;

			const size_t p_base = b * bp;
			const size_t this_bp =
				(b == bcnt - 1 && out.tail_positions != 0) ? out.tail_positions : bp;

			const size_t want_lo = group_id_of_pos(p_base);
			const size_t want_hi = group_id_of_pos(p_base + this_bp - 1);
			for (size_t k = 0; k < this_bp; ++k)
				logical_pos[k] = storage_index_to_logical_index(perm_plan, p_base + k);

			// Block-skip if no W/L cells: probe collapses DRAW/ILLEGAL to DRAW.
			// MUST test entry class, not storage 0 — storage 0 also covers
			// WIN-in-1 / LOSS-in-0, so `v == 0` would silently drop the only
			// live cells in drawn-heavy sub-tables (KBKB, KNKN, KNNK, ...).
			bool uniform_skip = true;
			uint16_t* const base = chunk.data();  // pack layer 0 (unbounded DTM)
			for (size_t lp = 1; lp < DTM50_PACK_LAYERS; ++lp)
			{
				const size_t h = lp - 1;
				DTM50_Pinned_Range pin(cache, dtm50_table_idx_of(color, h), want_lo, want_hi);
				uint16_t* row = chunk.data() + lp * this_bp;
				for (size_t k = 0; k < this_bp; ++k)
				{
					const auto e = table.read<DTM50_Final_Entry>(color, static_cast<uint16_t>(h), static_cast<Board_Index>(logical_pos[k]));
					row[k] = dtm50_value_for_storage(e);
					if (h != 0)
						continue;
					// hmc=0 pass: also fill the unbounded-DTM base layer. This cell's
					// (hmc-invariant) legality gates the DTM read; don't-cares stay ILLEGAL.
					if (e.is_illegal())
					{
						base[k] = DTM_Final_Entry::ILLEGAL_VAL;
						continue;
					}
					const DTM_Final_Entry d0 =
						dtm.read(color, static_cast<Board_Index>(logical_pos[k]));
					if (d0.is_draw())
					{
						base[k] = DTM_Final_Entry::ILLEGAL_VAL;
						continue;
					}
					base[k] = dtm_value_for_storage(d0);
					uniform_skip = false;
				}
			}

			if (uniform_skip)
			{
				// usz=0 in the offset table is the skip sentinel.
				out.usizes[b] = 0;
				out.compressed_blocks.clear(b);
				progress_bar += 1;
				continue;
			}

			const Const_Span<uint8_t> payload = encoder.encode(chunk.data(), this_bp);
			std::vector<uint8_t> compressed = lzma.compress(payload);
			if (compressed.size() > 0xFFFFFFFFu)
				throw std::runtime_error("rs block too large for offset encoding");

			out.usizes[b] = payload.size();
			out.compressed_blocks.set(b, std::move(compressed));
			progress_bar += 1;
		}
	});

	out.total_compressed_size = out.compressed_blocks.total_size();

	progress_bar.set_finished();
}

void save_dtm50_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const DTM50_Compressed_Color color_out[COLOR_NB],
	const std::filesystem::path& file_path,
	Fixed_Vector<Color, 2> colors)
{
	// Pre-encode offsets + usz so file_size knows the section bytes.
	Mono_Uint_Vec::Encoded mono_enc[COLOR_NB];
	Min0_Uint_Vec::Encoded min0_enc[COLOR_NB];
	for (Color c : colors)
	{
		const DTM50_Compressed_Color& co = color_out[c];
		if (co.is_singular) continue;
		const size_t n = co.block_cnt;
		std::vector<uint64_t> off(n + 1);
		size_t cur = 0;
		for (size_t k = 0; k < n; ++k)
		{
			off[k] = cur;
			cur += co.compressed_blocks.block_size(k);  // skip-block size == 0
		}
		off[n] = cur;
		ASSERT(cur == co.total_compressed_size);
		mono_enc[c] = Mono_Uint_Vec::encode(Const_Span<uint64_t>(off.data(), off.size()));

		std::vector<uint64_t> usz(n);
		for (size_t k = 0; k < n; ++k) usz[k] = co.usizes[k];
		min0_enc[c] = Min0_Uint_Vec::encode(Const_Span<uint64_t>(usz.data(), usz.size()));
	}

	size_t file_size = 8;  // magic/key

	for (Color c : colors)
	{
		file_size += 1;
		if (color_out[c].is_singular) file_size += 1;
		else {
			// index_perm(4)+entry_bytes(1)+block_positions(4)+block_cnt(8)+tail_positions(4)+data_size(8)+num_ranks(2)
			file_size += 4 + 1 + 4 + 8 + 4 + 8 + 2;
			file_size += color_out[c].ranks.rank_to_value.size() * 2;
		}
	}
	for (Color c : colors)
		if (!color_out[c].is_singular)
			file_size += MONO_SECTION_WIDTH_BYTES
			           + mono_enc[c].on_disk_bytes
			           + min0_enc[c].on_disk_bytes;
	file_size = ceil_to_multiple(file_size, size_t{ 64 });
	for (Color c : colors)
	{
		if (color_out[c].is_singular) continue;
		file_size += color_out[c].total_compressed_size;
		file_size = ceil_to_multiple(file_size, size_t{ 64 });
	}

	const auto tmp = file_path.string() + ".tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), file_size + 8))
		throw std::runtime_error("Failed to create " + tmp);
	Serial_Memory_Writer w(out.data_span());

	w.write<uint32_t>(narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC));
	w.write<uint32_t>(narrowing_static_cast<uint32_t>(
		(ps.min_material_key().value() << 2ull) | colors.size()));

	for (Color c : colors)
	{
		const DTM50_Compressed_Color& co = color_out[c];
		if (co.is_singular)
		{
			w.write<uint8_t>(EGTB_SINGULAR_FLAG);
			w.write<uint8_t>(0);
		}
		else
		{
			w.write<uint8_t>(0);
			w.write<uint32_t>(index_perm[c]);
			w.write<uint8_t>(co.ranks.entry_bytes);
			w.write<uint32_t>(co.block_positions);
			w.write<uint64_t>(co.block_cnt);
			w.write<uint32_t>(co.tail_positions);
			w.write<uint64_t>(static_cast<uint64_t>(co.total_compressed_size));
			w.write<uint16_t>(static_cast<uint16_t>(co.ranks.rank_to_value.size()));
			for (uint16_t v : co.ranks.rank_to_value) w.write<uint16_t>(v);
		}
	}
	for (Color c : colors)
	{
		const DTM50_Compressed_Color& co = color_out[c];
		if (co.is_singular) continue;
		const auto& m = mono_enc[c];
		const auto& u = min0_enc[c];
		w.write<uint8_t>(m.log2_bu);
		w.write<uint8_t>(m.sample_width);
		w.write<uint8_t>(m.offset_width);
		w.write<uint8_t>(u.width);
		w.write(Const_Span<uint8_t>(m.blob.data(), m.on_disk_bytes));
		w.write(Const_Span<uint8_t>(u.blob.data(), u.on_disk_bytes));
	}
	w.zero_align(64);
	for (Color c : colors)
	{
		const DTM50_Compressed_Color& co = color_out[c];
		if (co.is_singular) continue;
		for (size_t k = 0; k < co.compressed_blocks.size(); ++k)
			if (co.compressed_blocks.block_size(k) != 0)
				w.write(co.compressed_blocks.block(k));
		w.zero_align(64);
	}
	if (w.num_bytes_written() != file_size)
		throw std::runtime_error("dtm50 file size mismatch");
	w.write_end_checksum(EGTB_CHECKSUM_INIT_VALUE);
	out.close();

	std::error_code ec;
	std::filesystem::rename(tmp, file_path, ec);
	if (ec) throw std::runtime_error("rename failed: " + ec.message());
}

}  // namespace

void DTM50_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_save_start = std::chrono::steady_clock::now();
	const auto colors = table_colors();
	const size_t num_positions = m_epsi.num_positions();
	if (num_positions == 0) return;

	m_epsi.prepare_orbit_weight_table();
	m_info.clear();

	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t wss = m_table->m_dtm[WHITE][0].within_slice_size();
	const size_t positions_per_group = spg * wss;

	// One cache across all 2 × HMC_COUNT layer tables shares the paging budget.
	const size_t group_bytes = positions_per_group * sizeof(DTM50_Final_Entry);
	size_t cap_groups;
	size_t max_workers;
	if (m_paging_budget_bytes == 0 || group_bytes == 0)
	{
		cap_groups = std::numeric_limits<size_t>::max();
		max_workers = 0;
	}
	else
	{
		cap_groups = std::max<size_t>(1, m_paging_budget_bytes / group_bytes);
		max_workers = cap_groups;
	}

	std::vector<Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*> all_tables;
	all_tables.reserve(COLOR_NB * DTM50_HMC_COUNT);
	for (Color c : { WHITE, BLACK })
		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			all_tables.push_back(&m_table->m_dtm[c][h]);
	DTM50_Save_Cache cache(std::move(all_tables), cap_groups);

	DTM50_Compressed_Color color_out[COLOR_NB]{};
	uint32_t index_perm[COLOR_NB] = { 0, 0 };

	for (Color me : colors)
	{
		DTM_File_For_Probe dtm_probe(paths, m_epsi, thread_pool);

		const bool any = gather_dtm50_info(
			m_epsi, *m_table, cache, dtm_probe, me, num_positions, positions_per_group,
			thread_pool, max_workers, m_info, color_out[me].ranks);

		if (!any)
		{
			std::printf("save dtm50 %d: singular\n", static_cast<int>(me));
			color_out[me].is_singular = true;
		}
		else
		{
			index_perm[me] = dtm_probe.m_per_color[me].plan.perm;
			save_compress_dtm50(
				thread_pool, *m_table, cache, dtm_probe, me, num_positions,
				DTM50_BLOCK_SIZE,
				index_perm[me], max_workers,
				paths.block_spill_path(m_epsi, me),
				color_out[me]);
		}

		std::atomic<size_t> next(0);
		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			for (;;)
			{
				const size_t h = next.fetch_add(1, std::memory_order_relaxed);
				if (h >= DTM50_HMC_COUNT) return;
				cache.purge(dtm50_table_idx_of(me, h));
				m_table->m_dtm[me][h].remove_disk_files();
				m_table->m_dtm[me][h].close();
			}
		});
	}

	if (m_is_symmetric)
	{
		std::atomic<size_t> next(0);
		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			for (;;)
			{
				const size_t h = next.fetch_add(1, std::memory_order_relaxed);
				if (h >= DTM50_HMC_COUNT) return;
				cache.purge(dtm50_table_idx_of(BLACK, h));
				m_table->m_dtm[BLACK][h].remove_disk_files();
				m_table->m_dtm[BLACK][h].close();
			}
		});
	}

	save_dtm50_table(m_epsi, index_perm, color_out, paths.dtm50_save_path(m_epsi), colors);

	for (Color me : colors)
	{
		if (m_info.longest_win[me] == 0) continue;
		Position_For_Gen pos_gen(m_epsi,
			static_cast<Board_Index>(m_info.longest_idx[me]), me);
		pos_gen.board_unchecked().to_fen(Span(m_info.longest_fen[me]));
	}
	std::ofstream fp(paths.dtm50_info_save_path(m_epsi),
		std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
