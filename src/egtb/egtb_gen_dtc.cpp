#include "egtb/egtb_gen_dtc.h"
#include "egtb/egtb_compress.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/dispatch.h"
#include "util/math.h"
#include "util/progress_bar.h"
#include "util/utility.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

DTC_Generator::DTC_Generator(
	const Piece_Config& ps,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTC_Table>(ps, tmp_dir))
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	const size_t bytes_per_color =
		m_table->m_dtc[WHITE].num_slices()
		* m_table->m_dtc[WHITE].within_slice_size()
		* sizeof(DTC_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_iter_state(
		m_table->m_dtc[WHITE].num_groups(),
		m_table->m_dtc[WHITE].num_entries());
}

WDL_Entry DTC_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move) const
{
	Color sub_color;
	const Piece_Config_For_Gen* sub_epsi = nullptr;
	const Board_Index sub_idx = next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

	if (sub_epsi == nullptr) return WDL_Entry::DRAW;
	auto it = m_sub_wdl_by_material.find(sub_epsi->min_material_key());
	if (it == m_sub_wdl_by_material.end()) return WDL_Entry::DRAW;
	return it->second->read(sub_color, sub_idx);
}

WDL_Entry DTC_Generator::effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move) const
{
	const WDL_Entry no_ep = read_post_move_wdl(pos_gen, dp_move);

	// do_move/undo_move on pos_gen's own board: pos_gen's cached board matches
	// its index again after the undo, so callers see no change.
	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const Piece captured_by_dp = p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	WDL_Entry best_ep_for_opp = WDL_Entry::LOSE;
	bool any_ep = false;

	// Post-DP position is shared across both ep targets; build lazily.
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
		const WDL_Entry w_after_ep = read_sub_tb(*p_gen_for_ep, ep_move);
		WDL_Entry w_opp;
		switch (w_after_ep)
		{
			case WDL_Entry::WIN:          w_opp = WDL_Entry::LOSE;         break;
			case WDL_Entry::CURSED_WIN:   w_opp = WDL_Entry::BLESSED_LOSS; break;
			case WDL_Entry::DRAW:         w_opp = WDL_Entry::DRAW;         break;
			case WDL_Entry::BLESSED_LOSS: w_opp = WDL_Entry::CURSED_WIN;   break;
			case WDL_Entry::LOSE:         w_opp = WDL_Entry::WIN;          break;
			default:                      continue;
		}
		if (static_cast<int>(w_opp) > static_cast<int>(best_ep_for_opp))
			best_ep_for_opp = w_opp;
		any_ep = true;
	}

	p.undo_move(dp_move, captured_by_dp);

	if (!any_ep) return no_ep;
	return static_cast<int>(best_ep_for_opp) > static_cast<int>(no_ep)
		? best_ep_for_opp : no_ep;
}

WDL_Entry DTC_Generator::read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	return read_dtc<DTC_Final_Entry>(color_opp(mover), post_idx).wdl();
}

namespace {

INLINE bool is_pred_cursed(bool cursed, uint16_t value)
{
	return cursed || value >= DTC_Final_Entry::MAX_NON_CURSED_DTZ;
}

} // namespace

DTC_Any_Entry DTC_Generator::make_initial_entry(Position_For_Gen& pos_gen) const
{
	enum Value : int {
		ValueNone        = -32767,
		ValueClassicLoss = -2,
		ValueCursedLoss  = -1,
		ValueDraw        =  0,
		ValueCursedWin   =  1,
		ValueClassicWin  =  2,
	};

	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTC_Final_Entry::make_illegal();

	Position& pos = pos_gen.board_unchecked();

	auto fold = [](Value& best, WDL_Entry opp_wdl) {
		Value v;
		switch (opp_wdl)
		{
			case WDL_Entry::LOSE:         v = ValueClassicWin;  break;
			case WDL_Entry::BLESSED_LOSS: v = ValueCursedWin;   break;
			case WDL_Entry::DRAW:         v = ValueDraw;        break;
			case WDL_Entry::CURSED_WIN:   v = ValueCursedLoss;  break;
			case WDL_Entry::WIN:          v = ValueClassicLoss; break;
			default:                      return;
		}
		update_max(best, v);
	};

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	bool any_legal = false;
	bool any_quiet_legal = false;
	Value best = ValueNone;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		const bool is_cap   = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo = m.is_promotion();
		const bool is_pawn  = piece_type(pos.piece_at(m.from())) == PAWN;

		if (is_cap || is_promo) {
			fold(best, read_sub_tb(pos_gen, m));
		} else if (is_pawn) {
			fold(best, is_pawn_double_push(m)
				? effective_opp_wdl_after_dp(pos_gen, m)
				: read_post_move_wdl(pos_gen, m));
		} else {
			any_quiet_legal = true;
		}

		if (best >= ValueClassicWin) break;
	}
	if (!any_legal)
	{
		// Stalemate: plain Intermediate; no pre_quiet pred lands here, never reclassified.
		if (ctx.in_check) return DTC_Final_Entry::make_loss(0);
		return DTC_Intermediate_Entry{};
	}

	if (best == ValueClassicWin) return DTC_Final_Entry::make_win(1);

	if (!any_quiet_legal)
	{
		switch (best)
		{
		case ValueCursedWin:
		{
			DTC_Final_Entry e = DTC_Final_Entry::make_win(1);
			e.set_flag(DTC_FLAG_CAP_CWIN);
			return e;
		}
		case ValueDraw:        return DTC_Intermediate_Entry{};
		case ValueCursedLoss:
		{
			DTC_Final_Entry e = DTC_Final_Entry::make_loss(1);
			e.set_flag(DTC_FLAG_CAP_CLOSS);
			return e;
		}
		case ValueClassicLoss: return DTC_Final_Entry::make_loss(1);
		default: break;
		}
	}

	// Cache the best zeroing outcome on the Intermediate.
	switch (best)
	{
	case ValueCursedWin:   return DTC_Intermediate_Entry::make_cap_cwin();
	case ValueCursedLoss:  return DTC_Intermediate_Entry::make_cap_closs();
	case ValueDraw:        return DTC_Intermediate_Entry::make_cap_draw();
	default: break;
	}
	return DTC_Intermediate_Entry{};
}

bool DTC_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtc[WHITE].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtc[WHITE].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;
	const auto& targets_by_pid = m_targets_by_pid;
	bool pending_cursed = false;

	// Working set: both colors at g + (push_target_pid, same_kid) on opp.
	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
		if (2 * ngroups * spg * m_epsi.within_slice_size() * sizeof(DTC_Final_Entry) <= m_paging_budget_bytes)
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
		apply_working_set(thread_pool, &m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
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

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			bool any_cursed_hint = false;
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
					write_dtc(WHITE, idx, DTC_Final_Entry::make_illegal());
					write_dtc(BLACK, idx, DTC_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board_unchecked());
					if (canon != idx)
					{
						write_dtc(WHITE, idx, DTC_Final_Entry::make_illegal());
						write_dtc(BLACK, idx, DTC_Final_Entry::make_illegal());
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
					std::visit(overload{
						[&](DTC_Final_Entry entry) {
							write_dtc(us, idx, entry);
							if (!entry.is_illegal())
							{
								if (entry.is_cursed())
									any_cursed_hint = true;
								mark_iter(us, idx, m_table->m_dtc[us]);
							}
						},
						[&](DTC_Intermediate_Entry entry) {
							write_dtc(us, idx, entry);
							// Only cursed hints seed the cursed phase; cap_draw does not.
							if (entry.has_cap_cursed())
								any_cursed_hint = true;
						},
					}, make_initial_entry(pos_gen));
				}
			}
			return any_cursed_hint;
		});
		for (bool r : rets) if (r) pending_cursed = true;
	}

	progress_bar.set_finished();

	return pending_cursed;
}

void DTC_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_paging_budget_bytes == 0) return;
	const Color opp = color_opp(me);
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtc[me].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const auto& ksm = m_epsi.king_slice_manager();

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, ntotal);

	const size_t ngroups = m_table->m_dtc[WHITE].num_groups();
	auto& need_me  = m_scratch_need[me];
	auto& need_opp = m_scratch_need[opp];
	need_me.assign(ngroups, 0);
	need_opp.assign(ngroups, 0);
	need_me[group_id] = 1;

	if ((ngroups + 1) * spg * m_epsi.within_slice_size() * sizeof(DTC_Final_Entry) <= m_paging_budget_bytes)
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
			}
		}
	}

	apply_working_set(thread_pool, &m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

template <DTC_Generator::Iter_Phase Phase>
DTC_Generator::Loss_Verification_Result DTC_Generator::check_loss(
	Position_For_Gen& pos_gen,
	uint16_t ply, DTC_Intermediate_Entry hint) const
{
	constexpr bool cursed_phase = (Phase == Iter_Phase::CURSED);
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	Loss_Verification_Result r;

	if constexpr (!cursed_phase)
	{
		if (hint.has_any_hint())
			return r;
	}
	else
	{
		if (hint.has_cap_draw() || hint.has_cap_cwin())
			return r;
	}

	bool any_legal = false;
	bool any_zeroing = false;
	uint16_t max_contribution = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		const bool is_cap_or_ep = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo     = m.is_promotion();
		const bool is_pawn_move = piece_type(pos.piece_at(m.from())) == PAWN;
		if (is_cap_or_ep || is_promo || is_pawn_move)
		{
			any_zeroing = true;
			continue;
		}

		const Board_Index child = next_quiet_index(pos_gen, m);
		const auto ce = read_dtc<DTC_Final_Entry>(opp, child);
		if (!ce.is_win()) return r;
		const bool cursed = is_pred_cursed(ce.is_cursed(), ce.value());
		if constexpr (cursed_phase)
		{
			if (!cursed) continue;
		}
		else if (cursed) return r;
		// Strict ply-ordering: child dtz < ply (child must already be classified).
		if (ce.value() >= ply) return r;
		update_max(max_contribution, static_cast<uint16_t>(ce.value() + 1));
	}

	if (!any_legal) return r;

	// Zeroing dtz contribution (1 ply after the zeroing move): clean counts any
	// opp-win zeroing move; cursed counts only the cap_closs (cursed-win) case.
	if (any_zeroing)
	{
		if constexpr (!cursed_phase)
			update_max<uint16_t>(max_contribution, 1);
		else if (hint.has_cap_closs())
			update_max<uint16_t>(max_contribution, 1);
	}

	if (max_contribution != ply) return r;

	r.is_loss  = true;
	r.loss_dtz = max_contribution;
	return r;
}

void DTC_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtc<DTC_Final_Entry>(opp, pred);
		if (e.is_illegal() || !e.is_draw()) continue;
		write_dtc(opp, pred, DTC_Final_Entry::make_win(1));
	}
}

void DTC_Generator::retro_mark_changed(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const DTC_Final_Entry fe = read_dtc<DTC_Final_Entry>(opp, pred);
		if (fe.is_illegal() || !fe.is_draw()) continue;
		const auto& ie = reinterpret_cast<const DTC_Intermediate_Entry&>(fe);
		if (ie.has_change()) continue;
		// Atomic OR: a racing retro_mark_wins Final write keeps its bits; the
		// CHANGE bit lands on top harmlessly (Final dispatch ignores it).
		m_table->m_dtc[opp].lock_add_flags(pred, DTC_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtc[opp]);
	}
}

template <DTC_Generator::Iter_Phase Phase>
void DTC_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    uint16_t target_dtz)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	Move_List ml;
	pos.gen_pseudo_legal_pre_quiets(out_param(ml));
	DTC_Final_Entry new_e = DTC_Final_Entry::make_win(target_dtz);
	const bool cursed = Phase == Iter_Phase::CURSED
		|| target_dtz > DTC_Final_Entry::MAX_NON_CURSED_DTZ;
	if (cursed) new_e.set_flag(DTC_FLAG_CAP_CWIN);
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtc<DTC_Final_Entry>(opp, pred);
		if constexpr (Phase == Iter_Phase::CLEAN)
		{
			if (e.is_illegal() || !e.is_draw()) continue;
		}
		else
		{
			if (e.is_illegal() || e.is_loss()) continue;
			if (e.is_win())
			{
				if (!e.is_cursed() && cursed) continue;
				if (e.value() <= target_dtz) continue;
			}
		}
		write_dtc(opp, pred, new_e);
		mark_iter(opp, pred, m_table->m_dtc[opp]);
	}
}

bool DTC_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                             Color stm, uint16_t ply, Iter_Phase phase)
{
	return TEMPLATE_DISPATCH(
		(Template_Dispatch<Iter_Phase, Iter_Phase::CLEAN, Iter_Phase::CURSED>(phase)),
		run_iter_impl, thread_pool, stm, ply
	);
}

template <DTC_Generator::Iter_Phase Phase>
bool DTC_Generator::run_iter_impl(In_Out_Param<Thread_Pool> thread_pool,
                                  Color stm, uint16_t ply)
{
	const size_t spg = m_table->m_dtc[stm].slices_per_group();
	const auto& pid_in_pair = m_pid_in_pair;

	bool any_global = false;

	struct Iter_Result { bool any = false; bool any_intermediate = false; uint16_t max_classified = 0; };

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

					const DTC_Any_Entry cell = read_dtc_any(stm, idx);

					// Class-driven retrograde: a WIN flags its quiet preds CHANGE
					// (at its own dtz and one ply later); flagged preds reverify
					// via check_loss; a verified LOSS retros its preds to WIN(dtz+1).
					// Visit fuses chunk-state bookkeeping with the action decode;
					// bare DRAW Intermediates stay inert.
					const Iter_Action action = std::visit(overload{
						[&](DTC_Intermediate_Entry ie) -> Iter_Action {
							if (ie.has_cap_cursed() || ie.has_change())
								chunk.any_intermediate = true;
							if constexpr (Phase == Iter_Phase::CLEAN)
							{
								if (ie.has_change()) return Iter_Action::CHANGE_REVERIFY;
								return Iter_Action::SKIP;
							}
							else
							{
								// Seed the cursed phase at ply 1 (low pass): bridge the
								// cap_cwin/cap_closs routing hints in.
								if (ply == 1)
								{
									if (ie.has_cap_cwin())  return Iter_Action::PROMOTE_CWIN;
									if (ie.has_cap_closs()) return Iter_Action::CAPT_CLOSS_REVERIFY;
								}
								if (ie.has_change())
									return ie.has_cap_closs()
										? Iter_Action::CAPT_CLOSS_REVERIFY
										: Iter_Action::CHANGE_REVERIFY;
								return Iter_Action::SKIP;
							}
						},
						[&](DTC_Final_Entry fe) -> Iter_Action {
							if (fe.is_illegal()) return Iter_Action::SKIP;
							update_max(chunk.max_classified, static_cast<uint16_t>(
									(Phase == Iter_Phase::CLEAN && fe.is_cursed())
										? fe.value() + DTC_Final_Entry::MAX_NON_CURSED_DTZ
										: fe.value()));
							if (ply == 0)
							{
								// Mate: preds become WIN(1).
								if (fe.is_loss() && fe.value() == 0) return Iter_Action::MARK_WIN_IN_1;
								return Iter_Action::SKIP;
							}
							if constexpr (Phase == Iter_Phase::CLEAN)
							{
								// Clean phase ignores cursed entries entirely.
								if (fe.is_cursed()) return Iter_Action::SKIP;
							}
							else
							{
								// Cursed phase advance cursed entries plus the value-100
								// boundary; is_pred_cursed's `>= 100` covers both, so the
								// clean value-100 entries seed the first cursed results when
								// the cursed phase reaches ply 101.
								if (!is_pred_cursed(fe.is_cursed(), fe.value()))
									return Iter_Action::SKIP;
							}
							if (fe.is_win() && (fe.value() == ply || fe.value() == ply - 1))
								return Iter_Action::MARK_CHANGED;
							if (fe.is_loss() && fe.value() == ply)
								return Iter_Action::MARK_WIN_PREDS;
							return Iter_Action::SKIP;
						}
					}, cell);
					if (action == Iter_Action::SKIP) continue;

					if (prev != BOARD_INDEX_NONE
					    && static_cast<size_t>(idx) == static_cast<size_t>(prev) + 1)
						++pos_gen;
					else
						pos_gen.set_board_index(idx);
					prev = idx;

					switch (action)
					{
					case Iter_Action::MARK_WIN_IN_1:
						retro_mark_win_in_1(pos_gen);
						local.any = true;
						break;
					case Iter_Action::MARK_CHANGED:
						retro_mark_changed(pos_gen);
						local.any = true;
						break;
					case Iter_Action::PROMOTE_CWIN:
					{
						// Promote the Intermediate's cap_cwin hint to a Final cursed WIN(1).
						DTC_Final_Entry promoted = DTC_Final_Entry::make_win(1);
						promoted.set_flag(DTC_FLAG_CAP_CWIN);
						write_dtc(stm, idx, promoted);
						retro_mark_changed(pos_gen);
						local.any = true;
						break;
					}
					case Iter_Action::MARK_WIN_PREDS:
					{
						const auto fe = std::get<DTC_Final_Entry>(cell);
						retro_mark_wins<Phase>(pos_gen, fe.value() + 1);
						local.any = true;
						break;
					}
					case Iter_Action::CHANGE_REVERIFY:
					case Iter_Action::CAPT_CLOSS_REVERIFY:
					{
						auto ie = std::get<DTC_Intermediate_Entry>(cell);
						const auto res = check_loss<Phase>(pos_gen, ply, ie);
						if (!res.is_loss)
						{
							// Failed verify: clear CHANGE, preserve CAP_*.
							ie.clear_flag(DTC_FLAG_CHANGE);
							write_dtc(stm, idx, ie);
							break;
						}
						DTC_Final_Entry new_e = DTC_Final_Entry::make_loss(res.loss_dtz);
						if constexpr (Phase == Iter_Phase::CURSED)
							new_e.set_flag(DTC_FLAG_CAP_CLOSS);
						write_dtc(stm, idx, new_e);
						retro_mark_wins<Phase>(pos_gen, res.loss_dtz + 1);
						local.any = true;
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
			if (r.any) any_global = true;
			if (r.any_intermediate) any_intermediate = true;
			update_max(max_classified, r.max_classified);
		}
		// Evict: no Intermediates and every classified value < ply-1 → no
		// dispatch will fire here again. Later writes reinstate via mark_iter.
		if (!any_intermediate && max_classified + 1 < ply)
			m_iter_groups[stm][g] = 0;
	}
	return any_global;
}

void DTC_Generator::iterate(In_Out_Param<Thread_Pool> thread_pool, bool pending_cursed,
                            Iter_Phase start_phase, uint16_t finished_ply)
{
	auto check_interrupt = [](Iter_Phase phase, bool pending_cursed, uint16_t fp) {
		if (egtb_is_interrupt_requested())
			throw DTC_Interrupted{ static_cast<uint8_t>(phase), pending_cursed, fp };
	};

	bool finished = false;
	if (start_phase == Iter_Phase::CLEAN)
	{
		if (finished_ply == 0)
		{
			(void)run_iter(thread_pool, WHITE, 0, Iter_Phase::CLEAN);
			(void)run_iter(thread_pool, BLACK, 0, Iter_Phase::CLEAN);
		}

		while (!finished && finished_ply < DTC_Final_Entry::MAX_NON_CURSED_DTZ)
		{
			++finished_ply;
			finished = true;
			std::printf("  iterate clean %4u\r", finished_ply); std::fflush(stdout);
			const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CLEAN);
			const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CLEAN);
			if (wrote_w || wrote_b) finished = false;
			check_interrupt(Iter_Phase::CLEAN, pending_cursed, finished_ply);
		}
		if (!pending_cursed && finished)
			return;
		start_phase = Iter_Phase::CURSED;
		finished_ply = pending_cursed ? 0 : DTC_Final_Entry::MAX_NON_CURSED_DTZ;
		pending_cursed = !finished;
	}

	if (start_phase == Iter_Phase::CURSED && finished_ply < DTC_Final_Entry::MAX_NON_CURSED_DTZ)
	{
		finished = false;
		while (!finished && finished_ply < DTC_Final_Entry::MAX_NON_CURSED_DTZ)
		{
			++finished_ply;
			finished = true;
			std::printf("  iterate cursed %4u\r", finished_ply); std::fflush(stdout);
			const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CURSED);
			const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CURSED);
			if (wrote_w || wrote_b) finished = false;
			check_interrupt(Iter_Phase::CURSED, pending_cursed, finished_ply);
		}
		if (!pending_cursed && finished)
			return;
		finished_ply = DTC_Final_Entry::MAX_NON_CURSED_DTZ;
	}

	finished = false;
	while (!finished && finished_ply < DTC_SCORE_MAX)
	{
		++finished_ply;
		finished = true;
		std::printf("  iterate cursed %4u\r", finished_ply); std::fflush(stdout);
		const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CURSED);
		const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CURSED);
		if (wrote_w || wrote_b) finished = false;
		check_interrupt(Iter_Phase::CURSED, false, finished_ply);
	}
}

void DTC_Generator::gen(
	std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<WDL_Entry>>> sub_wdl,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_wdl_by_material = std::move(sub_wdl);

	const auto ckpt_path = paths.dtc_checkpoint_path(m_epsi);
	int64_t resume_batch_idx = -1;
	int64_t resume_fusion_idx = -1;
	Iter_Phase resume_phase = Iter_Phase::CLEAN;
	uint16_t resume_finished_ply = 0;
	bool resume_pending_cursed = false;
	{
		Checkpoint_File ckpt{};
		if (read_checkpoint(ckpt_path, &ckpt))
		{
			resume_batch_idx = static_cast<int64_t>(ckpt.batch_idx);
			resume_fusion_idx = static_cast<int64_t>(ckpt.fusion_idx);
			resume_phase = static_cast<Iter_Phase>(ckpt.phase);
			resume_finished_ply = ckpt.finished_ply;
			resume_pending_cursed = ckpt.pending_cursed;
		}
		else
		{
			m_table->m_dtc[WHITE].remove_disk_files();
			m_table->m_dtc[BLACK].remove_disk_files();
		}
	}
	remove_checkpoint(ckpt_path);

	if (m_paging_budget_bytes == 0)
	{
		const size_t ng = m_table->m_dtc[WHITE].num_groups();
		std::vector<uint8_t> all_needed(ng, 1);
		apply_working_set(thread_pool, &m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], all_needed, all_needed);
	}

	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtc[WHITE], batch, 1, false);
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
			refresh_active_metadata(m_table->m_dtc[WHITE]);

			bool pending_cursed = false;
			if (is_resume_fusion)
				seed_iter_groups();
			else
				pending_cursed = init_entries(thread_pool);

			try
			{
				if (is_resume_fusion)
					iterate(thread_pool, resume_pending_cursed, resume_phase, resume_finished_ply);
				else
					iterate(thread_pool, pending_cursed);
			}
			catch (const DTC_Interrupted& e)
			{
				m_table->m_dtc[WHITE].evict_all();
				m_table->m_dtc[BLACK].evict_all();
				Checkpoint_File ckpt{};
				ckpt.batch_idx = static_cast<uint32_t>(bi);
				ckpt.fusion_idx = static_cast<uint32_t>(fi);
				ckpt.phase = e.phase;
				ckpt.pending_cursed = e.pending_cursed;
				ckpt.finished_ply = e.finished_ply;
				write_checkpoint(ckpt_path, ckpt);
				std::printf("\n  interrupted: checkpoint written\n");
				std::fflush(stdout);
				throw;
			}

		}
	}

	m_sub_wdl_by_material.clear();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

using DTC_Save_Cache = Save_Group_Cache<DTC_Final_Entry, DTC_Intermediate_Entry>;
using DTC_Pinned_Range = Pinned_Group_Range<DTC_Final_Entry, DTC_Intermediate_Entry>;

struct Gather_Sink
{
	Color color;
	EGTB_Info* info;
	Value_Histogram* hist;
	std::mutex mu;
	std::vector<uint8_t> merged;
};

struct Singular_Probe_Result
{
	WDL_Entry singular;
	uint64_t  legal_cnt;
	uint64_t  illegal_cnt;
};

NODISCARD static Singular_Probe_Result singular_probe(
	const Piece_Config_For_Gen& epsi,
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>& src,
	DTC_Save_Cache& cache,
	Color color,
	size_t num_positions)
{
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t ns = src.num_slices();
	const size_t ng = src.num_groups();
	bool saw_win = false, saw_draw = false, saw_lose = false;
	uint64_t legal = 0, illegal = 0;

	Decomposed_Board_Index didx{};
	bool didx_init = false;
	for (size_t g = 0; g < ng; ++g)
	{
		DTC_Pinned_Range pin(cache, color, g, g);
		const size_t s_begin = g * spg;
		const size_t s_end   = std::min(s_begin + spg, ns);
		for (size_t s = s_begin; s < s_end; ++s)
		{
			const size_t base = s * within;
			if (base >= num_positions) break;
			const size_t end_in_slice = std::min(within, num_positions - base);
			const auto* const raw = src.template slice_view_as<DTC_Final_Entry>(s);
			if (!didx_init)
			{
				epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
				didx_init = true;
			}
			for (size_t i = 0; i < end_in_slice; ++i)
			{
				const auto& e = raw[i];
				const uint64_t w = epsi.orbit_weight(didx);
				switch (e.wdl())
				{
					case WDL_Entry::CURSED_WIN:
					case WDL_Entry::BLESSED_LOSS: return {WDL_Entry::ILLEGAL, 0, 0};
					case WDL_Entry::WIN:          saw_win = true;  legal   += w; break;
					case WDL_Entry::DRAW:         saw_draw = true; legal   += w; break;
					case WDL_Entry::LOSE:         saw_lose = true; legal   += w; break;
					case WDL_Entry::ILLEGAL:                       illegal += w; break;
				}
				if (static_cast<int>(saw_win) + static_cast<int>(saw_draw) + static_cast<int>(saw_lose) >= 2)
					return {WDL_Entry::ILLEGAL, 0, 0};
				epsi.step_to_next(inout_param(didx));
			}
		}
	}
	WDL_Entry s;
	if      (saw_win)  s = WDL_Entry::WIN;
	else if (saw_lose) s = WDL_Entry::LOSE;
	else if (saw_draw) s = WDL_Entry::DRAW;
	else               s = WDL_Entry::WIN;
	return {s, legal, illegal};
}

}  // namespace

static Block_Source make_wdl_block_source(
	const Piece_Config_For_Gen& epsi,
	DTC_Table& table,
	Color color,
	DTC_Save_Cache& cache,
	size_t num_positions,
	Gather_Sink& sink,
	uint32_t index_perm,
	bool gather)
{
	ASSERT(sink.color == color);
	auto& src = table.m_dtc[color];
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
	const auto perm_plan = make_index_permutation_plan(epsi, index_perm);
	return Block_Source{
		total_packed_bytes,
		[&epsi, &src, &cache, &sink, within, spg, num_positions, total_packed_bytes, perm_plan, gather](
			size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t>
		{
			const size_t byte_off = block_id * WDL_BLOCK_SIZE;
			const size_t byte_sz  = std::min<size_t>(WDL_BLOCK_SIZE, total_packed_bytes - byte_off);
			ASSERT(scratch.size() >= byte_sz);

			std::memset(scratch.data(), 0, byte_sz);
			auto* packed = reinterpret_cast<Packed_WDL_Entries*>(scratch.data());

			const size_t first_storage = byte_off * WDL_ENTRY_PACK_RATIO;
			const size_t end_storage   = std::min(first_storage + byte_sz * WDL_ENTRY_PACK_RATIO, num_positions);

			const size_t first_g = (first_storage / within) / spg;
			const size_t last_g  = (end_storage == first_storage ? first_g
			                                               : ((end_storage - 1) / within) / spg);
			DTC_Pinned_Range pin(cache, sink.color, first_g, last_g);

			thread_local std::vector<uint32_t> hist1_local;
			thread_local std::vector<uint32_t> hist2_local;
			hist1_local.assign(Value_Histogram::HIST_BINS, 0);
			hist2_local.assign(Value_Histogram::HIST_BINS, 0);
			EGTB_Info shard;

			for (size_t storage = first_storage; storage < end_storage; ++storage)
			{
				const size_t logical = storage_index_to_logical_index(perm_plan, storage);
				const auto& e = src.template view_at<DTC_Final_Entry>(static_cast<Board_Index>(logical));
				const size_t packed_byte = storage / WDL_ENTRY_PACK_RATIO - byte_off;
				const size_t in_packed   = storage % WDL_ENTRY_PACK_RATIO;
				set_wdl_entry(packed[packed_byte], in_packed, wdl_for_storage(e));

				if (gather)
				{
					Decomposed_Board_Index didx{};
					epsi.decompose_board_index(static_cast<Board_Index>(logical), out_param(didx));
					const uint64_t ow = epsi.orbit_weight(didx);
					shard.add_result(sink.color, e.wdl(), ow);
					if (e.is_win())
						shard.maybe_update_longest_win(sink.color, logical, e.value());
					// DRAW/ILLEGAL: WDL companion authoritative — exclude so
					// rank table spends short codes on W/L values.
					if (!e.is_draw())
					{
						++hist1_local[static_cast<size_t>(dtc_value_for_storage(e))];
						++hist2_local[static_cast<size_t>(static_cast<uint16_t>(e.value()))];
					}
				}
			}

			if (gather)
			{
				std::lock_guard<std::mutex> lk(sink.mu);
				if (!sink.merged[block_id])
				{
					sink.merged[block_id] = 1;
					sink.info->consolidate_from(&shard, &shard + 1, sink.color);
					for (size_t k = 0; k < Value_Histogram::HIST_BINS; ++k)
					{
						sink.hist->hist_1b[k] += hist1_local[k];
						sink.hist->hist_2b[k] += hist2_local[k];
					}
				}
			}

			if (!prepare_packed_wdl_entries_for_compression(
				Span<Packed_WDL_Entries>(packed, byte_sz)))
				return Const_Span<uint8_t>(scratch.data(), size_t{0});

			return Const_Span<uint8_t>(scratch.data(), byte_sz);
		}
	};
}

static void gather_dtc_info(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	DTC_Save_Cache& cache,
	Color color,
	size_t num_positions,
	size_t max_workers,
	EGTB_Info& info,
	Value_Histogram& hist)
{
	const auto bins = gather_egtb_info_parallel<Value_Histogram>(
		thread_pool, epsi, cache, static_cast<size_t>(color), color,
		num_positions, max_workers, info,
		[](Value_Histogram& h, size_t /*idx*/, const DTC_Final_Entry& e) {
			if (!e.is_draw())
			{
				++h.hist_1b[static_cast<size_t>(dtc_value_for_storage(e))];
				++h.hist_2b[static_cast<size_t>(static_cast<uint16_t>(e.value()))];
			}
		});

	for (const Value_Histogram& b : bins)
		for (size_t k = 0; k < Value_Histogram::HIST_BINS; ++k)
		{
			hist.hist_1b[k] += b.hist_1b[k];
			hist.hist_2b[k] += b.hist_2b[k];
		}
}

void DTC_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto colors = table_colors();

	const auto t_save_start = std::chrono::steady_clock::now();

	m_info.clear();
	m_epsi.prepare_orbit_weight_table();

	const size_t bytes_per_group = m_table->m_dtc[WHITE].slices_per_group()
		* m_table->m_dtc[WHITE].within_slice_size() * sizeof(DTC_Final_Entry);
	size_t cap_groups;
	size_t max_workers;
	if (m_paging_budget_bytes == 0 || bytes_per_group == 0)
	{
		cap_groups = std::numeric_limits<size_t>::max();
		max_workers = 0;
	}
	else
	{
		cap_groups = std::max<size_t>(1, m_paging_budget_bytes / bytes_per_group);
		max_workers = cap_groups;
	}

	Compressed_EGTB wdl_save[COLOR_NB];
	Compressed_EGTB dtc_save[COLOR_NB];
	Value_Histogram dtc_hist[COLOR_NB];
	uint32_t wdl_index_perm[COLOR_NB] = { 0, 0 };
	uint32_t dtc_index_perm[COLOR_NB] = { 0, 0 };

	DTC_Save_Cache cache(&m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], cap_groups);

	for (Color me : colors)
	{
		const Singular_Probe_Result probe = singular_probe(
			m_epsi, m_table->m_dtc[me], cache, me, m_epsi.num_positions());

		if (probe.singular != WDL_Entry::ILLEGAL)
		{
			if (probe.singular == WDL_Entry::DRAW)
			{
				m_info.draw_cnt[me]    = probe.legal_cnt;
				m_info.illegal_cnt[me] = probe.illegal_cnt;
			}
			else
			{
				gather_dtc_info(thread_pool, m_epsi, cache, me, m_epsi.num_positions(),
					max_workers, m_info, dtc_hist[me]);
			}
			std::printf("save wdl %d: singular\n", static_cast<int>(me));
			wdl_save[me] = Compressed_EGTB::make_singular(static_cast<uint8_t>(probe.singular));
		}
		else
		{
			const size_t num_positions = m_epsi.num_positions();
			const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
			const size_t num_blocks = ceil_div(total_packed_bytes, WDL_BLOCK_SIZE);

			// gather=false: only sink.color is read, so info/hist/merged stay empty.
			Gather_Sink trial_sink{me, nullptr, nullptr, {}, {}};
			wdl_index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_wdl_block_source(
						m_epsi, *m_table, me, cache, num_positions, trial_sink,
						perm, /*gather=*/false);
				},
				WDL_BLOCK_SIZE,
				std::make_unique<LZ4_Compress_Helper>(nullptr),
				/*max_samples=*/1024,
				"choose_wdl_storage");

			Gather_Sink sink{me, &m_info, &dtc_hist[me], {}, std::vector<uint8_t>(num_blocks, 0)};
			Block_Source src = make_wdl_block_source(
				m_epsi, *m_table, me, cache, num_positions, sink,
				wdl_index_perm[me], /*gather=*/true);
			wdl_save[me] = save_compress_wdl(
				thread_pool, src, me,
				paths.block_spill_path(m_epsi, me),
				max_workers);
		}

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board_unchecked().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	save_wdl_table(m_epsi, wdl_index_perm, wdl_save, paths.wdl_save_path(m_epsi), colors, EGTB_Magic::WDL_MAGIC);

	for (Color me : colors) wdl_save[me] = {};

	Value_Rank_Table dtc_rank_1b[COLOR_NB];
	Value_Rank_Table dtc_rank_2b[COLOR_NB];
	size_t dtc_entry_bytes[COLOR_NB]{};
	for (Color me : colors)
	{
		dtc_rank_1b[me] = Value_Rank_Table::build(dtc_hist[me].hist_1b);
		dtc_rank_2b[me] = Value_Rank_Table::build(dtc_hist[me].hist_2b);
		dtc_entry_bytes[me] = (dtc_rank_1b[me].ranks.size() <= 256) ? 1 : 2;
	}

	for (Color me : colors)
	{
		Value_Rank_Table& chosen = (dtc_entry_bytes[me] == 1) ? dtc_rank_1b[me] : dtc_rank_2b[me];
		if (chosen.ranks.size() > 1)
		{
			dtc_index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_entry_block_source(
						m_table->m_dtc[me], cache, me,
						make_index_permutation_plan(m_epsi, perm),
						DTC_BLOCK_SIZE, dtc_entry_bytes[me]);
				},
				DTC_BLOCK_SIZE,
				std::make_unique<LZMA_Rank_Compress_Helper>(
					chosen, dtc_entry_bytes[me], &dtc_storage_fn),
				/*max_samples=*/64,
				"choose_dtc_storage");
		}
		Block_Source src = make_entry_block_source(
			m_table->m_dtc[me], cache, me,
			make_index_permutation_plan(m_epsi, dtc_index_perm[me]),
			DTC_BLOCK_SIZE, dtc_entry_bytes[me]);
		dtc_save[me] = save_compress_egtb(
			thread_pool, src, me, dtc_entry_bytes[me], DTC_BLOCK_SIZE,
			paths.block_spill_path(m_epsi, me),
			max_workers, chosen);
		cache.purge(me);
		m_table->m_dtc[me].remove_disk_files();
		m_table->m_dtc[me].close();
	}

	if (m_is_symmetric)
	{
		cache.purge(BLACK);
		m_table->m_dtc[BLACK].remove_disk_files();
		m_table->m_dtc[BLACK].close();
	}

	save_egtb_table(m_epsi, dtc_index_perm, dtc_save, paths.dtc_save_path(m_epsi), colors, EGTB_Magic::DTC_MAGIC);

	std::ofstream fp(paths.dtc_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
