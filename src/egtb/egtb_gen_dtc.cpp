#include "egtb/egtb_gen_dtc.h"
#include "egtb/egtb_compress.h"
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

DTC_Generator::DTC_Generator(
	const Piece_Config& ps,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTC_Table>(ps, tmp_dir))
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);

	const size_t bytes_per_color =
		m_table->m_dtc[WHITE][0].num_slices()
		* m_table->m_dtc[WHITE][0].within_slice_size()
		* sizeof(DTC_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB * DTC_BUDGET_LAYERS;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_iter_state(
		m_table->m_dtc[WHITE][0].num_groups(),
		m_table->m_dtc[WHITE][0].num_entries());
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
			case WDL_Entry::WIN:          w_opp = WDL_Entry::LOSE; break;
			case WDL_Entry::LOSE:         w_opp = WDL_Entry::WIN;  break;
			case WDL_Entry::DRAW:
			case WDL_Entry::CURSED_WIN:
			case WDL_Entry::BLESSED_LOSS: w_opp = WDL_Entry::DRAW; break;
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

WDL_Entry DTC_Generator::read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = parent.move_is_capture(move);
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);
	return read_post_push_wdl(pos_gen, move);
}

DTC_Final_Entry DTC_Generator::read_exit_dtc(const Exit_Site& site,
                                             size_t budget) const
{
	const auto* reader = m_exit_reader[site.giver][site.dropped];
	if (reader == nullptr) return DTC_Final_Entry::make_draw();
	return reader->read_at_budget(site.read_color, site.index, budget);
}

// The mover spends a budget; the opponent does not.
WDL_Entry DTC_Generator::read_post_push_wdl(Position_For_Gen& pos_gen, Move move) const
{
	const Color opp = color_opp(pos_gen.board_unchecked().turn());

	Exit_Site site;
	if (resolve_castling(pos_gen, move, out_param(site)))
	{
		if (m_layer > 0 && read_exit_dtc(site, m_layer - 1).wdl() == WDL_Entry::LOSE)
			return WDL_Entry::LOSE;
		if (read_exit_dtc(site, m_layer).wdl() == WDL_Entry::WIN)
			return WDL_Entry::WIN;
		return WDL_Entry::DRAW;
	}

	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	const size_t post_pid = m_epsi.pawn_slice_of(post_idx);

	if (m_layer > 0 && read_dtc_at<DTC_Final_Entry>(
			opp, m_layer - 1, post_pid, post_idx).is_loss())
		return WDL_Entry::LOSE;
	if (read_dtc_at<DTC_Final_Entry>(opp, m_layer, post_pid, post_idx).is_win())
		return WDL_Entry::WIN;
	return WDL_Entry::DRAW;
}

FORCE_INLINE DTC_Final_Entry DTC_Generator::make_initial_entry(
	Position_For_Gen& pos_gen, Out_Param<bool> layer_dependent) const
{
	auto intermediate = [](DTC_Intermediate_Entry entry) FORCE_INLINE_LAMBDA {
		return reinterpret_cast<const DTC_Final_Entry&>(entry);
	};

	*layer_dependent = false;

	enum Value : int {
		ValueNone = -32767,
		ValueLoss = -1,
		ValueDraw =  0,
		ValueWin  =  1,
	};

	Position& pos = pos_gen.board_unchecked();

	// Cursed outcomes fold into the draw class.
	auto fold = [](Value& best, WDL_Entry opp_wdl) {
		Value v;
		switch (opp_wdl)
		{
			case WDL_Entry::LOSE:         v = ValueWin;  break;
			case WDL_Entry::WIN:          v = ValueLoss; break;
			case WDL_Entry::DRAW:
			case WDL_Entry::CURSED_WIN:
			case WDL_Entry::BLESSED_LOSS: v = ValueDraw; break;
			default:                      return;
		}
		update_max(best, v);
	};

	bool any_legal = false;
	bool any_quiet_legal = false;
	bool any_zeroing = false;
	bool zeroing_win = false;
	constexpr uint16_t NO_EXIT_DTC = std::numeric_limits<uint16_t>::max();
	uint16_t exit_win_dtc = NO_EXIT_DTC;
	uint16_t exit_loss_worst = 0;
	Value best = ValueNone;
	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const bool is_cap   = pos.move_is_capture(m);
		const bool is_promo = m.is_promotion();
		const bool is_pawn  = piece_type(pos.piece_at(m.from())) == PAWN;

		if (is_cap || is_promo || is_pawn) {
			any_zeroing = true;
			// A push spends a budget and lands in this table; a capture or a
			// promotion reads a subtable instead.
			if (!is_cap && !is_promo) *layer_dependent = true;
			const WDL_Entry w = (is_cap || is_promo)
				? read_sub_tb(pos_gen, m)
				: (is_pawn_double_push(m) ? effective_opp_wdl_after_dp(pos_gen, m)
				                          : read_post_move_wdl(pos_gen, m));
			fold(best, w);
			if (w == WDL_Entry::LOSE) zeroing_win = true;
		} else if (Exit_Site site; resolve_castling(pos_gen, m, out_param(site))) {
			*layer_dependent = true;
			const DTC_Final_Entry ce = read_exit_dtc(site, m_layer);
			const uint16_t d = static_cast<uint16_t>(ce.value() + 1);
			if (d > DTC_Final_Entry::MAX_DTC)
			{
				fold(best, WDL_Entry::DRAW);
			}
			else
			{
				fold(best, ce.wdl());
				if (ce.is_loss())
					update_min(exit_win_dtc, d);
				else if (ce.is_win())
					update_max(exit_loss_worst, d);
			}
		} else {
			any_quiet_legal = true;
		}

		return zeroing_win;
	});
	if (!any_legal)
	{
		if (pos.is_in_check()) return DTC_Final_Entry::make_loss(0);
		return intermediate(DTC_Intermediate_Entry{});
	}

	if (best == ValueWin)
	{
		if (zeroing_win) return DTC_Final_Entry::make_win(1);
		ASSERT(exit_win_dtc != NO_EXIT_DTC);
		return DTC_Final_Entry::make_win(exit_win_dtc);
	}

	if (!any_quiet_legal)
	{
		if (best == ValueDraw) return intermediate(DTC_Intermediate_Entry{});
		if (best == ValueLoss)
		{
			uint16_t d = any_zeroing ? uint16_t{1} : uint16_t{0};
			update_max(d, exit_loss_worst);
			return DTC_Final_Entry::make_loss(d == 0 ? uint16_t{1} : d);
		}
	}

	if (best == ValueDraw) return intermediate(DTC_Intermediate_Entry::make_cap_draw());
	return intermediate(DTC_Intermediate_Entry::make_bound(exit_loss_worst));
}

uint16_t DTC_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const size_t spg = m_table->m_dtc[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtc[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	auto pin = [&](const std::vector<uint8_t>& groups) {
		build_init_need(groups, ngroups, 0, ntotal, spg);
		size_t charge = 0;
		for (const uint8_t v : m_scratch_need[WHITE]) charge += v;
		charge *= COLOR_NB;
		return try_pin_phase(
			thread_pool, &m_table->m_dtc[WHITE][0], &m_table->m_dtc[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK], charge);
	};
	const bool pinned = pin(m_all_groups) || pin(m_fusion_groups);

	std::vector<uint8_t> want(ngroups, 0);
	auto page_in_for_init_group = [&](size_t g) {
		if (pinned) return;
		want[g] = 1;
		const size_t g_start = g * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		build_init_need(want, ngroups, g_start, g_end, spg);
		want[g] = 0;
		apply_working_set(thread_pool, &m_table->m_dtc[WHITE][0], &m_table->m_dtc[BLACK][0],
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

	uint16_t max_classified = 0;
	const size_t PRINT_PERIOD = thread_pool->num_workers() * (1 << 20);
	Concurrent_Progress_Bar progress_bar(total_indices, PRINT_PERIOD, "init_entries");

	for (size_t g : m_pair_group_ids)
	{
		page_in_for_init_group(g);

		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> uint16_t {
			uint16_t local_max = 0;
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			size_t local_progress = 0;
			const auto& slice_has_stab = m_epsi.king_slice_manager().slice_has_stabilizer;
			auto classify = [&](Color us, Board_Index idx, size_t pid) FORCE_INLINE_LAMBDA {
				pos_gen.set_turn(us);
				if (m_layer == 0
					&& !pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
				{
					write_dtc(us, idx, DTC_Final_Entry::make_illegal());
					return;
				}
				bool layer_dependent;
				DTC_Final_Entry entry = make_initial_entry(pos_gen, out_param(layer_dependent));
				if (entry.is_illegal() || !entry.is_draw())
				{
					if (!layer_dependent) entry.set_flag(DTC_FLAG_INIT_MARK);
					commit_if_new(us, idx, pid, entry);
					write_dtc(us, idx, entry);
					update_max(local_max, static_cast<uint16_t>(entry.value()));
					mark_iter(us, idx, m_table->m_dtc[us][m_layer]);
				}
				else
				{
					auto& ie = reinterpret_cast<DTC_Intermediate_Entry&>(entry);
					if (!layer_dependent) ie.set_flag(DTC_FLAG_INIT_MARK);
					write_dtc(us, idx, ie);
					update_max(local_max, ie.bound());
				}
			};
			for (const Board_Index idx : group_it.indices())
			{
				if (++local_progress % PROGRESS_BAR_UPDATE_PERIOD == 0)
					progress_bar += PROGRESS_BAR_UPDATE_PERIOD;
				const size_t pid_of_idx = m_epsi.pawn_slice_of(idx);
				if (!pid_in_pair[pid_of_idx]) continue;

				pos_gen.seek(idx);

				if (m_layer == 0)
				{
					if (!pos_gen.is_legal())
					{
						write_dtc(WHITE, idx, DTC_Final_Entry::make_illegal());
						write_dtc(BLACK, idx, DTC_Final_Entry::make_illegal());
						continue;
					}
					else if (slice_has_stab[pos_gen.index().king_slice_id])
					{
						const Board_Index canon =
							board_index_of_position(m_epsi, pos_gen.board_unchecked());
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
						classify(us, idx, pid_of_idx);
				}
				else
				{
					for (Color us : { WHITE, BLACK })
					{
						const DTC_Final_Entry e = read_dtc_at<DTC_Final_Entry>(
							us, m_layer - 1, pid_of_idx, idx);
						if (e.is_illegal()) { write_dtc(us, idx, e); continue; }
						if (e.is_draw())
						{
							const auto ie =
								reinterpret_cast<const DTC_Intermediate_Entry&>(e);
							if (!ie.has_init_mark())
							{
								classify(us, idx, pid_of_idx);
								continue;
							}
							write_dtc(us, idx, ie);
							update_max(local_max, ie.bound());
							continue;
						}
						if (e.has_retro_seed())
						{
							write_dtc(us, idx,
								DTC_Intermediate_Entry::make_seed(
									e.has_seed_cap_draw()));
							continue;
						}
						if (!e.has_init_mark()) { classify(us, idx, pid_of_idx); continue; }
						write_dtc(us, idx, e);
						update_max(local_max, static_cast<uint16_t>(e.value()));
						mark_iter(us, idx, m_table->m_dtc[us][m_layer]);
					}
				}
			}
			return local_max;
		});
		for (const uint16_t v : rets) update_max(max_classified, v);
	}

	progress_bar.set_finished();
	return max_classified;
}

bool DTC_Generator::pin_layers(In_Out_Param<Thread_Pool> thread_pool,
                               const std::vector<uint8_t>& groups, size_t ngroups, size_t count,
                               const std::vector<size_t>& layers)
{
	for (Color c : { WHITE, BLACK })
	{
		m_scratch_need[c].assign(ngroups * DTC_BUDGET_LAYERS, 0);
		for (const size_t l : layers)
			std::copy(groups.begin(), groups.begin() + ngroups,
			          m_scratch_need[c].begin() + l * ngroups);
	}
	return try_pin_phase(
		thread_pool, &m_table->m_dtc[WHITE][0], &m_table->m_dtc[BLACK][0],
		m_scratch_need[WHITE], m_scratch_need[BLACK], COLOR_NB * count * layers.size());
}

namespace {

NODISCARD DTC_Final_Entry with_init_seed(DTC_Final_Entry now, DTC_Final_Entry old)
{
	bool cap_draw;
	if (old.is_draw())
	{
		const auto ie = reinterpret_cast<const DTC_Intermediate_Entry&>(old);
		if (!ie.has_init_mark()) return now;
		cap_draw = ie.has_cap_draw();
	}
	else if (old.has_retro_seed())
		cap_draw = old.has_seed_cap_draw();
	else
		return now;

	if (cap_draw) now.set_flag(DTC_FLAG_INIT_MARK);
	now.set_flag(DTC_FLAG_CAP_DRAW);
	return now;
}

NODISCARD bool dtc_cell_matches_dtz(DTC_Final_Entry a, DTZ_Final_Entry u)
{
	const uint16_t d = static_cast<uint16_t>(a.value());
	const uint16_t z = static_cast<uint16_t>(u.value());
	switch (u.wdl())
	{
		case WDL_Entry::WIN:  return a.is_win()  && d == z;
		case WDL_Entry::LOSE: return a.is_loss() && d == z;
		default:              return a.is_draw();
	}
}

}  // namespace

bool DTC_Generator::active_layer_matches_dtz(In_Out_Param<Thread_Pool> thread_pool,
                                             const DTZ_File_For_Probe& dtz, size_t layer)
{
	const size_t spg = m_table->m_dtc[WHITE][0].slices_per_group();
	const size_t ngroups = m_table->m_dtc[WHITE][0].num_groups();
	const std::vector<size_t> one{ layer };
	const auto colors = table_colors();
	const bool pinned = pin_layers(thread_pool, m_fusion_groups, ngroups,
	                               m_fusion_group_count, one);

	const auto& pid_in_pair = m_pid_in_pair;

	for (const size_t g : m_pair_group_ids)
	{
		if (!pinned)
		{
			for (Color c : { WHITE, BLACK })
			{
				m_scratch_need[c].assign(ngroups * DTC_BUDGET_LAYERS, 0);
				m_scratch_need[c][layer * ngroups + g] = 1;
			}
			apply_working_set(thread_pool, &m_table->m_dtc[WHITE][0],
			                  &m_table->m_dtc[BLACK][0],
			                  m_scratch_need[WHITE], m_scratch_need[BLACK]);
		}

		Shared_Board_Index_Iterator cell_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> bool {
			for (auto [chunk_start, chunk_end] : cell_it.chunks())
				for (Color c : colors)
				{
					for (const auto [idx, a] : make_in_pair_cells<DTC_Final_Entry>(
							m_epsi, pid_in_pair, m_table->m_dtc[c][layer],
							chunk_start, chunk_end))
					{
						if (a.is_illegal()) continue;
						if (!dtc_cell_matches_dtz(a, dtz.read(c, idx))) return false;
					}
				}
			return true;
		});
		for (const bool r : rets)
			if (!r) return false;
	}
	return true;
}

void DTC_Generator::mark_push_target_layers(uint8_t* need, size_t ngroups,
                                            size_t g_start, size_t g_end, size_t spg) const
{
	if (!m_epsi.pawn_slice_manager().has_pawns()) return;
	const size_t nks = m_epsi.num_king_slices();

	for (size_t i = 0; i < m_active_pawn_slices.size(); ++i)
	{
		const int32_t pid = m_active_pawn_slices[i];
		const auto& targets = m_targets_by_active[i];
		if (targets.empty()) continue;
		const size_t pid_base = static_cast<size_t>(pid) * nks;
		const size_t s_lo = std::max(g_start, pid_base);
		const size_t s_hi = std::min(g_end,   pid_base + nks);
		if (s_lo >= s_hi) continue;
		for (const int32_t tpid : targets)
		{
			const size_t t = static_cast<size_t>(tpid);
			const size_t base_k = m_table->layer_of(m_layer, t) * ngroups;
			const size_t base_below = (m_layer == 0)
				? base_k
				: m_table->layer_of(m_layer - 1, t) * ngroups;
			for (size_t s = s_lo; s < s_hi; ++s)
			{
				const size_t g = (t * nks + (s - pid_base)) / spg;
				need[base_k + g] = 1;
				need[base_below + g] = 1;
			}
		}
	}
}

void DTC_Generator::build_init_need(const std::vector<uint8_t>& write_groups, size_t ngroups,
                                    size_t g_start, size_t g_end, size_t spg)
{
	auto& need = m_scratch_need[WHITE];
	need.assign(ngroups * DTC_BUDGET_LAYERS, 0);

	const size_t below = (m_layer == 0 || m_active_pawn_slices.empty())
		? m_layer
		: m_table->layer_of(m_layer - 1, static_cast<size_t>(m_active_pawn_slices.front()));
	for (size_t g = 0; g < ngroups; ++g)
		if (write_groups[g])
		{
			need[m_layer * ngroups + g] = 1;
			need[below * ngroups + g] = 1;
		}
	mark_push_target_layers(need.data(), ngroups, g_start, g_end, spg);
	m_scratch_need[BLACK] = need;
}

void DTC_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_phase_pinned) return;
	const Color opp = color_opp(me);
	const size_t spg = m_table->m_dtc[WHITE][0].slices_per_group();
	const size_t ngroups = m_table->m_dtc[WHITE][0].num_groups();
	const size_t base = m_layer * ngroups;
	for (Color c : { WHITE, BLACK })
		m_scratch_need[c].assign(ngroups * DTC_BUDGET_LAYERS, 0);
	m_scratch_need[me][base + group_id] = 1;

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, m_epsi.num_slices());
	mark_king_neighbor_reach(m_scratch_need[opp].data() + base, g_start, g_end, spg);

	apply_working_set(thread_pool, &m_table->m_dtc[WHITE][0], &m_table->m_dtc[BLACK][0],
	                  m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

FORCE_INLINE DTC_Generator::Loss_Verification_Result DTC_Generator::check_loss(
	Position_For_Gen& pos_gen,
	uint16_t ply, DTC_Intermediate_Entry hint) const
{
	Loss_Verification_Result r;

	// A zeroing draw cannot be improved by a quiet move.
	if (hint.has_cap_draw())
		return r;

	const uint16_t exit_bound = hint.bound();

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

		const auto ce = read_dtc<DTC_Final_Entry>(opp, child);
		if (!ce.is_win())      return true;
		if (ce.value() >= ply) return true;
		update_max(max_contribution, static_cast<uint16_t>(ce.value() + 1));
		return false;
	});

	if (disqualified) return r;

	if (!any_legal) return r;

	update_max(max_contribution, exit_bound);

	if (any_zeroing)
		update_max<uint16_t>(max_contribution, 1);

	if (max_contribution != ply) return r;

	r.is_loss  = true;
	r.loss_dtz = max_contribution;
	return r;
}

FORCE_INLINE void DTC_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	const bool wins_may_improve = m_epsi.has_castling();
	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const auto e = read_dtc<DTC_Final_Entry>(opp, pred);
		if (e.is_illegal()) return;
		if (!e.is_draw())
		{
			if (!wins_may_improve || !e.is_win()) return;
			if (e.value() <= 1) return;
		}
		write_dtc(opp, pred, with_init_seed(DTC_Final_Entry::make_win(1), e));
	});
}

FORCE_INLINE void DTC_Generator::retro_mark_changed(Position_For_Gen& pos_gen)
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const DTC_Final_Entry fe = read_dtc<DTC_Final_Entry>(opp, pred);
		if (fe.is_illegal() || !fe.is_draw()) return;
		const auto& ie = reinterpret_cast<const DTC_Intermediate_Entry&>(fe);
		if (ie.has_change() || ie.has_cap_draw()) return;
		m_table->m_dtc[opp][m_layer].add_flags(pred, DTC_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtc[opp][m_layer]);
	});
}

FORCE_INLINE void DTC_Generator::retro_mark_wins(
	Position_For_Gen& pos_gen, uint16_t target_dtc)
{
	// Wins beyond the band ceiling belong to a higher budget.
	if (target_dtc > DTC_Final_Entry::MAX_DTC) return;

	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	const DTC_Final_Entry new_e = DTC_Final_Entry::make_win(target_dtc);
	const bool wins_may_improve = m_epsi.has_castling();
	pos.visit_pseudo_legal_pre_quiets([&](Move m) FORCE_INLINE_LAMBDA {
		const Board_Index pred = next_quiet_index(pos_gen, m);
		if (pred == BOARD_INDEX_NONE) return;
		const auto e = read_dtc<DTC_Final_Entry>(opp, pred);
		if (e.is_illegal()) return;
		if (!e.is_draw())
		{
			if (!wins_may_improve || !e.is_win()) return;
			if (e.value() <= target_dtc) return;
		}
		write_dtc(opp, pred, with_init_seed(new_e, e));
		mark_iter(opp, pred, m_table->m_dtc[opp][m_layer]);
	});
}

bool DTC_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                             Color stm, uint16_t ply)
{
	const size_t spg = m_table->m_dtc[stm][0].slices_per_group();
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

			Shared_Board_Index_Iterator mark_it = make_slice_group_iterator(g, spg);

			const auto mark_rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> bool {
				Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
				bool any = false;

				for (auto [chunk_start, chunk_end] : mark_it.chunks())
				{
					const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
					if (!m_iter_chunks[stm][cid]) continue;
					for (const auto [idx, fe] : make_in_pair_cells<DTC_Final_Entry>(
							m_epsi, pid_in_pair, m_table->m_dtc[stm][m_layer], chunk_start, chunk_end))
					{
						if (!fe.is_win()) continue;
						if (fe.value() != ply && fe.value() != ply - 1) continue;
						any = true;
						if (fe.value() != mark_changed_value) continue;
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

			auto reverify = [&](Board_Index i, DTC_Intermediate_Entry ie, DTC_Final_Entry fe) FORCE_INLINE_LAMBDA {
				pos_gen.seek(i);
				const auto res = check_loss(pos_gen, ply, ie);
				if (!res.is_loss)
				{
					if (ie.has_change())
					{
						ie.clear_flag(DTC_FLAG_CHANGE);
						write_dtc(stm, i, ie);
					}
					return;
				}
				write_dtc(stm, i, with_init_seed(
					DTC_Final_Entry::make_loss(res.loss_dtz), fe));
				retro_mark_wins(pos_gen, res.loss_dtz + 1);
				local.any = true;
			};

			for (auto [chunk_start, chunk_end] : cell_it.chunks())
			{
				const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
				if (!m_iter_chunks[stm][cid]) continue;
				Iter_Result chunk;
				for (const auto [idx, fe] : make_in_pair_cells<DTC_Final_Entry>(
						m_epsi, pid_in_pair, m_table->m_dtc[stm][m_layer], chunk_start, chunk_end))
				{
					if (fe.is_illegal()) continue;
					if (fe.is_draw())
					{
						const auto& ie = reinterpret_cast<const DTC_Intermediate_Entry&>(fe);
						const uint16_t bound = ie.bound();
						const bool bound_due = ply != 0 && bound == ply;
						if (ie.has_change() || bound >= ply)
							chunk.any_intermediate = true;
						if (!ie.has_change() && !bound_due) continue;
						reverify(idx, ie, fe);
					}
					else
					{
						update_max(chunk.max_classified, static_cast<uint16_t>(fe.value()));
						if (!fe.is_loss() || fe.value() != ply) continue;
						pos_gen.seek(idx);
						if (ply == 0) retro_mark_win_in_1(pos_gen);
						else          retro_mark_wins(pos_gen, fe.value() + 1);
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

void DTC_Generator::iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply)
{
	const size_t ngroups = m_table->m_dtc[WHITE][0].num_groups();
	const std::vector<size_t> this_layer{ m_layer };
	auto pin = [&](const std::vector<uint8_t>& groups, size_t count) {
		return pin_layers(thread_pool, groups, ngroups, count, this_layer);
	};
	m_phase_pinned = pin(m_all_groups, ngroups)
	              || pin(m_fusion_groups, m_fusion_group_count);

	// The band ceiling leaves unresolved cells drawn at this budget.
	if (finished_ply == 0)
	{
		(void)run_iter(thread_pool, WHITE, 0);
		(void)run_iter(thread_pool, BLACK, 0);
	}

	bool finished = false;
	while (!finished && finished_ply < DTC_Final_Entry::MAX_DTC)
	{
		++finished_ply;
		finished = true;
		std::printf("  iterate %4u\r", finished_ply); std::fflush(stdout);
		const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply);
		const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply);
		if (wrote_w || wrote_b || finished_ply <= m_max_dtc) finished = false;
		if (egtb_is_interrupt_requested(true))
			throw DTC_Interrupted{ finished_ply, m_layer_max_dtc };
	}
}

void DTC_Generator::claim_batch_slices(const std::vector<int32_t>& batch, size_t batch_row)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	for (const int32_t sid : batch)
		for (const int32_t pid : psm.pair_members(sid))
			m_table->m_slice_batch[static_cast<size_t>(pid)] =
				static_cast<uint8_t>(batch_row);
}

size_t DTC_Generator::build_batch(In_Out_Param<Thread_Pool> thread_pool,
                                  const DTZ_File_For_Probe& dtz,
                                  const std::vector<int32_t>& batch, size_t batch_idx,
								  size_t batches_total,
                                  size_t batch_row,
                                  const std::filesystem::path& ckpt_path,
                                  const Resume_Point& resume)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	const bool pawnful = psm.has_pawns();
	const bool resuming = static_cast<int64_t>(batch_idx) == resume.batch_idx;
	size_t life = 0;
	for (const int32_t sid : batch)
		life = std::max(life, static_cast<size_t>(psm.slice_life(sid)));
	const auto counts = m_epsi.piece_counts();
	const size_t num_pawns = counts[WHITE_PAWN] + counts[BLACK_PAWN]
	                       + (m_epsi.has_opposing_pair() ? 2 : 0);
	ASSERT(!pawnful || life >= num_pawns);
	const size_t max_layers =
		pawnful ? std::min(life - num_pawns + 1, DTC_BUDGET_LAYERS) : 1;

	auto& real = m_table->m_batch_real[batch_row];
	size_t last_real = 0;
	for (size_t k = (resuming ? static_cast<size_t>(resume.layer) : 0); k < max_layers; ++k)
	{
		const bool resuming_layer = resuming && static_cast<int64_t>(k) == resume.layer;
		m_layer = k;
		m_layer_committed = false;
		m_max_dtc = resuming_layer ? static_cast<uint16_t>(resume.max_dtc) : uint16_t{0};
		m_layer_max_dtc = m_max_dtc;

		set_active_fusion(psm, batch);
		const size_t init_layers = (k == 0) ? 1 : 2;
		const auto fusions = compute_fusion_groups(
			m_table->m_dtc[WHITE][0], batch, init_layers, init_layers);

		if (pawnful)
		{
			std::printf("  batch %zu/%zu layer %zu (%zu pairs in %zu fusion%s)\n",
				batch_idx + 1, batches_total, k, batch.size(),
				fusions.size(), fusions.size() == 1 ? "" : "s");
			std::fflush(stdout);
		}

		std::vector<uint16_t> fusion_max_dtc(fusions.size(), m_max_dtc);
		if (!resuming_layer)
		{
			for (size_t fi = 0; fi < fusions.size(); ++fi)
			{
				set_active_fusion(psm, fusions[fi]);
				refresh_active_metadata(m_table->m_dtc[WHITE][0]);
				fusion_max_dtc[fi] = init_entries(thread_pool);
				update_max(m_layer_max_dtc, fusion_max_dtc[fi]);
			}
			if (k > 0 && !m_layer_committed)
			{
				bool saturated = true;
				for (const auto& fusion : fusions)
				{
					set_active_fusion(psm, fusion);
					refresh_active_metadata(m_table->m_dtc[WHITE][0]);
					if (!active_layer_matches_dtz(thread_pool, dtz, last_real))
					{
						saturated = false;
						break;
					}
				}
				if (saturated)
				{
					for (size_t l = k; l < max_layers; ++l)
						real[l] = static_cast<uint8_t>(last_real);
					break;
				}
			}
		}

		for (size_t fi = 0; fi < fusions.size(); ++fi)
		{
			const bool is_resume_fusion =
				resuming_layer && static_cast<int64_t>(fi) == resume.fusion_idx;
			if (resuming_layer && static_cast<int64_t>(fi) < resume.fusion_idx) continue;

			set_active_fusion(psm, fusions[fi]);
			refresh_active_metadata(m_table->m_dtc[WHITE][0]);
			m_max_dtc = fusion_max_dtc[fi];
			if (fusions.size() > 1 || resuming_layer) seed_iter_groups();

			try
			{
				iterate(thread_pool, is_resume_fusion ? resume.finished_ply : uint16_t{0});
			}
			catch (const DTC_Interrupted& e)
			{
				for (size_t l = 0; l < DTC_BUDGET_LAYERS; ++l)
					for (Color c : { WHITE, BLACK })
						m_table->m_dtc[c][l].evict_all(thread_pool);
				Checkpoint_File ckpt{};
				ckpt.batch_idx = static_cast<uint32_t>(batch_idx);
				ckpt.layer = static_cast<uint16_t>(k);
				ckpt.fusion_idx = static_cast<uint32_t>(fi);
				ckpt.finished_ply = e.finished_ply;
				ckpt.max_dtc = e.max_dtc;
				for (size_t r = 0; r < m_table->m_batch_real.size(); ++r)
					std::copy(m_table->m_batch_real[r].begin(), m_table->m_batch_real[r].end(),
					          std::begin(ckpt.real[r]));
				write_checkpoint(ckpt_path, ckpt);
				throw;
			}
		}
		real[k] = static_cast<uint8_t>(k);
		last_real = k;
	}
	for (size_t k = max_layers; k < DTC_BUDGET_LAYERS; ++k)
		real[k] = static_cast<uint8_t>(last_real);
	return last_real + 1;
}

void DTC_Generator::gen(
	Table_Reader_Map<WDL_Entry> sub_wdl,
	Table_Reader_Map<DTC_Final_Entry> exit_dtc,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_wdl_by_material = std::move(sub_wdl);
	m_exit_dtc_by_material = std::move(exit_dtc);
	bind_exit_readers(m_exit_dtc_by_material, m_exit_reader);

	const DTZ_File_For_Probe dtz(paths, m_epsi, thread_pool);

	const auto ckpt_path = paths.dtc_checkpoint_path(m_epsi);
	Resume_Point resume;
	m_table->m_batch_real.assign(batches.size() + 1, {});
	{
		Checkpoint_File ckpt{};
		if (read_checkpoint(ckpt_path, &ckpt))
		{
			resume.batch_idx = static_cast<int64_t>(ckpt.batch_idx);
			resume.layer = static_cast<int64_t>(ckpt.layer);
			resume.fusion_idx = static_cast<int64_t>(ckpt.fusion_idx);
			resume.finished_ply = ckpt.finished_ply;
			resume.max_dtc = ckpt.max_dtc;
			for (size_t r = 0; r < m_table->m_batch_real.size(); ++r)
				std::copy(std::begin(ckpt.real[r]), std::end(ckpt.real[r]),
				          m_table->m_batch_real[r].begin());
		}
		else
		{
			for (size_t l = 0; l < DTC_BUDGET_LAYERS; ++l)
				for (Color c : { WHITE, BLACK })
					m_table->m_dtc[c][l].remove_disk_files(thread_pool);
		}
	}
	remove_checkpoint(ckpt_path);

	m_num_layers = 1;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		const size_t batch_row = bi + 1;
		claim_batch_slices(batches[bi], batch_row);
		if (static_cast<int64_t>(bi) < resume.batch_idx)
		{
			m_num_layers = std::max<size_t>(
				m_num_layers, m_table->m_batch_real[batch_row][DTC_BUDGET_LAYERS - 1] + 1);
			continue;
		}
		m_num_layers = std::max(m_num_layers,
			build_batch(thread_pool, dtz, batches[bi], bi, batches.size(), batch_row, ckpt_path, resume));
	}

	for (size_t l = m_num_layers; l < DTC_BUDGET_LAYERS; ++l)
		for (Color c : { WHITE, BLACK })
		{
			m_table->m_dtc[c][l].remove_disk_files(thread_pool);
			m_table->m_dtc[c][l].close();
		}
	m_layer = m_num_layers - 1;

	m_sub_wdl_by_material.clear();
	m_exit_dtc_by_material.clear();
	thread_pool->respawn_all_threads();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu layers, %zu pawn-slice pairs in %zu batches)\n",
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		m_num_layers, total_pairs, batches.size());
}

namespace {

using DTC_Save_Cache = Save_Group_Cache<DTC_Final_Entry, DTC_Intermediate_Entry>;
using DTC_Pinned_Range = Pinned_Group_Range<DTC_Final_Entry, DTC_Intermediate_Entry>;

size_t dtc_table_idx_of(Color c, size_t layer)
{
	ASSERT(layer < DTC_BUDGET_LAYERS);
	return static_cast<size_t>(c) * DTC_BUDGET_LAYERS + layer;
}

// Row 0 preserves DTZ as the clean curve's terminal changepoint; the 29
// separately solved predecessors use unhalved non-cursed values.
NODISCARD uint16_t dtz_pack_value(DTZ_Final_Entry e)
{
	if (e.is_illegal() || e.is_draw()) return DTZ_Final_Entry::ILLEGAL_VAL;
	return static_cast<uint16_t>(e.value());
}

NODISCARD uint16_t dtc_layer_value(DTC_Final_Entry e)
{
	if (e.is_illegal() || e.is_draw()) return DTC_Final_Entry::ILLEGAL_VAL;
	ASSERT(static_cast<uint16_t>(e.value()) <= DTC_Final_Entry::MAX_DTC);
	return static_cast<uint16_t>(e.value());
}

size_t dtc_pack_index_of_budget(size_t budget)
{
	ASSERT(budget < DTC_PACK_LAYERS);
	return DTC_PACK_LAYERS - 1 - budget;
}

NODISCARD bool fill_dtc_chunk(
	DTC_Table& table, const DTZ_File_For_Probe& dtz, DTC_Save_Cache& cache, Color color,
	size_t num_layers, const Index_Permutation_Plan& perm_plan,
	size_t p_base, size_t this_bp, const uint32_t* row_layer_mask,
	uint16_t* chunk, size_t* logical_pos, uint8_t* cell_row, uint8_t* seen)
{
	const size_t top = num_layers - 1;
	auto& base = table.m_dtc[color][top];
	const size_t want_lo = base.group_id_of_index(p_base);
	const size_t want_hi = base.group_id_of_index(p_base + this_bp - 1);
	for (size_t k = 0; k < this_bp; ++k)
		logical_pos[k] = storage_index_to_logical_index(perm_plan, p_base + k);

	uint32_t layer_used = 0;
	for (size_t k = 0; k < this_bp; ++k)
	{
		const uint8_t r = table.m_slice_batch[
			table.m_epsi.pawn_slice_of(static_cast<Board_Index>(logical_pos[k]))];
		cell_row[k] = r;
		layer_used |= row_layer_mask[r];
	}

	std::fill(seen, seen + this_bp, uint8_t{0});
	bool any_live = false;

	for (size_t layer = num_layers; layer-- > 0; )
	{
		if (!(layer_used & (uint32_t{1} << layer))) continue;
		DTC_Pinned_Range pin(cache, dtc_table_idx_of(color, layer), want_lo, want_hi);
		for (size_t k = 0; k < this_bp; ++k)
		{
			const auto& real = table.m_batch_real[cell_row[k]];
			if (real[layer] != layer) continue;
			const bool at_cell_top = !seen[k];
			seen[k] = 1;
			if (!at_cell_top && chunk[k] == DTC_Final_Entry::ILLEGAL_VAL) continue;
			const auto e = table.read<DTC_Final_Entry>(
				color, layer, static_cast<Board_Index>(logical_pos[k]));
			uint16_t v = dtc_layer_value(e);
			if (at_cell_top)
			{
				if (e.is_illegal())
				{
					chunk[k] = DTC_Final_Entry::ILLEGAL_VAL;
					v = DTC_Final_Entry::ILLEGAL_VAL;
				}
				else
				{
					chunk[k] = dtz_pack_value(
						dtz.read(color, static_cast<Board_Index>(logical_pos[k])));
					if (chunk[k] != DTC_Final_Entry::ILLEGAL_VAL) any_live = true;
				}
			}
			size_t b_hi = layer;
			while (b_hi + 1 < DTC_BUDGET_LAYERS && real[b_hi + 1] == layer) ++b_hi;
			const size_t j_hi = dtc_pack_index_of_budget(layer);
			const size_t j_lo = (b_hi + 1 == DTC_BUDGET_LAYERS)
				? 1 : dtc_pack_index_of_budget(b_hi);
			for (size_t j = j_lo; j <= j_hi; ++j)
				chunk[j * this_bp + k] = v;
		}
	}
	return any_live;
}

}  // namespace

NODISCARD static bool gather_dtc_info(
	const Piece_Config_For_Gen& epsi,
	DTC_Table& table,
	const DTZ_File_For_Probe& dtz,
	DTC_Save_Cache& cache,
	Color color,
	size_t num_layers,
	size_t num_positions,
	size_t positions_per_group,
	In_Out_Param<Thread_Pool> thread_pool,
	size_t max_workers,
	EGTB_Info& info,
	Layered_Rank_Table& ranks)
{
	const size_t top = num_layers - 1;
	const size_t ng = table.m_dtc[color][top].num_groups();

	std::vector<std::array<uint8_t, Layered_Rank_Table::LUT_SIZE>> per_layer_seen(num_layers + 1);

	struct Seen
	{
		std::array<uint8_t, Layered_Rank_Table::LUT_SIZE> budget{};
		std::array<uint8_t, Layered_Rank_Table::LUT_SIZE> unbounded{};
	};
	const size_t within = table.m_dtc[color][0].within_slice_size();
	const auto slice_top = [&table, &epsi, color, top, within](size_t s) {
		const size_t pid = epsi.pawn_slice_of(static_cast<Board_Index>(s * within));
		return dtc_table_idx_of(color, table.layer_of(top, pid));
	};

	const auto bins = gather_egtb_info_parallel<Seen>(
		thread_pool, epsi, cache, color, num_positions, max_workers, info,
		[&dtz, color](Seen& s, size_t idx, const DTC_Final_Entry& e) {
			if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
				s.budget[dtc_layer_value(e)] = 1;
			if (e.is_illegal()) return;
			const DTZ_Final_Entry u = dtz.read(color, static_cast<Board_Index>(idx));
			if (!(EGTB_GEN_LOSS_ONLY && u.is_win()))
			{
				const uint16_t v = dtz_pack_value(u);
				if (v != DTZ_Final_Entry::ILLEGAL_VAL) s.unbounded[v] = 1;
			}
		},
		slice_top);

	for (const Seen& s : bins)
		for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
		{
			if (s.budget[v]) per_layer_seen[0][v] = 1;
			if (s.unbounded[v]) per_layer_seen[num_layers][v] = 1;
		}

	if (num_layers > 1)
	{
		const size_t n = num_layers - 1;
		const size_t capped_n = (max_workers == 0) ? n : std::min(n, max_workers);
		const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), capped_n));
		std::atomic<size_t> next(1);
		thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
			for (;;)
			{
				const size_t j = next.fetch_add(1, std::memory_order_relaxed);
				if (j >= num_layers) return;
				const size_t layer = top - j;
				auto& seen = per_layer_seen[j];
				const size_t ti = dtc_table_idx_of(color, layer);
				for (size_t g = 0; g < ng; ++g)
				{
					const size_t p_lo = g * positions_per_group;
					const size_t p_hi = std::min(p_lo + positions_per_group, num_positions);
					bool any_owner = false;
					for (size_t pid = epsi.pawn_slice_of(static_cast<Board_Index>(p_lo)),
					            last = epsi.pawn_slice_of(static_cast<Board_Index>(p_hi - 1));
					     pid <= last; ++pid)
						if (table.layer_of(layer, pid) == layer
						    && layer != table.layer_of(DTC_BUDGET_LAYERS - 1, pid))
						{
							any_owner = true;
							break;
						}
					if (!any_owner) continue;
					DTC_Pinned_Range pin(cache, ti, g, g);
					for (size_t p = p_lo; p < p_hi; ++p)
					{
						const size_t pid = epsi.pawn_slice_of(static_cast<Board_Index>(p));
						if (table.layer_of(layer, pid) != layer) continue;
						if (layer == table.layer_of(DTC_BUDGET_LAYERS - 1, pid)) continue;
						const auto e = table.read<DTC_Final_Entry>(color, layer, static_cast<Board_Index>(p));
						if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
							seen[dtc_layer_value(e)] = 1;
					}
				}
			}
		});
	}

	std::array<uint64_t, Layered_Rank_Table::LUT_SIZE> score{};
	for (size_t j = 0; j < per_layer_seen.size(); ++j)
		for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
			if (per_layer_seen[j][v]) ++score[v];

	std::vector<uint16_t> values;
	values.reserve(128);
	for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
		if (score[v] != 0) values.push_back(static_cast<uint16_t>(v));
	if (values.empty()) return false;

	std::sort(values.begin(), values.end(),
		[&](uint16_t a, uint16_t b) {
			if (score[a] != score[b]) return score[a] > score[b];
			return a < b;
		});

	ranks.rank_to_value = std::move(values);
	ranks.entry_bytes = (ranks.rank_to_value.size() <= 256) ? 1 : 2;
	for (size_t i = 0; i < ranks.rank_to_value.size(); ++i)
		ranks.value_to_rank[ranks.rank_to_value[i]] = static_cast<uint16_t>(i);
	return true;
}

static void save_compress_dtc(
	In_Out_Param<Thread_Pool> thread_pool,
	DTC_Table& table,
	const DTZ_File_For_Probe& dtz,
	DTC_Save_Cache& cache,
	Color color,
	size_t num_layers,
	size_t num_positions,
	uint32_t block_size,
	uint32_t index_perm,
	size_t max_workers,
	std::filesystem::path spill_path,
	Layered_Compressed_Color& out)
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
		std::string("save_compress_dtc ") + std::to_string(static_cast<int>(color)));

	out.block_positions = narrowing_static_cast<uint32_t>(bp);
	out.block_cnt = bcnt;
	out.tail_positions = (tail == bp) ? 0u : narrowing_static_cast<uint32_t>(tail);
	out.compressed_blocks = Compressed_Block_Store(std::move(spill_path), bcnt, static_cast<size_t>(block_size));
	out.usizes.resize(bcnt);

	const auto perm_plan = make_index_permutation_plan(table.m_epsi, index_perm);

	static_assert(DTC_BUDGET_LAYERS <= 32, "layer mask is 32 bits");
	ASSERT(table.m_batch_real.size() <= 256);
	std::array<uint32_t, 256> row_layer_mask{};
	for (size_t r = 0; r < table.m_batch_real.size(); ++r)
		for (size_t l = 0; l < num_layers; ++l)
			if (table.m_batch_real[r][l] == l) row_layer_mask[r] |= uint32_t{1} << l;

	std::atomic<size_t> next_block_id(0);

	thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
		if (thread_id >= effective_workers) return;

		std::vector<uint16_t> chunk(bp * DTC_PACK_LAYERS);
		std::vector<size_t> logical_pos(bp);
		std::vector<uint8_t> cell_row(bp);
		std::vector<uint8_t> seen(bp);
		DTC_Block_Encoder encoder;
		encoder.ranks = &out.ranks;
		encoder.layers = DTC_PACK_LAYERS;
		LZMA_Compress_Helper lzma;

		for (;;)
		{
			const size_t b = next_block_id.fetch_add(1, std::memory_order_relaxed);
			if (b >= bcnt) return;

			const size_t p_base = b * bp;
			const size_t this_bp =
				(b == bcnt - 1 && out.tail_positions != 0) ? out.tail_positions : bp;

			if (!fill_dtc_chunk(table, dtz, cache, color, num_layers, perm_plan,
			                    p_base, this_bp, row_layer_mask.data(),
			                    chunk.data(), logical_pos.data(),
			                    cell_row.data(), seen.data()))
			{
				out.usizes[b] = 0;
				out.compressed_blocks.clear(b);
				progress_bar += 1;
				continue;
			}

			const Const_Span<uint8_t> payload = encoder.encode(chunk.data(), this_bp);
			std::vector<uint8_t> compressed = lzma.compress(payload);
			if (compressed.size() > 0xFFFFFFFFu)
				print_and_abort("Block too large for offset encoding\n");

			out.usizes[b] = payload.size();
			out.compressed_blocks.set(b, Const_Span<uint8_t>(compressed));
			progress_bar += 1;
		}
	});

	out.total_compressed_size = out.compressed_blocks.total_size();
	progress_bar.set_finished();
}

void DTC_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_save_start = std::chrono::steady_clock::now();
	const DTZ_File_For_Probe dtz(paths, m_epsi, thread_pool);
	const auto colors = table_colors();
	const size_t num_positions = m_epsi.num_positions();
	if (num_positions == 0) return;

	m_epsi.prepare_orbit_weight_table();
	m_info.clear();

	const size_t spg = m_table->m_dtc[WHITE][0].slices_per_group();
	const size_t wss = m_table->m_dtc[WHITE][0].within_slice_size();
	const size_t positions_per_group = spg * wss;

	const size_t group_bytes = positions_per_group * sizeof(DTC_Final_Entry);
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

	std::vector<Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*> all_tables;
	all_tables.reserve(COLOR_NB * DTC_BUDGET_LAYERS);
	for (Color c : { WHITE, BLACK })
		for (size_t l = 0; l < DTC_BUDGET_LAYERS; ++l)
			all_tables.push_back(&m_table->m_dtc[c][l]);
	DTC_Save_Cache cache(std::move(all_tables), cap_groups);

	Layered_Compressed_Color color_out[COLOR_NB]{};
	uint32_t index_perm[COLOR_NB] = { 0, 0 };

	for (Color me : colors)
	{
		const bool any = gather_dtc_info(
			m_epsi, *m_table, dtz, cache, me, m_num_layers, num_positions,
			positions_per_group, thread_pool, max_workers, m_info, color_out[me].ranks);

		if (!any)
		{
			std::printf("save dtc %d: singular\n", static_cast<int>(me));
			color_out[me].is_singular = true;
		}
		else
		{
			// Row 0 fixes the pack to DTZ's permutation.
			index_perm[me] = dtz.index_perm(me);

			save_compress_dtc(
				thread_pool, *m_table, dtz, cache, me, m_num_layers, num_positions,
				DTC_BLOCK_SIZE, index_perm[me], max_workers,
				paths.block_spill_path(m_epsi, me), color_out[me]);
		}

		for (size_t l = 0; l < DTC_BUDGET_LAYERS; ++l)
		{
			cache.purge(dtc_table_idx_of(me, l));
			m_table->m_dtc[me][l].remove_disk_files(thread_pool);
			m_table->m_dtc[me][l].close();
		}
	}

	if (m_is_symmetric)
		for (size_t l = 0; l < DTC_BUDGET_LAYERS; ++l)
		{
			cache.purge(dtc_table_idx_of(BLACK, l));
			m_table->m_dtc[BLACK][l].remove_disk_files(thread_pool);
			m_table->m_dtc[BLACK][l].close();
		}

	save_layered_table(thread_pool, m_epsi, index_perm, color_out, paths.dtc_save_path(m_epsi), colors,
		EGTB_GEN_LOSS_ONLY, EGTB_Magic::DTC_MAGIC);

	for (Color me : colors)
	{
		if (m_info.longest_win[me] == 0) continue;
		Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
		pos_gen.board_unchecked().to_fen(Span(m_info.longest_fen[me]));
	}
	std::ofstream fp(paths.dtc_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
