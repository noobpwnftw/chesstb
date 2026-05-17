#include "egtb/egtb_gen_dtm.h"
#include "egtb/egtb_compress.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/math.h"
#include "util/progress_bar.h"
#include "util/utility.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

DTM_Generator::DTM_Generator(
	const Piece_Config& ps,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTM_Table>(ps, tmp_dir))
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	const size_t bytes_per_color =
		m_table->m_dtm[WHITE].num_slices()
		* m_table->m_dtm[WHITE].within_slice_size()
		* sizeof(DTM_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_iter_state(
		m_table->m_dtm[WHITE].num_groups(),
		m_table->m_dtm[WHITE].num_entries());
}

DTM_Final_Entry DTM_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move) const
{
	Color sub_color;
	const Piece_Config_For_Gen* sub_epsi = nullptr;
	const Board_Index sub_idx = next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

	if (sub_epsi == nullptr) return DTM_Final_Entry::make_draw();
	auto it = m_sub_dtm_by_material.find(sub_epsi->min_material_key());
	if (it == m_sub_dtm_by_material.end()) return DTM_Final_Entry::make_draw();
	return it->second->read(sub_color, sub_idx);
}

DTM_Final_Entry DTM_Generator::read_post_move_dtm(Position_For_Gen& pos_gen, Move move) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	return read_dtm<DTM_Final_Entry>(color_opp(mover), post_idx);
}

namespace {

// True iff `a` is strictly better for the mover: highest class, then
// smaller dtm on WIN / larger dtm on LOSS.
INLINE bool dtm_better_for_mover(DTM_Final_Entry a, DTM_Final_Entry b)
{
	auto rank = [](DTM_Final_Entry e) -> int {
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

}  // namespace

DTM_Final_Entry DTM_Generator::effective_opp_dtm_after_dp(Position_For_Gen& pos_gen, Move dp_move) const
{
	const DTM_Final_Entry no_ep = read_post_move_dtm(pos_gen, dp_move);

	// do_move/undo_move on pos_gen's own board: pos_gen's cached board matches
	// its index again after the undo, so callers see no change.
	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const Piece captured_by_dp = p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	DTM_Final_Entry best_ep_for_opp = DTM_Final_Entry::make_loss(0);  // worst for opp; first EP overrides
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
		const DTM_Final_Entry after_ep = read_sub_tb(*p_gen_for_ep, ep_move);
		DTM_Final_Entry opp_at_pre_ep;
		if (after_ep.is_illegal())          continue;
		else if (after_ep.is_win())         opp_at_pre_ep = DTM_Final_Entry::make_loss(after_ep.value() + 1);
		else if (after_ep.is_loss())        opp_at_pre_ep = DTM_Final_Entry::make_win(after_ep.value() + 1);
		else                                opp_at_pre_ep = DTM_Final_Entry::make_draw();

		if (!any_ep || dtm_better_for_mover(opp_at_pre_ep, best_ep_for_opp))
			best_ep_for_opp = opp_at_pre_ep;
		any_ep = true;
	}

	p.undo_move(dp_move, captured_by_dp);

	if (!any_ep) return no_ep;
	return dtm_better_for_mover(best_ep_for_opp, no_ep) ? best_ep_for_opp : no_ep;
}

DTM_Any_Entry DTM_Generator::make_initial_entry(Position_For_Gen& pos_gen,
                                                Out_Param<uint16_t> worst_loss_dtm) const
{
	*worst_loss_dtm = 0;
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM_Final_Entry::make_illegal();

	Position& pos = pos_gen.board_unchecked();

	// Seeds derived from sub-DTM values on cap/promo moves.
	uint16_t best_win_dtm  = std::numeric_limits<uint16_t>::max();
	bool saw_win  = false;
	bool saw_draw = false;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	bool any_legal = false;
	bool any_in_material = false;
	bool any_pawn_eval = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;
		const bool is_cap = m.is_ep_capture() || !pos.is_empty(m.to());
		if (!is_cap && !m.is_promotion())
		{
			any_in_material = true;
			// Pawn push crosses slices; child lives in a non-iterating built slice → retro can't bridge.
			if (piece_type(pos.piece_at(m.from())) == PAWN)
			{
				any_pawn_eval = true;
				const DTM_Final_Entry opp_e = is_pawn_double_push(m)
					? effective_opp_dtm_after_dp(pos_gen, m)
					: read_post_move_dtm(pos_gen, m);
				if (opp_e.is_loss())
				{
					saw_win = true;
					update_min(best_win_dtm, static_cast<uint16_t>(opp_e.value() + 1));
				}
				else if (opp_e.is_win())
					update_max(*worst_loss_dtm, static_cast<uint16_t>(opp_e.value() + 1));
				else if (opp_e.is_draw())
					saw_draw = true;
			}
			continue;
		}
		any_pawn_eval = true;
		const DTM_Final_Entry sub_e = read_sub_tb(pos_gen, m);
		if (sub_e.is_illegal()) continue;
		if (sub_e.is_loss())
		{
			saw_win = true;
			const uint16_t cand = static_cast<uint16_t>(sub_e.value() + 1);
			update_min(best_win_dtm, cand);
		}
		else if (sub_e.is_win())
		{
			const uint16_t cand = static_cast<uint16_t>(sub_e.value() + 1);
			update_max(*worst_loss_dtm, cand);
		}
		else
		{
			// DRAW blocks both pure-WIN and forced-LOSS classification.
			saw_draw = true;
		}
	}
	if (!any_legal)
	{
		// Stalemate: plain Intermediate. No pre-quiet pred lands on a no-move
		// position, so retro never overwrites it.
		if (ctx.in_check) return DTM_Final_Entry::make_loss(0);
		return DTM_Intermediate_Entry{};
	}

	if (saw_win) return DTM_Final_Entry::make_win(best_win_dtm);

	if (!any_in_material && !saw_draw)
	{
		// All cap/promo → opp-WIN, no in-material escape, no draw → forced LOSS at max child dtm + 1.
		if (*worst_loss_dtm > 0)
			return DTM_Final_Entry::make_loss(*worst_loss_dtm);
	}
	// Intermediate; *worst_loss_dtm seeds m_max_dtm so iterate runs long enough
	// for check_loss at the cap/promo classification ply.
	return (any_pawn_eval && !saw_draw)
		? DTM_Intermediate_Entry::make_pawn_eval(*worst_loss_dtm)
		: DTM_Intermediate_Entry{};
}

uint16_t DTM_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtm[WHITE].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;
	const auto& targets_by_pid = m_targets_by_pid;

	// Working set: both colors at g + (push_target_pid, same_kid) for read_post_move_dtm.
	// Init writes both colors, so needs[B] == needs[W].
	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
		if (2 * ngroups * spg * m_epsi.within_slice_size() * sizeof(DTM_Final_Entry) <= m_paging_budget_bytes)
		{
			std::fill_n(m_scratch_need[WHITE].begin(), ngroups, 1);
		}
		else
		{
			m_scratch_need[WHITE][g] = 1;
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
					for (int32_t tpid : targets_by_pid[static_cast<size_t>(pid)])
					{
						const size_t target_slice =
							static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
						m_scratch_need[WHITE][target_slice / spg] = 1;
					}
				}
			}
		}
		m_scratch_need[BLACK] = m_scratch_need[WHITE];
		apply_working_set(thread_pool, &m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
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

	uint16_t max_init = 0;
	for (size_t g : m_pair_group_ids)
	{
		page_in_for_init_group(g);

		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> uint16_t {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			uint16_t local_max = 0;
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
					write_dtm(WHITE, idx, DTM_Final_Entry::make_illegal());
					write_dtm(BLACK, idx, DTM_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board_unchecked());
					if (canon != idx)
					{
						write_dtm(WHITE, idx, DTM_Final_Entry::make_illegal());
						write_dtm(BLACK, idx, DTM_Final_Entry::make_illegal());
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
					uint16_t worst_loss_dtm = 0;
					std::visit(overload{
						[&](DTM_Final_Entry entry) {
							write_dtm(us, idx, entry);
							if (!entry.is_illegal())
							{
								const uint16_t v = static_cast<uint16_t>(entry.value());
								update_max(local_max, v);
								mark_iter(us, idx, m_table->m_dtm[us]);
							}
						},
						[&](DTM_Intermediate_Entry entry) {
							write_dtm(us, idx, entry);
							update_max(local_max, worst_loss_dtm);
						},
					}, make_initial_entry(pos_gen, out_param(worst_loss_dtm)));
				}
			}
			return local_max;
		});
		for (uint16_t v : rets) update_max(max_init, v);
	}

	progress_bar.set_finished();

	return max_init;
}

DTM_Generator::Loss_Verification_Result DTM_Generator::check_loss(
	Position_For_Gen& pos_gen,
	uint16_t ply) const
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	Loss_Verification_Result r;
	bool any_legal = false;
	uint16_t max_contribution = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		if (!pos.is_empty(m.to()) || piece_type(pos.piece_at(m.from())) == PAWN)
		{
			const auto e = read_dtm<DTM_Intermediate_Entry>(pos.turn(), pos_gen.board_index());
			if (!e.has_pawn_eval()) return r;
			if (e.value() > ply) return r;
			update_max(max_contribution, static_cast<uint16_t>(e.value()));
			continue;
		}

		const Board_Index child = next_quiet_index(pos_gen, m);
		const auto ce = read_dtm<DTM_Final_Entry>(opp, child);
		if (!ce.is_win()) return r;
		if (ce.value() >= ply) return r;
		update_max(max_contribution, static_cast<uint16_t>(ce.value() + 1));
	}

	if (!any_legal) return r;
	if (max_contribution != ply) return r;

	r.is_loss = true;
	r.loss_dtm = max_contribution;
	return r;
}

bool DTM_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	bool wrote = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtm<DTM_Final_Entry>(opp, pred);
		if (e.is_illegal()) continue;
		if (e.is_loss()) continue;
		if (e.is_win() && e.value() <= 1) continue;
		write_dtm(opp, pred, DTM_Final_Entry::make_win(1));
		wrote = true;
	}
	return wrote;
}

void DTM_Generator::retro_mark_changed(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const DTM_Final_Entry fe = read_dtm<DTM_Final_Entry>(opp, pred);
		if (fe.is_illegal() || !fe.is_draw()) continue;
		const auto& ie = reinterpret_cast<const DTM_Intermediate_Entry&>(fe);
		if (ie.has_change()) continue;
		// Atomic OR: a racing retro_mark_wins Final write keeps its bits; the
		// CHANGE bit lands on top harmlessly (Final dispatch ignores it).
		m_table->m_dtm[opp].lock_add_flags(pred, DTM_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtm[opp]);
	}
}

bool DTM_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    uint16_t target_dtm)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	const DTM_Final_Entry new_e = DTM_Final_Entry::make_win(target_dtm);
	bool wrote = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtm<DTM_Final_Entry>(opp, pred);
		if (e.is_illegal() || e.is_loss()) continue;
		if (e.is_win() && e.value() <= target_dtm) continue;
		write_dtm(opp, pred, new_e);
		mark_iter(opp, pred, m_table->m_dtm[opp]);
		wrote = true;
	}
	return wrote;
}

DTM_Generator::Iter_Result DTM_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                                                   Color stm, uint16_t ply)
{
	const size_t spg = m_table->m_dtm[stm].slices_per_group();
	const auto& pid_in_pair = m_pid_in_pair;

	Iter_Result global;

	for (size_t g : m_pair_group_ids)
	{
		if (m_iter_groups[stm][g] == 0) continue;

		page_in_for_group(thread_pool, stm, g);

		Shared_Board_Index_Iterator cell_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> Iter_Result {
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
			Board_Index prev = BOARD_INDEX_NONE;
			Iter_Result local;

			for (auto [chunk_start, chunk_end] : cell_it.chunks())
			{
				const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
				if (!m_iter_chunks[stm][cid]) continue;

				Iter_Result chunk;

				for (Board_Index idx = chunk_start; idx != chunk_end;
				     idx = static_cast<Board_Index>(static_cast<size_t>(idx) + 1))
				{
					const size_t pid_of_idx = m_epsi.pawn_slice_of(idx);
					if (!pid_in_pair[pid_of_idx]) continue;

					// Intermediate handles draws with flags (CHANGE / PAWN_EVAL
					// routing); Final handles classified WIN/LOSS retros. Bare DRAW
					// Intermediates stay inert.
					const DTM_Final_Entry fe = read_dtm<DTM_Final_Entry>(stm, idx);

					Iter_Action action;
					if (!fe.is_illegal() && fe.is_draw())
					{
						const auto& ie = reinterpret_cast<const DTM_Intermediate_Entry&>(fe);
						// Only flagged Intermediates revisit; bare DRAW skips forever.
						if (ie.has_any_flags()) chunk.any_intermediate = true;
						if (ie.has_change())
							action = Iter_Action::REVERIFY;
						// PAWN_EVAL bootstraps the first check_loss for a flagged cell.
						else if (ie.has_pawn_eval() && ply == static_cast<uint16_t>(ie.value()))
							action = Iter_Action::REVERIFY;
						else
							continue;
					}
					else if (fe.is_illegal())
					{
						continue;
					}
					else
					{
						update_max(chunk.max_classified, static_cast<uint16_t>(fe.value()));
						if (ply == 0)
						{
							if (!(fe.is_loss() && fe.value() == 0)) continue;
							action = Iter_Action::MARK_WIN_IN_1;
						}
						// WIN(ply) and WIN(ply-1) both MARK_CHANGED so pred check_loss sees a
						// fully-classified child (same-ply just-overwritten + prior-ply settled).
						else if (fe.is_win() && (fe.value() == ply || fe.value() == ply - 1))
							action = Iter_Action::MARK_CHANGED;
						else if (fe.is_loss() && fe.value() == ply)
							action = Iter_Action::MARK_WIN_PREDS;
						else
							continue;
					}

					if (prev != BOARD_INDEX_NONE
					    && static_cast<size_t>(idx) == static_cast<size_t>(prev) + 1)
						++pos_gen;
					else
						pos_gen.set_board_index(idx);
					prev = idx;

					switch (action)
					{
					case Iter_Action::MARK_WIN_IN_1:
						if (retro_mark_win_in_1(pos_gen)) { local.wrote = true; update_max<uint16_t>(local.max_v, 1); }
						break;
					case Iter_Action::MARK_CHANGED:
						retro_mark_changed(pos_gen);
						local.wrote = true;
						break;
					case Iter_Action::MARK_WIN_PREDS:
					{
						const uint16_t target = static_cast<uint16_t>(ply + 1);
						if (retro_mark_wins(pos_gen, target)) { local.wrote = true; update_max(local.max_v, target); }
						break;
					}
					case Iter_Action::REVERIFY:
					{
						const auto res = check_loss(pos_gen, ply);
						if (!res.is_loss)
						{
							// Only a CHANGE clear counts as wrote — keeps iterate
							// alive one more ply. PAWN_EVAL-no-change must not
							// bump wrote, else iterate overshoots.
							auto ie = reinterpret_cast<const DTM_Intermediate_Entry&>(fe);
							if (ie.has_change())
							{
								ie.clear_flag(DTM_FLAG_CHANGE);
								write_dtm(stm, idx, ie);
								local.wrote = true;
							}
							break;
						}
						write_dtm(stm, idx, DTM_Final_Entry::make_loss(res.loss_dtm));
						update_max<uint16_t>(local.max_v, res.loss_dtm + 1);
						(void)retro_mark_wins(pos_gen, res.loss_dtm + 1);
						local.wrote = true;
						break;
					}
					default:
						break;
					}
				}

				if (chunk.any_intermediate) local.any_intermediate = true;
				update_max(local.max_classified, chunk.max_classified);

				// Evict full-CHUNK_SIZE chunks only; head/tail share a bit.
				if (static_cast<size_t>(chunk_end) - static_cast<size_t>(chunk_start) == CHUNK_SIZE
				    && !chunk.any_intermediate && chunk.max_classified + 1 < ply)
					m_iter_chunks[stm][cid] = 0;
			}
			return local;
		});
		bool any_intermediate = false;
		uint16_t max_classified = 0;
		for (const Iter_Result& r : rets) {
			if (r.wrote) global.wrote = true;
			update_max(global.max_v, r.max_v);
			if (r.any_intermediate) any_intermediate = true;
			update_max(max_classified, r.max_classified);
		}
		// Evict: no Intermediates and every classified value < ply-1 → no
		// dispatch will fire here again. Later writes reinstate via mark_iter.
		if (!any_intermediate && max_classified + 1 < ply)
			m_iter_groups[stm][g] = 0;
	}
	return global;
}

void DTM_Generator::iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply)
{
	auto check_interrupt = [&](uint16_t fp) {
		if (egtb_is_interrupt_requested())
			throw DTM_Interrupted{ 0, 0, fp, m_max_dtm };
	};

	if (finished_ply == 0)
	{
		(void)run_iter(thread_pool, WHITE, 0);
		(void)run_iter(thread_pool, BLACK, 0);
	}

	while (finished_ply < DTM_SCORE_MAX)
	{
		++finished_ply;
		std::printf("  iterate %4u\r", finished_ply); std::fflush(stdout);
		const Iter_Result rw = run_iter(thread_pool, WHITE, finished_ply);
		const Iter_Result rb = run_iter(thread_pool, BLACK, finished_ply);
		update_max(m_max_dtm, rw.max_v);
		update_max(m_max_dtm, rb.max_v);
		// Silent ply is safe to stop on only past every classified dtm value:
		// init-seeded WIN(d) from cap/promo sits dormant until ply hits d-1.
		if (!rw.wrote && !rb.wrote && finished_ply > m_max_dtm) break;
		check_interrupt(finished_ply);
	}
}

void DTM_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_paging_budget_bytes == 0) return;
	const Color opp = color_opp(me);
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtm[me].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const auto& ksm = m_epsi.king_slice_manager();
	const auto& psm = m_epsi.pawn_slice_manager();
	const bool has_pawns = psm.has_pawns();

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, ntotal);

	const size_t ngroups = m_table->m_dtm[WHITE].num_groups();
	auto& need_me  = m_scratch_need[me];
	auto& need_opp = m_scratch_need[opp];
	need_me.assign(ngroups, 0);
	need_opp.assign(ngroups, 0);
	need_me[group_id] = 1;

	if ((ngroups + 1) * spg * m_epsi.within_slice_size() * sizeof(DTM_Final_Entry) <= m_paging_budget_bytes)
	{
		std::fill_n(need_opp.begin(), ngroups, 1);
	}
	else
	{
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
					need_opp[same / spg] = 1;
					for (int32_t k : m_scratch_nbrs)
					{
						const size_t neigh = static_cast<size_t>(pp) * nks + static_cast<size_t>(k);
						need_opp[neigh / spg] = 1;
					}
				}
				if (has_pawns)
				{
					for (int32_t tpid : m_targets_by_pid[static_cast<size_t>(pid)])
					{
						const size_t target =
							static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
						need_opp[target / spg] = 1;
					}
				}
			}
		}
	}

	apply_working_set(thread_pool, &m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

void DTM_Generator::gen(
	std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<DTM_Final_Entry>>> sub_dtm,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_dtm_by_material = std::move(sub_dtm);

	const auto ckpt_path = paths.dtm_checkpoint_path(m_epsi);
	int64_t resume_batch_idx = -1;
	int64_t resume_fusion_idx = -1;
	uint16_t resume_finished_ply = 0;
	uint16_t resume_max_dtm = 0;
	{
		Checkpoint_File ckpt{};
		if (read_checkpoint(ckpt_path, &ckpt))
		{
			resume_batch_idx = static_cast<int64_t>(ckpt.batch_idx);
			resume_fusion_idx = static_cast<int64_t>(ckpt.fusion_idx);
			resume_finished_ply = ckpt.finished_ply;
			resume_max_dtm = ckpt.max_dtm;
		}
		else
		{
			m_table->m_dtm[WHITE].remove_disk_files();
			m_table->m_dtm[BLACK].remove_disk_files();
		}
	}
	remove_checkpoint(ckpt_path);

	if (m_paging_budget_bytes == 0)
	{
		const size_t ng = m_table->m_dtm[WHITE].num_groups();
		std::vector<uint8_t> all_needed(ng, 1);
		apply_working_set(thread_pool, &m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], all_needed, all_needed);
	}

	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtm[WHITE], batch, 1, true,
			thread_pool->num_workers());
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
			refresh_active_metadata(m_table->m_dtm[WHITE]);

			uint16_t start_ply = 0;
			if (is_resume_fusion)
			{
				seed_iter_groups();
				start_ply = resume_finished_ply;
				m_max_dtm = resume_max_dtm;
			}
			else
			{
				m_max_dtm = init_entries(thread_pool);
			}

			try
			{
				iterate(thread_pool, start_ply);
			}
			catch (const DTM_Interrupted& e)
			{
				m_table->m_dtm[WHITE].evict_all();
				m_table->m_dtm[BLACK].evict_all();
				Checkpoint_File ckpt{};
				ckpt.batch_idx = static_cast<uint32_t>(bi);
				ckpt.fusion_idx = static_cast<uint32_t>(fi);
				ckpt.finished_ply = e.finished_ply;
				ckpt.max_dtm = e.max_dtm;
				write_checkpoint(ckpt_path, ckpt);
				std::printf("\n  interrupted: checkpoint written\n");
				std::fflush(stdout);
				throw;
			}
		}
	}

	m_sub_dtm_by_material.clear();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

using DTM_Save_Cache = Save_Group_Cache<DTM_Final_Entry, DTM_Intermediate_Entry>;

void gather_dtm_info(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	DTM_Save_Cache& cache,
	Color color,
	size_t num_positions,
	size_t max_workers,
	EGTB_Info& info,
	Value_Histogram& hist)
{
	// hist_2b unused (DTM halves in both tiers); only hist_1b is gathered.
	const auto bins = gather_egtb_info_parallel<Value_Histogram>(
		thread_pool, epsi, cache, static_cast<size_t>(color), color,
		num_positions, max_workers, info,
		[](Value_Histogram& h, size_t /*idx*/, const DTM_Final_Entry& e) {
			// DRAW/ILLEGAL: WDL companion is authoritative — exclude from
			// histogram so rank table spends short codes on W/L values.
			if (!e.is_draw())
			{
				const uint16_t v = dtm_value_for_storage(e);
				if (v < Value_Histogram::HIST_BINS) ++h.hist_1b[v];
			}
		});

	for (const Value_Histogram& b : bins)
		for (size_t k = 0; k < Value_Histogram::HIST_BINS; ++k)
			hist.hist_1b[k] += b.hist_1b[k];
}

}  // namespace

void DTM_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto colors = table_colors();
	const auto t_save_start = std::chrono::steady_clock::now();

	m_info.clear();
	m_epsi.prepare_orbit_weight_table();

	const size_t bytes_per_group = m_table->m_dtm[WHITE].slices_per_group()
		* m_table->m_dtm[WHITE].within_slice_size() * sizeof(DTM_Final_Entry);
	size_t cap_groups;
	size_t max_workers;
	if (m_paging_budget_bytes == 0 || bytes_per_group == 0)
	{
		cap_groups = std::numeric_limits<size_t>::max();
		max_workers = 0;
	}
	else
	{
		// Budget each resident group plus its eviction compress scratch.
		const size_t per_group = bytes_per_group + spill_compress_scratch_bytes(bytes_per_group);
		cap_groups = std::max<size_t>(1, m_paging_budget_bytes / per_group);
		max_workers = cap_groups;
	}

	Compressed_EGTB dtm_save[COLOR_NB];
	Value_Histogram dtm_hist[COLOR_NB];
	uint32_t dtm_index_perm[COLOR_NB] = { 0, 0 };

	DTM_Save_Cache cache(&m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], cap_groups);

	const size_t num_positions = m_epsi.num_positions();

	for (Color me : colors)
	{
		gather_dtm_info(thread_pool, m_epsi, cache, me, num_positions, max_workers, m_info, dtm_hist[me]);

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board_unchecked().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	// Both tiers store the same halved value; tier only affects rank-index width.
	Value_Rank_Table dtm_rank[COLOR_NB];
	size_t dtm_entry_bytes[COLOR_NB]{};
	for (Color me : colors)
	{
		dtm_rank[me] = Value_Rank_Table::build(dtm_hist[me].hist_1b);
		dtm_entry_bytes[me] = (dtm_rank[me].ranks.size() <= 256) ? 1 : 2;
	}

	for (Color me : colors)
	{
		if (dtm_rank[me].ranks.size() > 1)
		{
			dtm_index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_entry_block_source(
						m_table->m_dtm[me], cache, me,
						make_index_permutation_plan(m_epsi, perm),
						DTM_BLOCK_SIZE, dtm_entry_bytes[me]);
				},
				DTM_BLOCK_SIZE,
				std::make_unique<LZMA_Rank_Compress_Helper>(
					dtm_rank[me], dtm_entry_bytes[me], &dtm_storage_fn),
				/*max_samples=*/64,
				"choose_dtm_storage");
		}
		Block_Source src = make_entry_block_source(
			m_table->m_dtm[me], cache, me,
			make_index_permutation_plan(m_epsi, dtm_index_perm[me]),
			DTM_BLOCK_SIZE, dtm_entry_bytes[me]);
		dtm_save[me] = save_compress_egtb(
			thread_pool, src, me, dtm_entry_bytes[me], DTM_BLOCK_SIZE,
			paths.block_spill_path(m_epsi, me),
			max_workers, dtm_rank[me], &dtm_storage_fn);
		cache.purge(me);
		m_table->m_dtm[me].remove_disk_files();
		m_table->m_dtm[me].close();
	}

	if (m_is_symmetric)
	{
		cache.purge(BLACK);
		m_table->m_dtm[BLACK].remove_disk_files();
		m_table->m_dtm[BLACK].close();
	}

	save_egtb_table(m_epsi, dtm_index_perm, dtm_save, paths.dtm_save_path(m_epsi), colors, EGTB_Magic::DTM_MAGIC);

	std::ofstream fp(paths.dtm_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
