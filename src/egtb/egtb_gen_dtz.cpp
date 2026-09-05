#include "egtb/egtb_gen_dtz.h"
#include "egtb/relax_bound.h"
#include "egtb/egtb_compress.h"
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
#include <vector>

DTZ_Generator::DTZ_Generator(
	const Piece_Config& ps,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTZ_Table>(ps, tmp_dir))
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);

	const size_t bytes_per_color =
		m_table->m_dtz[WHITE].num_slices()
		* m_table->m_dtz[WHITE].within_slice_size()
		* sizeof(DTZ_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_iter_state(
		m_table->m_dtz[WHITE].num_groups(),
		m_table->m_dtz[WHITE].num_entries());
}

WDL_Entry DTZ_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move) const
{
	Color sub_color;
	const Piece_Config_For_Gen* sub_epsi = nullptr;
	const Board_Index sub_idx = next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

	if (sub_epsi == nullptr) return WDL_Entry::DRAW;
	auto it = m_sub_wdl_by_material.find(sub_epsi->min_material_key());
	if (it == m_sub_wdl_by_material.end()) return WDL_Entry::DRAW;
	return it->second->read(sub_color, sub_idx);
}

DTZ_Final_Entry DTZ_Generator::read_exit_dtz(const Exit_Site& site) const
{
	const auto* reader = m_exit_reader[site.giver][site.dropped];
	if (reader == nullptr) return DTZ_Final_Entry::make_draw();
	return reader->read(site.read_color, site.index);
}

WDL_Entry DTZ_Generator::effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move) const
{
	const WDL_Entry no_ep = read_post_move_wdl(pos_gen, dp_move);

	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const uint8_t rights_before = p.castling();
	const Square ep_sq = ep_square_of_double_push(dp_move);
	const Piece captured_by_dp = p.do_move(dp_move);

	WDL_Entry best_ep_for_opp = WDL_Entry::LOSE;
	bool any_ep = false;

	std::optional<Position_For_Gen> p_gen_for_ep;

	(void)p.visit_legal_ep_captures(ep_sq, [&](Move ep_move) FORCE_INLINE_LAMBDA {
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
			default:                      return false;
		}
		if (static_cast<int>(w_opp) > static_cast<int>(best_ep_for_opp))
			best_ep_for_opp = w_opp;
		any_ep = true;
		return false;
	});

	p.undo_move(dp_move, captured_by_dp, rights_before);

	if (!any_ep) return no_ep;
	return static_cast<int>(best_ep_for_opp) > static_cast<int>(no_ep)
		? best_ep_for_opp : no_ep;
}

WDL_Entry DTZ_Generator::read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = parent.move_is_capture(move);
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	return read_dtz<DTZ_Final_Entry>(color_opp(mover), post_idx).wdl();
}

namespace {

bool is_pred_cursed(bool cursed, uint16_t value)
{
	return cursed || value >= DTZ_Final_Entry::MAX_NON_CURSED_DTZ;
}

// Our distance across an exit: the twin's, plus the ply we spend leaving. A
// cursed value is kept odd because the 1-byte shipping tier halves cursed
// values round-up and recovers them as 2s-1, exact only for odd ones.
uint16_t compose_exit_dtz(uint16_t child_value, bool cursed)
{
	const uint16_t v = static_cast<uint16_t>(child_value + 1);
	return cursed ? static_cast<uint16_t>(v | 1u) : v;
}

} // namespace

FORCE_INLINE DTZ_Final_Entry DTZ_Generator::make_initial_entry(
	Position_For_Gen& pos_gen) const
{
	auto intermediate = [](DTZ_Intermediate_Entry entry) FORCE_INLINE_LAMBDA {
		return reinterpret_cast<const DTZ_Final_Entry&>(entry);
	};

	enum Value : int {
		ValueNone        = -32767,
		ValueClassicLoss = -2,
		ValueCursedLoss  = -1,
		ValueDraw        =  0,
		ValueCursedWin   =  1,
		ValueClassicWin  =  2,
	};

	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTZ_Final_Entry::make_illegal();

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

	bool any_legal = false;
	bool any_quiet_legal = false;
	bool any_zeroing = false;
	bool zeroing_classic_win = false;
	bool zeroing_cursed_win = false;
	constexpr uint16_t NO_EXIT_DTZ = std::numeric_limits<uint16_t>::max();
	uint16_t exit_win_dtz = NO_EXIT_DTZ;   // shortest classic win an exit reaches
	uint16_t exit_cwin_dtz = NO_EXIT_DTZ;  // shortest cursed win an exit reaches
	uint16_t exit_loss_clean = 0;
	uint16_t exit_loss_cursed = 0;
	Value best = ValueNone;

	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const bool is_cap   = pos.move_is_capture(m);
		const bool is_promo = m.is_promotion();
		const bool is_pawn  = piece_type(pos.piece_at(m.from())) == PAWN;

		if (is_cap || is_promo || is_pawn) {
			any_zeroing = true;
			const WDL_Entry w = (is_cap || is_promo)
				? read_sub_tb(pos_gen, m)
				: (is_pawn_double_push(m) ? effective_opp_wdl_after_dp(pos_gen, m)
				                          : read_post_move_wdl(pos_gen, m));
			fold(best, w);
			if (w == WDL_Entry::LOSE) zeroing_classic_win = true;
			if (w == WDL_Entry::BLESSED_LOSS) zeroing_cursed_win = true;
		} else if (Exit_Site site; resolve_castling(pos_gen, m, out_param(site))) {
			const DTZ_Final_Entry ce = read_exit_dtz(site);
			const bool cursed = is_pred_cursed(ce.is_cursed(), ce.value());
			const uint16_t d = compose_exit_dtz(ce.value(), cursed);
			WDL_Entry w;
			if (ce.is_loss())
			{
				if (cursed) { update_min(exit_cwin_dtz, d); w = WDL_Entry::BLESSED_LOSS; }
				else        { update_min(exit_win_dtz, d);  w = WDL_Entry::LOSE; }
			}
			else if (ce.is_win())
			{
				if (cursed) { update_max(exit_loss_cursed, d); w = WDL_Entry::CURSED_WIN; }
				else        { update_max(exit_loss_clean, d);  w = WDL_Entry::WIN; }
			}
			else
			{
				ASSERT(!ce.is_illegal());
				w = ce.wdl();
			}
			fold(best, w);
		} else {
			any_quiet_legal = true;
		}

		return zeroing_classic_win;
	});
	if (!any_legal)
	{
		if (pos.is_in_check()) return DTZ_Final_Entry::make_loss(0);
		return intermediate(DTZ_Intermediate_Entry{});
	}

	if (best == ValueClassicWin)
	{
		if (zeroing_classic_win) return DTZ_Final_Entry::make_win(1);
		ASSERT(exit_win_dtz != NO_EXIT_DTZ);
		return DTZ_Final_Entry::make_win(exit_win_dtz);
	}
	if (best == ValueCursedWin && !zeroing_cursed_win)
	{
		ASSERT(exit_cwin_dtz != NO_EXIT_DTZ);
		DTZ_Final_Entry e = DTZ_Final_Entry::make_win(exit_cwin_dtz);
		e.set_flag(DTZ_FLAG_CAP_CWIN);
		return e;
	}

	if (!any_quiet_legal)
	{
		auto loss_distance = [&](uint16_t exit_worst) {
			uint16_t d = any_zeroing ? uint16_t{1} : uint16_t{0};
			update_max(d, exit_worst);
			return d == 0 ? uint16_t{1} : d;
		};
		switch (best)
		{
		case ValueCursedWin:
		{
			DTZ_Final_Entry e = DTZ_Final_Entry::make_win(1);
			e.set_flag(DTZ_FLAG_CAP_CWIN);
			return e;
		}
		case ValueDraw:        return intermediate(DTZ_Intermediate_Entry{});
		case ValueCursedLoss:
		{
			DTZ_Final_Entry e =
				DTZ_Final_Entry::make_loss(loss_distance(exit_loss_cursed));
			e.set_flag(DTZ_FLAG_CAP_CLOSS);
			return e;
		}
		case ValueClassicLoss:
			return DTZ_Final_Entry::make_loss(loss_distance(exit_loss_clean));
		default: break;
		}
	}

	switch (best)
	{
	case ValueCursedWin:   return intermediate(DTZ_Intermediate_Entry::make_cap_cwin());
	case ValueCursedLoss:  return intermediate(DTZ_Intermediate_Entry::make_cap_closs(exit_loss_cursed));
	case ValueDraw:        return intermediate(DTZ_Intermediate_Entry::make_cap_draw());
	default: break;
	}
	return intermediate(DTZ_Intermediate_Entry::make_bound(exit_loss_clean));
}

DTZ_Generator::Init_Result DTZ_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const size_t spg = m_table->m_dtz[WHITE].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtz[WHITE].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;
	Init_Result result;

	auto pin = [&](const std::vector<uint8_t>& groups, size_t count) {
		m_scratch_need[WHITE] = groups;
		m_scratch_need[BLACK] = groups;
		return try_pin_phase(
			thread_pool, &m_table->m_dtz[WHITE], &m_table->m_dtz[BLACK],
			m_scratch_need[WHITE], m_scratch_need[BLACK], COLOR_NB * count);
	};
	const bool pinned = pin(m_all_groups, ngroups)
	                 || pin(m_fusion_init_groups, m_fusion_init_group_count);

	auto page_in_for_init_group = [&](size_t g) {
		if (pinned) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
		m_scratch_need[WHITE][g] = 1;
		const size_t g_start = g * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		mark_push_target_reach(m_scratch_need[WHITE].data(), g_start, g_end, spg);
		m_scratch_need[BLACK] = m_scratch_need[WHITE];
		apply_working_set(thread_pool, &m_table->m_dtz[WHITE], &m_table->m_dtz[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
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
			Init_Result local;
			size_t local_progress = 0;
			const auto& slice_has_stab = m_epsi.king_slice_manager().slice_has_stabilizer;
			for (const Board_Index idx : group_it.indices())
			{
				if (++local_progress % PROGRESS_BAR_UPDATE_PERIOD == 0)
					progress_bar += PROGRESS_BAR_UPDATE_PERIOD;
				const size_t pid_of_idx = m_epsi.pawn_slice_of(idx);
				if (!pid_in_pair[pid_of_idx]) continue;

				pos_gen.seek(idx);

				if (!pos_gen.is_legal())
				{
					write_dtz(WHITE, idx, DTZ_Final_Entry::make_illegal());
					write_dtz(BLACK, idx, DTZ_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board_unchecked());
					if (canon != idx)
					{
						write_dtz(WHITE, idx, DTZ_Final_Entry::make_illegal());
						write_dtz(BLACK, idx, DTZ_Final_Entry::make_illegal());
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
					const DTZ_Final_Entry entry = make_initial_entry(pos_gen);
					if (entry.is_illegal() || !entry.is_draw())
					{
						write_dtz(us, idx, entry);
						if (!entry.is_illegal())
						{
							if (entry.is_cursed())
								local.pending_cursed = true;
							update_max(local.max_classified,
							           static_cast<uint16_t>(entry.value()));
							mark_iter(us, idx, m_table->m_dtz[us]);
						}
					}
					else
					{
						const auto& ie = reinterpret_cast<const DTZ_Intermediate_Entry&>(entry);
						write_dtz(us, idx, ie);
						if (ie.has_cap_cursed())
							local.pending_cursed = true;
						update_max(local.max_classified, ie.bound());
					}
				}
			}
			return local;
		});
		for (const Init_Result& r : rets)
		{
			if (r.pending_cursed) result.pending_cursed = true;
			update_max(result.max_classified, r.max_classified);
		}
	}

	progress_bar.set_finished();

	return result;
}

void DTZ_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_phase_pinned) return;
	const Color opp = color_opp(me);
	const size_t spg = m_table->m_dtz[WHITE].slices_per_group();
	const size_t ngroups = m_table->m_dtz[WHITE].num_groups();
	auto& need_me  = m_scratch_need[me];
	auto& need_opp = m_scratch_need[opp];
	need_me.assign(ngroups, 0);
	need_opp.assign(ngroups, 0);
	need_me[group_id] = 1;

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, m_epsi.num_slices());
	mark_king_neighbor_reach(need_opp.data(), g_start, g_end, spg);

	apply_working_set(thread_pool, &m_table->m_dtz[WHITE], &m_table->m_dtz[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

template <DTZ_Generator::Iter_Phase Phase>
FORCE_INLINE DTZ_Generator::Loss_Verification_Result DTZ_Generator::check_loss(
	Position_For_Gen& pos_gen,
	uint16_t ply, DTZ_Intermediate_Entry hint) const
{
	constexpr bool cursed_phase = (Phase == Iter_Phase::CURSED);

	Loss_Verification_Result r;

	uint16_t exit_bound = 0;
	if constexpr (!cursed_phase)
	{
		if (hint.has_any_hint())
			return r;
		exit_bound = hint.bound();
	}
	else
	{
		if (hint.has_cap_draw() || hint.has_cap_cwin())
			return r;
		if (hint.has_cap_closs())
			exit_bound = hint.bound();
	}

	if (exit_bound > ply) return r;

	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	bool any_zeroing = false;
	uint16_t max_contribution = 0;
	bool any_legal = false;

	const bool disqualified = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const bool is_cap_or_ep = pos.move_is_capture(m);
		const bool is_promo     = m.is_promotion();
		const bool is_pawn_move = piece_type(pos.piece_at(m.from())) == PAWN;
		if (is_cap_or_ep || is_promo || is_pawn_move)
		{
			any_zeroing = true;
			return false;
		}

		if (m.is_castling()) return false;
		const Board_Index child = next_quiet_index(pos_gen, m);
		if (child == BOARD_INDEX_NONE) return false;

		const auto ce = read_dtz<DTZ_Final_Entry>(opp, child);
		if (!ce.is_win()) return true;
		const bool cursed = is_pred_cursed(ce.is_cursed(), ce.value());
		if constexpr (cursed_phase)
		{
			if (!cursed) return false;
		}
		else if (cursed) return true;
		if (ce.value() >= ply) return true;
		update_max(max_contribution, static_cast<uint16_t>(ce.value() + 1));
		return false;
	});

	if (disqualified) return r;

	if (!any_legal) return r;

	update_max(max_contribution, exit_bound);

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

FORCE_INLINE void DTZ_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	const bool wins_may_improve = m_epsi.has_castling();
	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const auto e = read_dtz<DTZ_Final_Entry>(opp, pred);
		if (e.is_illegal()) return;
		if (!e.is_draw())
		{
			if (!wins_may_improve || !e.is_win()) return;
			if (!e.is_cursed() && e.value() <= 1) return;
		}
		write_dtz(opp, pred, DTZ_Final_Entry::make_win(1));
	});
}

FORCE_INLINE void DTZ_Generator::retro_mark_changed(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const DTZ_Final_Entry fe = read_dtz<DTZ_Final_Entry>(opp, pred);
		if (fe.is_illegal() || !fe.is_draw()) return;
		const auto& ie = reinterpret_cast<const DTZ_Intermediate_Entry&>(fe);
		if (ie.has_change() || ie.has_cap_draw() || ie.has_cap_cwin()) return;
		m_table->m_dtz[opp].add_flags(pred, DTZ_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtz[opp]);
	});
}

template <DTZ_Generator::Iter_Phase Phase>
FORCE_INLINE void DTZ_Generator::retro_mark_wins(
	Position_For_Gen& pos_gen, uint16_t target_dtz)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	DTZ_Final_Entry new_e = DTZ_Final_Entry::make_win(target_dtz);
	const bool cursed = Phase == Iter_Phase::CURSED
		|| target_dtz > DTZ_Final_Entry::MAX_NON_CURSED_DTZ;
	if (cursed) new_e.set_flag(DTZ_FLAG_CAP_CWIN);
	const bool wins_may_improve = m_epsi.has_castling();

	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const auto e = read_dtz<DTZ_Final_Entry>(opp, pred);
		if constexpr (Phase == Iter_Phase::CLEAN)
		{
			if (e.is_illegal()) return;
			if (e.is_draw())
			{
				// A quiet cursed win does not beat a pending cursed capture in 1.
				if (cursed
				    && reinterpret_cast<const DTZ_Intermediate_Entry&>(e).has_cap_cwin())
					return;
			}
			else
			{
				// The boundary target never improves a classified cell: a clean
				// win here is at or below the boundary, and a seeded cursed win
				// is caught at cursed ply 100, where distances do compare.
				if (cursed) return;
				if (!wins_may_improve) return;
				if (!e.is_win()) return;
				// A clean win beats a cursed one whatever the distance says.
				if (!e.is_cursed() && e.value() <= target_dtz) return;
			}
		}
		else
		{
			if (e.is_illegal() || e.is_loss()) return;
			if (e.is_win())
			{
				if (!e.is_cursed() && cursed) return;
				if (e.value() <= target_dtz) return;
			}
		}
		write_dtz(opp, pred, new_e);
		mark_iter(opp, pred, m_table->m_dtz[opp]);
	});
}

bool DTZ_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                             Color stm, uint16_t ply, Iter_Phase phase)
{
	return TEMPLATE_DISPATCH(
		(Template_Dispatch<Iter_Phase, Iter_Phase::CLEAN, Iter_Phase::CURSED>(phase)),
		run_iter_impl, thread_pool, stm, ply
	);
}

template <DTZ_Generator::Iter_Phase Phase>
bool DTZ_Generator::run_iter_impl(In_Out_Param<Thread_Pool> thread_pool,
                                  Color stm, uint16_t ply)
{
	const size_t spg = m_table->m_dtz[stm].slices_per_group();
	const auto& pid_in_pair = m_pid_in_pair;

	bool any_global = false;

	struct Iter_Result { bool any = false; bool any_intermediate = false; uint16_t max_classified = 0; };

	for (size_t g : m_pair_group_ids)
	{
		if (m_iter_groups[stm][g] == 0) continue;

		page_in_for_group(thread_pool, stm, g);

		if (ply != 0)
		{
			const uint16_t mark_changed_value = static_cast<uint16_t>(ply - (stm ^ 1));
			const bool promote_cap_cwin = Phase == Iter_Phase::CURSED && ply == 1;

			Shared_Board_Index_Iterator mark_it = make_slice_group_iterator(g, spg);

			const auto mark_rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> bool {
				Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
				bool any = false;

				for (auto [chunk_start, chunk_end] : mark_it.chunks())
				{
					const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
					if (!m_iter_chunks[stm][cid]) continue;
					for (const auto [idx, fe] : make_in_pair_cells<DTZ_Final_Entry>(
							m_epsi, pid_in_pair, m_table->m_dtz[stm], chunk_start, chunk_end))
					{
						if (fe.is_illegal()) continue;
						if (fe.is_draw())
						{
							if (!promote_cap_cwin) continue;
							const auto& ie = reinterpret_cast<const DTZ_Intermediate_Entry&>(fe);
							if (!ie.has_cap_cwin()) continue;
							DTZ_Final_Entry promoted = DTZ_Final_Entry::make_win(1);
							promoted.set_flag(DTZ_FLAG_CAP_CWIN);
							write_dtz(stm, idx, promoted);
							any = true;
							if (mark_changed_value != 1) continue;
						}
						else
						{
							if (!fe.is_win()) continue;
							if (fe.value() != ply && fe.value() != ply - 1) continue;
							if constexpr (Phase == Iter_Phase::CLEAN)
							{
								if (fe.is_cursed()) continue;
							}
							else
							{
								if (!is_pred_cursed(fe.is_cursed(), fe.value())) continue;
							}
							any = true;
							if (fe.value() != mark_changed_value) continue;
						}
						pos_gen.seek(idx);
						retro_mark_changed(pos_gen);
					}
				}
				return any;
			});
			for (const bool r : mark_rets)
				if (r) any_global = true;
		}

		Shared_Board_Index_Iterator cell_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> Iter_Result {
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
			Iter_Result local;

			auto reverify = [&](Board_Index i, DTZ_Intermediate_Entry ie) FORCE_INLINE_LAMBDA {
				pos_gen.seek(i);
				const auto res = check_loss<Phase>(pos_gen, ply, ie);
				if (!res.is_loss)
				{
					if (ie.has_change())
					{
						ie.clear_flag(DTZ_FLAG_CHANGE);
						write_dtz(stm, i, ie);
					}
					return;
				}
				DTZ_Final_Entry new_e = DTZ_Final_Entry::make_loss(res.loss_dtz);
				if constexpr (Phase == Iter_Phase::CURSED)
					new_e.set_flag(DTZ_FLAG_CAP_CLOSS);
				write_dtz(stm, i, new_e);
				retro_mark_wins<Phase>(pos_gen, res.loss_dtz + 1);
				local.any = true;
			};

			for (auto [chunk_start, chunk_end] : cell_it.chunks())
			{
				const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
				if (!m_iter_chunks[stm][cid]) continue;
				Iter_Result chunk;
				for (const auto [idx, fe] : make_in_pair_cells<DTZ_Final_Entry>(
						m_epsi, pid_in_pair, m_table->m_dtz[stm], chunk_start, chunk_end))
				{
					if (fe.is_illegal()) continue;
					if (fe.is_draw())
					{
						const auto& ie = reinterpret_cast<const DTZ_Intermediate_Entry&>(fe);
						const uint16_t bound = ie.bound();
						const bool bound_due = ply != 0 && bound == ply
							&& ((Phase == Iter_Phase::CLEAN)
								? !ie.has_any_hint() : ie.has_cap_closs());
						if (ie.has_cap_cursed() || ie.has_change() || bound >= ply)
							chunk.any_intermediate = true;
						if constexpr (Phase == Iter_Phase::CLEAN)
						{
							if (!ie.has_change() && !bound_due) continue;
						}
						else
						{
							if (!((ply == 1 && ie.has_cap_closs())
							      || bound_due || ie.has_change()))
								continue;
						}
						reverify(idx, ie);
					}
					else
					{
						update_max(chunk.max_classified, static_cast<uint16_t>(
								(Phase == Iter_Phase::CLEAN && fe.is_cursed())
									? fe.value() + DTZ_Final_Entry::MAX_NON_CURSED_DTZ
									: fe.value()));
						if (!fe.is_loss() || fe.value() != ply) continue;
						if constexpr (Phase == Iter_Phase::CLEAN)
						{
							if (fe.is_cursed()) continue;
						}
						else
						{
							// Value 100 seeds the cursed phase boundary.
							if (!is_pred_cursed(fe.is_cursed(), fe.value())) continue;
						}
						pos_gen.seek(idx);
						if (ply == 0) retro_mark_win_in_1(pos_gen);
						else          retro_mark_wins<Phase>(pos_gen, fe.value() + 1);
						local.any = true;
					}
				}

				if (chunk.any_intermediate) local.any_intermediate = true;
				update_max(local.max_classified, chunk.max_classified);

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
		if (!any_intermediate && max_classified + 1 < ply)
			m_iter_groups[stm][g] = 0;
	}
	return any_global;
}

void DTZ_Generator::iterate(In_Out_Param<Thread_Pool> thread_pool, bool pending_cursed,
                            Iter_Phase start_phase, uint16_t finished_ply)
{
	auto check_interrupt = [this](Iter_Phase phase, bool pending_cursed, uint16_t fp) {
		if (egtb_is_interrupt_requested(true))
			throw DTZ_Interrupted{ static_cast<uint8_t>(phase), pending_cursed, fp, m_max_dtz };
	};

	auto iter_continues = [this](bool wrote_w, bool wrote_b, uint16_t ply) {
		return wrote_w || wrote_b || ply <= m_max_dtz;
	};

	auto pin = [&](const std::vector<uint8_t>& groups, size_t count) {
		m_scratch_need[WHITE] = groups;
		m_scratch_need[BLACK] = groups;
		return try_pin_phase(
			thread_pool, &m_table->m_dtz[WHITE], &m_table->m_dtz[BLACK],
			m_scratch_need[WHITE], m_scratch_need[BLACK], COLOR_NB * count);
	};
	m_phase_pinned = pin(m_all_groups, m_table->m_dtz[WHITE].num_groups())
	              || pin(m_fusion_groups, m_fusion_group_count);

	bool finished = false;
	if (start_phase == Iter_Phase::CLEAN)
	{
		if (finished_ply == 0)
		{
			(void)run_iter(thread_pool, WHITE, 0, Iter_Phase::CLEAN);
			(void)run_iter(thread_pool, BLACK, 0, Iter_Phase::CLEAN);
		}

		while (!finished && finished_ply < DTZ_Final_Entry::MAX_NON_CURSED_DTZ)
		{
			++finished_ply;
			finished = true;
			std::printf("  iterate clean %4u\r", finished_ply); std::fflush(stdout);
			const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CLEAN);
			const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CLEAN);
			if (iter_continues(wrote_w, wrote_b, finished_ply)) finished = false;
			check_interrupt(Iter_Phase::CLEAN, pending_cursed, finished_ply);
		}
		if (!pending_cursed && finished)
			return;
		start_phase = Iter_Phase::CURSED;
		finished_ply = pending_cursed ? 0 : DTZ_Final_Entry::MAX_NON_CURSED_DTZ;
		pending_cursed = !finished;
	}

	if (start_phase == Iter_Phase::CURSED && finished_ply < DTZ_Final_Entry::MAX_NON_CURSED_DTZ)
	{
		finished = false;
		while (!finished && finished_ply < DTZ_Final_Entry::MAX_NON_CURSED_DTZ)
		{
			++finished_ply;
			finished = true;
			std::printf("  iterate cursed %4u\r", finished_ply); std::fflush(stdout);
			const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CURSED);
			const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CURSED);
			if (iter_continues(wrote_w, wrote_b, finished_ply)) finished = false;
			check_interrupt(Iter_Phase::CURSED, pending_cursed, finished_ply);
		}
		if (!pending_cursed && finished)
			return;
		finished_ply = DTZ_Final_Entry::MAX_NON_CURSED_DTZ;
	}

	finished = false;
	while (!finished && finished_ply < DTZ_SCORE_MAX)
	{
		++finished_ply;
		finished = true;
		std::printf("  iterate cursed %4u\r", finished_ply); std::fflush(stdout);
		const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CURSED);
		const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CURSED);
		if (iter_continues(wrote_w, wrote_b, finished_ply)) finished = false;
		check_interrupt(Iter_Phase::CURSED, false, finished_ply);
	}
}

void DTZ_Generator::gen(
	Table_Reader_Map<WDL_Entry> sub_wdl,
	Table_Reader_Map<DTZ_Final_Entry> exit_dtz,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_wdl_by_material = std::move(sub_wdl);
	m_exit_dtz_by_material = std::move(exit_dtz);
	bind_exit_readers(m_exit_dtz_by_material, m_exit_reader);

	const auto ckpt_path = paths.dtz_checkpoint_path(m_epsi);
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
			m_max_dtz = ckpt.max_dtz;
			resume_pending_cursed = ckpt.pending_cursed;
		}
		else
		{
			m_table->m_dtz[WHITE].remove_disk_files(thread_pool);
			m_table->m_dtz[BLACK].remove_disk_files(thread_pool);
		}
	}
	remove_checkpoint(ckpt_path);

	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtz[WHITE], batch, 1, 1);
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
			refresh_active_metadata(m_table->m_dtz[WHITE]);

			bool pending_cursed = false;
			if (is_resume_fusion)
				seed_iter_groups();
			else
			{
				const Init_Result init = init_entries(thread_pool);
				pending_cursed = init.pending_cursed;
				m_max_dtz = init.max_classified;
			}

			try
			{
				if (is_resume_fusion)
					iterate(thread_pool, resume_pending_cursed, resume_phase, resume_finished_ply);
				else
					iterate(thread_pool, pending_cursed);
			}
			catch (const DTZ_Interrupted& e)
			{
				m_table->m_dtz[WHITE].evict_all(thread_pool);
				m_table->m_dtz[BLACK].evict_all(thread_pool);
				Checkpoint_File ckpt{};
				ckpt.batch_idx = static_cast<uint32_t>(bi);
				ckpt.fusion_idx = static_cast<uint32_t>(fi);
				ckpt.phase = e.phase;
				ckpt.pending_cursed = e.pending_cursed;
				ckpt.finished_ply = e.finished_ply;
				ckpt.max_dtz = e.max_dtz;
				write_checkpoint(ckpt_path, ckpt);
				throw;
			}

		}
	}

	m_sub_wdl_by_material.clear();
	m_exit_dtz_by_material.clear();
	thread_pool->respawn_all_threads();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

using DTZ_Save_Cache = Save_Group_Cache<DTZ_Final_Entry, DTZ_Intermediate_Entry>;
using DTZ_Pinned_Range = Pinned_Group_Range<DTZ_Final_Entry, DTZ_Intermediate_Entry>;

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
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>& src,
	DTZ_Save_Cache& cache,
	Color color,
	size_t num_positions,
	size_t max_workers)
{
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t ns = src.num_slices();
	const size_t ng = src.num_groups();

	struct Shard { bool saw_win = false, saw_draw = false, saw_lose = false;
	               uint64_t legal = 0, illegal = 0; };

	const size_t capped = (max_workers == 0)
		? thread_pool->num_workers()
		: std::min(thread_pool->num_workers(), max_workers);
	const size_t workers = std::max<size_t>(1, std::min(capped, ng));

	std::atomic<size_t> next_group(0);
	std::atomic<bool> not_singular(false);

	auto shards = thread_pool->run_sync_task_on_multiple_threads(
		workers, [&](size_t) -> Shard {
			Shard shard{};
			Decomposed_Board_Index didx;
			for (;;)
			{
				if (not_singular.load(std::memory_order_relaxed)) break;
				const size_t g = next_group.fetch_add(1, std::memory_order_relaxed);
				if (g >= ng) break;
				DTZ_Pinned_Range pin(cache, color, g, g);
				const size_t s_begin = g * spg;
				const size_t s_end   = std::min(s_begin + spg, ns);
				for (size_t s = s_begin; s < s_end; ++s)
				{
					const size_t base = s * within;
					if (base >= num_positions) break;
					const size_t end_in_slice = std::min(within, num_positions - base);
					const auto* const raw = src.template slice_view_as<DTZ_Final_Entry>(s);
					epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
					for (size_t i = 0; i < end_in_slice; ++i)
					{
						const auto& e = raw[i];
						const uint64_t w = epsi.orbit_weight(didx);
						switch (e.wdl())
						{
							case WDL_Entry::CURSED_WIN:
							case WDL_Entry::BLESSED_LOSS:
								not_singular.store(true, std::memory_order_relaxed);
								return shard;
							case WDL_Entry::WIN:  shard.saw_win = true;  shard.legal   += w; break;
							case WDL_Entry::DRAW: shard.saw_draw = true; shard.legal   += w; break;
							case WDL_Entry::LOSE: shard.saw_lose = true; shard.legal   += w; break;
							case WDL_Entry::ILLEGAL:                     shard.illegal += w; break;
						}
						if (static_cast<int>(shard.saw_win) + static_cast<int>(shard.saw_draw)
							+ static_cast<int>(shard.saw_lose) >= 2)
						{
							not_singular.store(true, std::memory_order_relaxed);
							return shard;
						}
						epsi.step_to_next(inout_param(didx));
					}
				}
			}
			return shard;
		});

	bool saw_win = false, saw_draw = false, saw_lose = false;
	uint64_t legal = 0, illegal = 0;
	for (const Shard& s : shards)
	{
		saw_win  |= s.saw_win;
		saw_draw |= s.saw_draw;
		saw_lose |= s.saw_lose;
		legal    += s.legal;
		illegal  += s.illegal;
	}
	if (not_singular.load(std::memory_order_relaxed)
		|| static_cast<int>(saw_win) + static_cast<int>(saw_draw) + static_cast<int>(saw_lose) >= 2)
		return {WDL_Entry::ILLEGAL, 0, 0};

	if (saw_win)  return {WDL_Entry::WIN,  legal, illegal};
	if (saw_lose) return {WDL_Entry::LOSE, legal, illegal};
	return {WDL_Entry::DRAW, legal, illegal};
}

}  // namespace

static Block_Source make_wdl_block_source(
	const Piece_Config_For_Gen& epsi,
	DTZ_Table& table,
	Color color,
	DTZ_Save_Cache& cache,
	size_t num_positions,
	Gather_Sink& sink,
	uint32_t index_perm,
	bool gather,
	const Relax_Bound* relax)
{
	ASSERT(sink.color == color);
	auto& src = table.m_dtz[color];
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
	const auto perm_plan = make_index_permutation_plan(epsi, index_perm);
	return Block_Source{
		total_packed_bytes,
		[&epsi, &src, &cache, &sink, within, spg, num_positions, total_packed_bytes, perm_plan,
		 gather, relax](
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
			DTZ_Pinned_Range pin(cache, sink.color, first_g, last_g);

			thread_local std::vector<uint32_t> hist1_local;
			thread_local std::vector<uint32_t> hist2_local;
			hist1_local.assign(Value_Histogram::HIST_BINS, 0);
			hist2_local.assign(Value_Histogram::HIST_BINS, 0);
			EGTB_Info shard;

			thread_local std::vector<WDL_Stored> caps;
			caps.assign(relax ? byte_sz * WDL_ENTRY_PACK_RATIO : 0, NOT_RELAXED);
			std::optional<Position_For_Gen> pos_gen;
			if (relax) pos_gen.emplace(epsi, BOARD_INDEX_ZERO, sink.color);

			for (size_t storage = first_storage; storage < end_storage; ++storage)
			{
				const size_t logical = storage_index_to_logical_index(perm_plan, storage);
				const auto& e = src.template view_at<DTZ_Final_Entry>(static_cast<Board_Index>(logical));
				const size_t packed_byte = storage / WDL_ENTRY_PACK_RATIO - byte_off;
				const size_t in_packed   = storage % WDL_ENTRY_PACK_RATIO;
				const WDL_Stored v = wdl_for_storage(e);
				set_wdl_entry(packed[packed_byte], in_packed, v);

				if (relax && is_relaxable_class(v))
				{
					pos_gen->seek(static_cast<Board_Index>(logical));
					pos_gen->set_turn(sink.color);
					caps[storage - first_storage] = relax->cap_for(*pos_gen, v);
				}

				if (gather)
				{
					Decomposed_Board_Index didx;
					epsi.decompose_board_index(static_cast<Board_Index>(logical), out_param(didx));
					const uint64_t ow = epsi.orbit_weight(didx);
					shard.add_result(sink.color, e.wdl(), ow);
					if (e.is_win())
						shard.maybe_update_longest_win(sink.color, logical, e.value());
					if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
					{
						++hist1_local[static_cast<size_t>(dtz_value_for_storage(e))];
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

			if (!prepare_wdl_entries_for_compression(
					Span<Packed_WDL_Entries>(packed, byte_sz),
					Const_Span<WDL_Stored>(caps.data(), caps.size())))
				return Const_Span<uint8_t>(scratch.data(), size_t{0});

			return Const_Span<uint8_t>(scratch.data(), byte_sz);
		}
	};
}

static void gather_dtz_info(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	DTZ_Save_Cache& cache,
	Color color,
	size_t num_positions,
	size_t max_workers,
	EGTB_Info& info,
	Value_Histogram& hist)
{
	const auto bins = gather_egtb_info_parallel<Value_Histogram>(
		thread_pool, epsi, cache, color, num_positions, max_workers, info,
		[](Value_Histogram& h, size_t /*idx*/, const DTZ_Final_Entry& e) {
			if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
			{
				++h.hist_1b[static_cast<size_t>(dtz_value_for_storage(e))];
				++h.hist_2b[static_cast<size_t>(static_cast<uint16_t>(e.value()))];
			}
		},
		[color](size_t) { return static_cast<size_t>(color); });

	for (const Value_Histogram& b : bins)
		for (size_t k = 0; k < Value_Histogram::HIST_BINS; ++k)
		{
			hist.hist_1b[k] += b.hist_1b[k];
			hist.hist_2b[k] += b.hist_2b[k];
		}
}

void DTZ_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto colors = table_colors();

	const auto t_save_start = std::chrono::steady_clock::now();

	m_info.clear();
	m_epsi.prepare_orbit_weight_table();

	const size_t bytes_per_group = m_table->m_dtz[WHITE].slices_per_group()
		* m_table->m_dtz[WHITE].within_slice_size() * sizeof(DTZ_Final_Entry);
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
	Compressed_EGTB dtz_save[COLOR_NB];
	Value_Histogram dtz_hist[COLOR_NB];
	uint32_t wdl_index_perm[COLOR_NB] = { 0, 0 };
	uint32_t dtz_index_perm[COLOR_NB] = { 0, 0 };

	DTZ_Save_Cache cache(&m_table->m_dtz[WHITE], &m_table->m_dtz[BLACK], cap_groups);

	std::optional<Relax_Bound> relax;
	if constexpr (EGTB_GEN_RELAXED)
		relax.emplace(m_epsi, paths, thread_pool);

	for (Color me : colors)
	{
		const Singular_Probe_Result probe = singular_probe(
			thread_pool, m_epsi, m_table->m_dtz[me], cache, me, m_epsi.num_positions(), max_workers);

		if (probe.singular != WDL_Entry::ILLEGAL)
		{
			if (probe.singular == WDL_Entry::DRAW)
			{
				m_info.draw_cnt[me]    = probe.legal_cnt;
				m_info.illegal_cnt[me] = probe.illegal_cnt;
			}
			else
			{
				gather_dtz_info(thread_pool, m_epsi, cache, me, m_epsi.num_positions(),
					max_workers, m_info, dtz_hist[me]);
			}
			std::printf("save_compress_wdl %d: singular\n", static_cast<int>(me));
			wdl_save[me] = Compressed_EGTB::make_singular(static_cast<uint8_t>(probe.singular));
		}
		else
		{
			const size_t num_positions = m_epsi.num_positions();
			const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
			const size_t num_blocks = ceil_div(total_packed_bytes, WDL_BLOCK_SIZE);

			Gather_Sink trial_sink{me, nullptr, nullptr, {}, {}};
			wdl_index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_wdl_block_source(
						m_epsi, *m_table, me, cache, num_positions, trial_sink,
						perm, /*gather=*/false, relax ? &*relax : nullptr);
				},
				WDL_BLOCK_SIZE,
				std::make_unique<LZ4_Compress_Helper>(nullptr),
				/*max_samples=*/1024,
				"choose_wdl_storage");

			Gather_Sink sink{me, &m_info, &dtz_hist[me], {}, std::vector<uint8_t>(num_blocks, 0)};
			Block_Source src = make_wdl_block_source(
				m_epsi, *m_table, me, cache, num_positions, sink,
				wdl_index_perm[me], /*gather=*/true,
				relax ? &*relax : nullptr);
			wdl_save[me] = save_compress_wdl(
				thread_pool, src, me, WDL_BLOCK_SIZE,
				paths.block_spill_path(m_epsi, me),
				max_workers);
		}

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board_unchecked().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	save_wdl_table(thread_pool, m_epsi, wdl_index_perm, wdl_save, paths.wdl_save_path(m_epsi), colors,
		EGTB_Magic::WDL_MAGIC, EGTB_GEN_RELAXED);

	for (Color me : colors) wdl_save[me] = {};

	Value_Rank_Table dtz_rank_1b[COLOR_NB];
	Value_Rank_Table dtz_rank_2b[COLOR_NB];
	size_t dtz_entry_bytes[COLOR_NB]{};
	for (Color me : colors)
	{
		dtz_rank_1b[me] = Value_Rank_Table::build(dtz_hist[me].hist_1b);
		dtz_rank_2b[me] = Value_Rank_Table::build(dtz_hist[me].hist_2b);
		dtz_entry_bytes[me] = (dtz_rank_1b[me].rank_to_value.size() <= 256) ? 1 : 2;
	}

	for (Color me : colors)
	{
		Value_Rank_Table& chosen = (dtz_entry_bytes[me] == 1) ? dtz_rank_1b[me] : dtz_rank_2b[me];
		if (chosen.rank_to_value.size() > 1)
		{
			dtz_index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_entry_block_source(
						m_table->m_dtz[me], cache, me,
						make_index_permutation_plan(m_epsi, perm),
						DTZ_BLOCK_SIZE, dtz_entry_bytes[me]);
				},
				DTZ_BLOCK_SIZE,
				std::make_unique<LZMA_Rank_Compress_Helper>(
					chosen, dtz_entry_bytes[me], EGTB_GEN_LOSS_ONLY
						? Rank_Storage_Mode::DTZ_LOSS_ONLY : Rank_Storage_Mode::DTZ),
				/*max_samples=*/64,
				"choose_dtz_storage");
		}
		Block_Source src = make_entry_block_source(
			m_table->m_dtz[me], cache, me,
			make_index_permutation_plan(m_epsi, dtz_index_perm[me]),
			DTZ_BLOCK_SIZE, dtz_entry_bytes[me]);
		dtz_save[me] = save_compress_egtb(
			thread_pool, src, me, dtz_entry_bytes[me], DTZ_BLOCK_SIZE,
			paths.block_spill_path(m_epsi, me),
			max_workers, chosen, EGTB_GEN_LOSS_ONLY
				? Rank_Storage_Mode::DTZ_LOSS_ONLY : Rank_Storage_Mode::DTZ);
		cache.purge(me);
		m_table->m_dtz[me].remove_disk_files(thread_pool);
		m_table->m_dtz[me].close();
	}

	if (m_is_symmetric)
	{
		cache.purge(BLACK);
		m_table->m_dtz[BLACK].remove_disk_files(thread_pool);
		m_table->m_dtz[BLACK].close();
	}

	save_egtb_table(thread_pool, m_epsi, dtz_index_perm, dtz_save, paths.dtz_save_path(m_epsi), colors,
		EGTB_Magic::DTZ_MAGIC, EGTB_GEN_LOSS_ONLY, /*relaxed=*/false);

	std::ofstream fp(paths.dtz_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
