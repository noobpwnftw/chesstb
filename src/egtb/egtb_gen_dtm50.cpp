#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_compress.h"
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
	return it->second->read_at_hmc(sub_color, sub_idx, 0);
}

DTM50_Final_Entry DTM50_Generator::read_exit_dtm(const Exit_Site& site,
                                                 uint16_t target_hmc) const
{
	const auto* reader = m_exit_reader[site.giver][site.dropped];
	if (reader == nullptr) return DTM50_Final_Entry::make_draw();
	return reader->read_at_hmc(site.read_color, site.index, target_hmc);
}

DTM50_Final_Entry DTM50_Generator::read_post_move_dtm(Position_For_Gen& pos_gen, Move move, uint16_t hmc) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = parent.move_is_capture(move);
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move);

	const Color mover = parent.turn();
	const Color opp = color_opp(mover);

	// Pawn pushes reset HMC; other quiet moves increment it. A pawn move drops
	// no rights, so it is never an exit and can index straight away.
	const bool is_pawn_push = piece_type(parent.piece_at(move.from())) == PAWN;
	if (is_pawn_push)
		return read_dtm50<DTM50_Final_Entry>(opp, 0, next_quiet_index(pos_gen, move));

	const uint16_t target_hmc = static_cast<uint16_t>(hmc + 1);
	if (target_hmc >= DTM50_HMC_COUNT)
	{
		Position child = parent;
		(void)child.do_move(move);
		return child.is_checkmate() ? DTM50_Final_Entry::make_loss(0)
		                            : DTM50_Final_Entry::make_draw();
	}
	Exit_Site site;
	if (resolve_castling(pos_gen, move, out_param(site)))
		return read_exit_dtm(site, target_hmc);

	return read_dtm50<DTM50_Final_Entry>(opp, target_hmc, next_quiet_index(pos_gen, move));
}

namespace {

bool dtm_better_for_mover(DTM50_Final_Entry a, DTM50_Final_Entry b)
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

	DTM50_Entry_Builder& operator+=(DTM50_Final_Entry child_e)
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

	DTM50_Entry_Builder& operator+=(const DTM50_Entry_Builder& o)
	{
		if (o.saw_win) { saw_win = true; update_min(best_win_dtm, o.best_win_dtm); }
		if (o.saw_draw) saw_draw = true;
		update_max(best_loss_dtm, o.best_loss_dtm);
		return *this;
	}

	DTM50_Entry_Builder& operator+=(DTM50_Intermediate_Entry c)
	{
		if (c.is_win())       { saw_win = true; update_min(best_win_dtm, static_cast<uint16_t>(c.value())); }
		else if (c.is_loss())   update_max(best_loss_dtm, static_cast<uint16_t>(c.value()));
		else                    saw_draw = true;
		return *this;
	}

	template <class EntryT>
	operator EntryT() const
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

	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const uint8_t rights_before = p.castling();
	const Square ep_sq = ep_square_of_double_push(dp_move);
	const Piece captured_by_dp = p.do_move(dp_move);

	DTM50_Final_Entry best_ep_for_opp = DTM50_Final_Entry::make_loss(0);  // worst for opp; bumped on first EP
	bool any_ep = false;

	std::optional<Position_For_Gen> p_gen_for_ep;

	(void)p.visit_legal_ep_captures(ep_sq, [&](Move ep_move) FORCE_INLINE_LAMBDA {
		if (!p_gen_for_ep)
		{
			const Board_Index child_idx = board_index_of_position(m_epsi, p);
			p_gen_for_ep.emplace(m_epsi, child_idx, opp);
		}
		// Convert post-EP DTM to the opponent's pre-EP view.
		const DTM50_Final_Entry after_ep = read_sub_tb(*p_gen_for_ep, ep_move);
		DTM50_Final_Entry opp_at_pre_ep;
		if (after_ep.is_illegal())          return false;
		else if (after_ep.is_win())         opp_at_pre_ep = DTM50_Final_Entry::make_loss(after_ep.value() + 1);
		else if (after_ep.is_loss())        opp_at_pre_ep = DTM50_Final_Entry::make_win(after_ep.value() + 1);
		else                                opp_at_pre_ep = DTM50_Final_Entry::make_draw();

		if (!any_ep || dtm_better_for_mover(opp_at_pre_ep, best_ep_for_opp))
			best_ep_for_opp = opp_at_pre_ep;
		any_ep = true;
		return false;
	});

	p.undo_move(dp_move, captured_by_dp, rights_before);

	if (!any_ep) return no_ep;
	return dtm_better_for_mover(best_ep_for_opp, no_ep) ? best_ep_for_opp : no_ep;
}

FORCE_INLINE DTM50_Final_Entry DTM50_Generator::make_initial_entry(
	Position_For_Gen& pos_gen, uint16_t hmc)
{
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM50_Final_Entry::make_illegal();

	Position& pos = pos_gen.board_unchecked();

	DTM50_Entry_Builder inv;
	DTM50_Entry_Builder dep;

	bool any_legal = false;
	bool any_invariant = false;
	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;
		const bool invariant = is_pawn || pos.move_is_capture(m);
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
		return false;
	});

	if (!any_legal)
		return pos.is_in_check()
			? DTM50_Final_Entry::make_loss(0)
			: DTM50_Final_Entry::make_draw();

	if (any_invariant)
	{
		write_dtm50<DTM50_Intermediate_Entry>(pos.turn(), 0, pos_gen.board_index(), inv);
		dep += inv;
	}
	return dep;
}

FORCE_INLINE DTM50_Final_Entry DTM50_Generator::make_layer_entry(Position_For_Gen& pos_gen, DTM50_Intermediate_Entry inv, uint16_t hmc) const
{
	Position& pos = pos_gen.board_unchecked();

	DTM50_Entry_Builder dep;

	bool any_legal = false;
	bool any_invariant = false;
	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;
		const bool invariant = is_pawn || pos.move_is_capture(m);
		if (invariant)
		{
			any_invariant = true;
			return false;
		}
		dep += read_post_move_dtm(pos_gen, m, hmc);
		return false;
	});

	if (!any_legal)
		return pos.is_in_check()
			? DTM50_Final_Entry::make_loss(0)
			: DTM50_Final_Entry::make_draw();

	if (any_invariant) dep += inv;
	return dep;
}

void DTM50_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	constexpr uint16_t hmc = DTM50_HMC_COUNT - 1;
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	auto pin = [&](const std::vector<uint8_t>& g0, const std::vector<uint8_t>& gh,
	               size_t count) {
		m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
		std::copy(g0.begin(), g0.end(), m_scratch_need[WHITE].begin());
		std::copy(gh.begin(), gh.end(),
		          m_scratch_need[WHITE].begin() + static_cast<size_t>(hmc) * ngroups);
		m_scratch_need[BLACK] = m_scratch_need[WHITE];
		return try_pin_phase(
			thread_pool, &m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK], COLOR_NB * count);
	};
	const bool pinned = pin(m_all_groups, m_all_groups, 2 * ngroups)
	                 || pin(m_fusion_init_groups, m_fusion_groups,
	                        m_fusion_init_group_count + m_fusion_group_count);

	auto page_in_for_init_group = [&](size_t g) {
		if (pinned) return;
		m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
		m_scratch_need[BLACK].assign(ngroups * DTM50_HMC_COUNT, 0);
		auto* need_w0 = m_scratch_need[WHITE].data();
		auto* need_wh = m_scratch_need[WHITE].data() + static_cast<size_t>(hmc) * ngroups;
		auto* need_b0 = m_scratch_need[BLACK].data();
		auto* need_bh = m_scratch_need[BLACK].data() + static_cast<size_t>(hmc) * ngroups;
		need_wh[g] = 1;
		need_bh[g] = 1;
		need_w0[g] = 1;
		const size_t g_start = g * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		mark_push_target_reach(need_w0, g_start, g_end, spg);
		std::copy_n(need_w0, ngroups, need_b0);
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
	if (m_phase_pinned) return;
	constexpr Color opp = (me == WHITE) ? BLACK : WHITE;
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();

	m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
	m_scratch_need[BLACK].assign(ngroups * DTM50_HMC_COUNT, 0);
	auto* need_me0  = m_scratch_need[me].data();
	auto* need_me   = m_scratch_need[me].data() + static_cast<size_t>(hmc) * ngroups;
	auto* need_opp  = m_scratch_need[opp].data() + static_cast<size_t>(hmc + 1) * ngroups;

	need_me0[group_id] = 1;
	need_me[group_id]  = 1;
	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, m_epsi.num_slices());
	mark_king_neighbor_reach(need_opp, g_start, g_end, spg);

	apply_working_set(thread_pool,
		&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
		m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

template <Color stm>
void DTM50_Generator::build_layer(In_Out_Param<Thread_Pool> thread_pool, uint16_t hmc)
{
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	constexpr Color pin_opp = (stm == WHITE) ? BLACK : WHITE;
	auto pin = [&](const std::vector<uint8_t>& groups, size_t count) {
		m_scratch_need[WHITE].assign(ngroups * DTM50_HMC_COUNT, 0);
		m_scratch_need[BLACK].assign(ngroups * DTM50_HMC_COUNT, 0);
		std::copy(groups.begin(), groups.end(), m_scratch_need[stm].begin());
		if (hmc > 0)
			std::copy(groups.begin(), groups.end(),
			          m_scratch_need[stm].begin() + static_cast<size_t>(hmc) * ngroups);
		std::copy(groups.begin(), groups.end(),
		          m_scratch_need[pin_opp].begin() + static_cast<size_t>(hmc + 1) * ngroups);
		return try_pin_phase(
			thread_pool, &m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK], (hmc == 0 ? 2 : 3) * count);
	};
	m_phase_pinned = pin(m_all_groups, ngroups)
	              || pin(m_fusion_groups, m_fusion_group_count);

	for (size_t g : m_pair_group_ids)
	{
		if (m_iter_groups[stm][g] == 0) continue;

		page_in_for_group<stm>(thread_pool, g, hmc);

		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t) -> bool {
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
			bool any_legal_local = false;
			for (auto [chunk_start, chunk_end] : group_it.chunks())
			{
				const size_t cid = static_cast<size_t>(chunk_start) / CHUNK_SIZE;
				if (!m_iter_chunks[stm][cid]) continue;
				bool any_legal = false;
				// hmc=0 carries the invariant legality marker.
				for (const auto [idx, fe] : make_in_pair_cells<DTM50_Final_Entry>(
						m_epsi, pid_in_pair, m_table->m_dtm[stm][0], chunk_start, chunk_end))
				{
					if (fe.is_illegal()) continue;
					const auto& ie = reinterpret_cast<const DTM50_Intermediate_Entry&>(fe);
					pos_gen.seek(idx);

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
	Table_Reader_Map<DTM50_Final_Entry> sub_dtm,
	Table_Reader_Map<DTM50_Final_Entry> exit_dtm,
	In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

	m_sub_dtm_by_material = std::move(sub_dtm);
	m_exit_dtm_by_material = std::move(exit_dtm);
	bind_exit_readers(m_exit_dtm_by_material, m_exit_reader);

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
			for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
				for (Color c : { WHITE, BLACK })
					m_table->m_dtm[c][h].remove_disk_files(thread_pool);
		}
	}
	remove_checkpoint(ckpt_path);

	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtm[WHITE][0], batch, 2, 1);
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
				if (is_resume_fusion && static_cast<int64_t>(hmc) > resume_hmc) continue;

				try
				{
					if (egtb_is_interrupt_requested(true))
						throw DTM50_Interrupted{ 0, 0, hmc };

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
						m_table->m_dtm[WHITE][h].evict_all(thread_pool);
						m_table->m_dtm[BLACK][h].evict_all(thread_pool);
					}
					Checkpoint_File ckpt{};
					ckpt.batch_idx = static_cast<uint32_t>(bi);
					ckpt.fusion_idx = static_cast<uint32_t>(fi);
					ckpt.hmc = e.hmc;
					write_checkpoint(ckpt_path, ckpt);
					throw;
				}
			}
		}
	}

	m_sub_dtm_by_material.clear();
	m_exit_dtm_by_material.clear();
	thread_pool->respawn_all_threads();

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (%zu hmc layers): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

using DTM50_Save_Cache = Save_Group_Cache<DTM50_Final_Entry, DTM50_Intermediate_Entry>;
using DTM50_Pinned_Range = Pinned_Group_Range<DTM50_Final_Entry, DTM50_Intermediate_Entry>;

size_t dtm50_table_idx_of(Color c, size_t h)
{
	return static_cast<size_t>(c) * DTM50_HMC_COUNT + h;
}

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
	Layered_Rank_Table& ranks)
{
	const size_t ng = table.m_dtm[color][0].num_groups();

	std::array<std::array<uint8_t, Layered_Rank_Table::LUT_SIZE>, DTM50_PACK_LAYERS> per_layer_seen{};

	struct Seen_Pair
	{
		std::array<uint8_t, Layered_Rank_Table::LUT_SIZE> base{};
		std::array<uint8_t, Layered_Rank_Table::LUT_SIZE> layer0{};
	};

	const auto bins = gather_egtb_info_parallel<Seen_Pair>(
		thread_pool, epsi, cache, color, num_positions, max_workers, info,
		[&](Seen_Pair& s, size_t idx, const DTM50_Final_Entry& e) {
			if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
				s.layer0[dtm50_value_for_storage(e)] = 1;
			if (!e.is_illegal())
			{
				const DTM_Final_Entry d0 =
					dtm.read(color, static_cast<Board_Index>(idx));
				if (!d0.is_draw() && !(EGTB_GEN_LOSS_ONLY && d0.is_win()))
					s.base[dtm_value_for_storage(d0)] = 1;
			}
		},
		[color](size_t) { return dtm50_table_idx_of(color, 0); });

	for (const Seen_Pair& s : bins)
		for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
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
					if (!e.is_draw() && !(EGTB_GEN_LOSS_ONLY && e.is_win()))
						seen[dtm50_value_for_storage(e)] = 1;
				}
			}
		}
	});

	// Values present in more layers get smaller ranks (LZMA bias).
	std::array<uint64_t, Layered_Rank_Table::LUT_SIZE> score{};
	for (size_t lp = 0; lp < DTM50_PACK_LAYERS; ++lp)
		for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
			if (per_layer_seen[lp][v]) ++score[v];

	std::vector<uint16_t> values;
	values.reserve(64);
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
		std::string("save_compress_dtm50 ") + std::to_string(static_cast<int>(color)));

	out.block_positions = narrowing_static_cast<uint32_t>(bp);
	out.block_cnt = bcnt;
	out.tail_positions = (tail == bp) ? 0u : narrowing_static_cast<uint32_t>(tail);
	out.compressed_blocks = Compressed_Block_Store(std::move(spill_path), bcnt, static_cast<size_t>(block_size));
	out.usizes.resize(bcnt);

	auto& src = table.m_dtm[color][0];

	const auto perm_plan = make_index_permutation_plan(table.m_epsi, index_perm);

	std::atomic<size_t> next_block_id(0);

	thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
		if (thread_id >= effective_workers) return;

		std::vector<uint16_t> chunk(bp * DTM50_PACK_LAYERS);
		std::vector<size_t> logical_pos(bp);
		DTM50_Block_Encoder encoder;
		encoder.ranks = &out.ranks;
		LZMA_Compress_Helper lzma;

		for (;;)
		{
			const size_t b = next_block_id.fetch_add(1, std::memory_order_relaxed);
			if (b >= bcnt) return;

			const size_t p_base = b * bp;
			const size_t this_bp =
				(b == bcnt - 1 && out.tail_positions != 0) ? out.tail_positions : bp;

			const size_t want_lo = src.group_id_of_index(p_base);
			const size_t want_hi = src.group_id_of_index(p_base + this_bp - 1);
			for (size_t k = 0; k < this_bp; ++k)
				logical_pos[k] = storage_index_to_logical_index(perm_plan, p_base + k);

			// Storage zero is live; only entry class can identify a skipped block.
			bool uniform_skip = true;
			uint16_t* const base = chunk.data();  // pack layer 0 (unbounded DTM)
			for (size_t lp = 1; lp < DTM50_PACK_LAYERS; ++lp)
			{
				const size_t h = lp - 1;
				DTM50_Pinned_Range pin(cache, dtm50_table_idx_of(color, h), want_lo, want_hi);
				uint16_t* row = chunk.data() + lp * this_bp;
				if (h != 0)
				{
					// Layer 0 gates the column.
					for (size_t k = 0; k < this_bp; ++k)
					{
						if (base[k] == DTM_Final_Entry::ILLEGAL_VAL) continue;
						const auto e = table.read<DTM50_Final_Entry>(color, static_cast<uint16_t>(h), static_cast<Board_Index>(logical_pos[k]));
						row[k] = dtm50_value_for_storage(e);
					}
					continue;
				}
				for (size_t k = 0; k < this_bp; ++k)
				{
					const auto e = table.read<DTM50_Final_Entry>(color, static_cast<uint16_t>(h), static_cast<Board_Index>(logical_pos[k]));
					row[k] = dtm50_value_for_storage(e);
					// Legality gates the unbounded DTM row.
					if (e.is_illegal())
					{
						base[k] = DTM_Final_Entry::ILLEGAL_VAL;
						continue;
					}
					const DTM_Final_Entry d0 =
						dtm.read(color, static_cast<Board_Index>(logical_pos[k]));
					if (d0.is_draw() || (EGTB_GEN_LOSS_ONLY && d0.is_win()))
					{
						base[k] = DTM_Final_Entry::ILLEGAL_VAL;
						continue;
					}
					base[k] = dtm_value_for_storage(d0);
					uniform_skip = false;
				}
				if (uniform_skip) break;
			}

			if (uniform_skip)
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

	Layered_Compressed_Color color_out[COLOR_NB]{};
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
			// DTM50 inherits DTM's value geometry and permutation.
			index_perm[me] = dtm_probe.m_per_color[me].plan.perm;
			save_compress_dtm50(
				thread_pool, *m_table, cache, dtm_probe, me, num_positions,
				DTM50_BLOCK_SIZE,
				index_perm[me], max_workers,
				paths.block_spill_path(m_epsi, me),
				color_out[me]);
		}

		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
		{
			cache.purge(dtm50_table_idx_of(me, h));
			m_table->m_dtm[me][h].remove_disk_files(thread_pool);
			m_table->m_dtm[me][h].close();
		}

		thread_pool->respawn_all_threads();
	}

	if (m_is_symmetric)
	{
		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
		{
			cache.purge(dtm50_table_idx_of(BLACK, h));
			m_table->m_dtm[BLACK][h].remove_disk_files(thread_pool);
			m_table->m_dtm[BLACK][h].close();
		}
	}

	save_layered_table(thread_pool, m_epsi, index_perm, color_out, paths.dtm50_save_path(m_epsi), colors,
		EGTB_GEN_LOSS_ONLY, EGTB_Magic::DTM50_MAGIC);

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
