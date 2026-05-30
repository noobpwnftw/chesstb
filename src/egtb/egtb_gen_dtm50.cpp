#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_compress.h"
#include "egtb/slice_storage.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/index_permutation.h"
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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>

namespace {

struct Checkpoint_File
{
	static constexpr uint64_t MAGIC = 0x30354D44544D4843ull;  // 'CHMTDM50'
	static constexpr uint32_t VERSION = 1;
	uint64_t magic = MAGIC;
	uint32_t version = VERSION;
	uint32_t batch_idx = 0;
	uint32_t fusion_idx = 0;
	uint16_t hmc = 0;
	uint8_t  _pad[2] = { 0, 0 };
};
static_assert(sizeof(Checkpoint_File) == 24, "DTM50 Checkpoint_File size");

bool read_checkpoint(const std::filesystem::path& p, Checkpoint_File* out)
{
	std::error_code ec;
	if (!std::filesystem::exists(p, ec)) return false;
	std::ifstream fp(p, std::ios::binary);
	if (!fp) return false;
	Checkpoint_File c{};
	fp.read(reinterpret_cast<char*>(&c), sizeof(c));
	if (!fp || fp.gcount() != sizeof(c)) return false;
	if (c.magic != Checkpoint_File::MAGIC) return false;
	if (c.version != Checkpoint_File::VERSION) return false;
	*out = c;
	return true;
}

void write_checkpoint(const std::filesystem::path& p, const Checkpoint_File& c)
{
	std::filesystem::create_directories(p.parent_path());
	const auto tmp = p.string() + ".tmp";
	{
		std::ofstream fp(tmp, std::ios::binary | std::ios::trunc);
		fp.write(reinterpret_cast<const char*>(&c), sizeof(c));
		fp.flush();
	}
	std::error_code ec;
	std::filesystem::rename(tmp, p, ec);
}

void remove_checkpoint(const std::filesystem::path& p)
{
	std::error_code ec;
	std::filesystem::remove(p, ec);
}

}  // namespace

DTM50_Generator::DTM50_Generator(
	const Piece_Config& ps,
	const std::map<Material_Key, std::shared_ptr<DTM50_Sub_File_Flat>>& sub_dtm,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTM50_Table>(ps, tmp_dir)),
	m_sub_dtm(sub_dtm)
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	// Budget is "unbounded" only if it fits the save-time peak (all HMC_COUNT
	// layers × 2 colors), not just the gen peak (~3 layers).
	const size_t bytes_per_color =
		m_table->m_dtm[WHITE][0].num_slices()
		* m_table->m_dtm[WHITE][0].within_slice_size()
		* sizeof(DTM_Final_Entry);
	const size_t total_bytes = bytes_per_color * COLOR_NB * DTM50_HMC_COUNT;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_group_state(m_table->m_dtm[WHITE][0].num_groups());

	for (Color c : { WHITE, BLACK })
	for (Piece captured = PIECE_NONE; captured < PIECE_NB; captured = static_cast<Piece>(captured + 1))
	for (Piece_Type promo = PIECE_TYPE_NONE; promo < PIECE_TYPE_NB; promo = static_cast<Piece_Type>(promo + 1))
	{
		const Piece_Config_For_Gen* sub = m_sub_epsi_by_move[c][captured][promo];
		if (sub == nullptr) continue;
		if (sub->num_pieces() <= 2) continue;
		auto it = m_sub_dtm.find(sub->min_material_key());
		m_sub_dtm_by_move[c][captured][promo] = it->second.get();
	}
}

// =============================================================================
// Sub-TB and post-move reads.
// =============================================================================

DTM_Final_Entry DTM50_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move, size_t thread_id) const
{
	Color sub_color;
	const Piece_Config_For_Gen* sub_epsi = nullptr;
	const Board_Index sub_idx = next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

	const Position& parent = pos_gen.board_unchecked();
	Piece captured = PIECE_NONE;
	if (move.is_ep_capture())
		captured = parent.piece_at(sq_make(sq_rank(move.from()), sq_file(move.to())));
	else if (!parent.is_empty(move.to()))
		captured = parent.piece_at(move.to());
	const Piece_Type promo = move.is_promotion() ? move.promotion() : PIECE_TYPE_NONE;
	const DTM50_Sub_File_Flat* sub = m_sub_dtm_by_move[parent.turn()][captured][promo];

	// Resulting K vs K or sub-table unavailable → bare-king draw.
	return (sub == nullptr) ? DTM_Final_Entry::make_draw() : sub->read(sub_color, sub_idx, thread_id);
}

DTM_Final_Entry DTM50_Generator::read_post_move_dtm(Position_For_Gen& pos_gen, Move move, uint16_t hmc, size_t thread_id) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move, thread_id);

	const Color mover = parent.turn();
	const Color opp = color_opp(mover);
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	if (post_idx == BOARD_INDEX_NONE) return DTM_Final_Entry::make_illegal();

	// Pawn push → opp[0]; non-pawn quiet → opp[k+1]; k=99 targets virtual
	// hmc=100 (50MR draw unless mate). See header for build-order argument.
	const bool is_pawn_push = piece_type(parent.piece_at(move.from())) == PAWN;
	if (is_pawn_push)
		return read_dtm(post_idx, opp, 0);

	const uint16_t target_hmc = static_cast<uint16_t>(hmc + 1);
	if (target_hmc >= DTM50_HMC_COUNT)
	{
		Position child = parent;
		(void)child.do_move(move);
		if (!child.is_in_check(child.turn())) return DTM_Final_Entry::make_draw();
		Move_List ml;
		child.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
		for (size_t i = 0; i < ml.size(); ++i)
			if (child.is_pseudo_legal_move_legal(ml[i])) return DTM_Final_Entry::make_draw();
		return DTM_Final_Entry::make_loss(0);
	}
	return read_dtm(post_idx, opp, target_hmc);
}

namespace {

// True if `a` is strictly better for the mover: highest class, then shorter
// WIN / longer LOSS.
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

DTM_Final_Entry DTM50_Generator::effective_opp_dtm_after_dp(Position_For_Gen& pos_gen, Move dp_move, uint16_t hmc, size_t thread_id) const
{
	const DTM_Final_Entry no_ep = read_post_move_dtm(pos_gen, dp_move, hmc, thread_id);

	// do_move/undo_move on pos_gen's own board: pos_gen's cached board matches
	// its index again after the undo, so callers see no change.
	Position& p = pos_gen.board_unchecked();
	const Color opp = color_opp(p.turn());
	const Piece captured_by_dp = p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	DTM_Final_Entry best_ep_for_opp = DTM_Final_Entry::make_loss(0);  // worst for opp; bumped on first EP
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
			if (child_idx == BOARD_INDEX_NONE) continue;
			p_gen_for_ep.emplace(m_epsi, child_idx, opp);
		}
		// read_sub_tb returns the post-EP DTM from mover's perspective; invert
		// class and add 1 ply for opp's EP move.
		const DTM_Final_Entry after_ep = read_sub_tb(*p_gen_for_ep, ep_move, thread_id);
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

// =============================================================================
// Initial classification.
// =============================================================================

DTM_Final_Entry DTM50_Generator::make_initial_entry(Position_For_Gen& pos_gen, uint16_t hmc, size_t thread_id) const
{
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM_Final_Entry::make_illegal();

	Position& pos = pos_gen.board();
	const bool in_check = pos.is_in_check(pos.turn());

	uint16_t best_win_dtm  = std::numeric_limits<uint16_t>::max();
	uint16_t best_loss_dtm = 0;
	bool saw_win  = false;
	bool saw_draw = false;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	bool any_legal = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;
		const DTM_Final_Entry child_e = is_pawn && is_pawn_double_push(m)
			? effective_opp_dtm_after_dp(pos_gen, m, hmc, thread_id)
			: read_post_move_dtm(pos_gen, m, hmc, thread_id);
		if (child_e.is_illegal()) continue;
		if (child_e.is_loss())
		{
			saw_win = true;
			const uint16_t cand = static_cast<uint16_t>(child_e.value() + 1);
			update_min(best_win_dtm, cand);
		}
		else if (child_e.is_win())
		{
			const uint16_t cand = static_cast<uint16_t>(child_e.value() + 1);
			update_max(best_loss_dtm, cand);
		}
		else
		{
			saw_draw = true;  // blocks forced-LOSS
		}
	}
	if (!any_legal)
	{
		if (in_check) return DTM_Final_Entry::make_loss(0);
		return DTM_Final_Entry::make_draw();
	}
	if (saw_win) return DTM_Final_Entry::make_win(best_win_dtm);
	if (saw_draw) return DTM_Final_Entry::make_draw();
	if (best_loss_dtm > 0) return DTM_Final_Entry::make_loss(best_loss_dtm);
	return DTM_Final_Entry::make_draw();
}

void DTM50_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool, uint16_t hmc)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	// Slice topology is layer-independent; any layer's metadata works.
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	// push_target_slices allocates; cache once.
	std::vector<std::vector<int32_t>> targets_by_pid(m_epsi.num_pawn_slices());
	for (int32_t pid : m_active_pawn_slices)
		targets_by_pid[static_cast<size_t>(pid)] = psm.push_target_slices(pid);

	// Working set per group: cur + king-neighbor (non-pawn quiet) + push-target
	// (pawn push) groups. needs[B] == needs[W]; mirrors page_in_for_group in DTM/DTC.
	const auto& kingsm = m_epsi.slice_manager();
	const bool has_pawns = psm.has_pawns();

	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
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
				kingsm.neighbors(kid, m_scratch_nbrs);
				for (int32_t pp : m_active_pawn_slices)
				{
					const size_t same = static_cast<size_t>(pp) * nks + static_cast<size_t>(kid);
					m_scratch_need[WHITE][same / spg] = 1;
					for (int32_t k : m_scratch_nbrs)
					{
						const size_t neigh = static_cast<size_t>(pp) * nks + static_cast<size_t>(k);
						m_scratch_need[WHITE][neigh / spg] = 1;
					}
				}
				if (has_pawns)
				{
					for (int32_t tpid : targets_by_pid[static_cast<size_t>(pid)])
					{
						const size_t target =
							static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
						m_scratch_need[WHITE][target / spg] = 1;
					}
				}
			}
		}
		m_scratch_need[BLACK] = m_scratch_need[WHITE];
		apply_working_set(thread_pool,
			&m_table->m_dtm[WHITE][hmc], &m_table->m_dtm[BLACK][hmc],
			m_scratch_need[WHITE], m_scratch_need[BLACK]);
		// opp[0]: pawn-push reads. opp[k+1]: non-pawn quiet + ILLEGAL reuse.
		apply_working_set(thread_pool,
			&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK]);
		if (hmc + 1 < DTM50_HMC_COUNT)
		{
			apply_working_set(thread_pool,
				&m_table->m_dtm[WHITE][hmc + 1],
				&m_table->m_dtm[BLACK][hmc + 1],
				m_scratch_need[WHITE], m_scratch_need[BLACK]);
		}
	};

	// Progress bar only on hmc=99: its per-cell legality check dominates;
	// other layers reuse ILLEGAL from opp[k+1].
	std::optional<Concurrent_Progress_Bar> progress_bar;
	if (hmc == DTM50_HMC_COUNT - 1)
	{
		const size_t wss = m_epsi.within_slice_size();
		size_t total_indices = 0;
		for (size_t g : m_pair_group_ids)
		{
			const size_t g_start_slice = g * spg;
			const size_t g_end_slice = std::min(g_start_slice + spg, ntotal);
			total_indices += (g_end_slice - g_start_slice) * wss;
		}
		const size_t PRINT_PERIOD = thread_pool->num_workers() * (1 << 20);
		progress_bar.emplace(total_indices, PRINT_PERIOD, "init_entries");
	}
	else
	{
		std::printf("  build layer %2u\r", hmc);
		std::fflush(stdout);
	}

	for (size_t g : m_pair_group_ids)
	{
		page_in_for_init_group(g);
		Shared_Board_Index_Iterator group_it = make_slice_group_iterator(g, spg);

		thread_pool->run_sync_task_on_all_threads([&](size_t tid) {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			size_t local_progress = 0;
			const auto& slice_has_stab = m_epsi.slice_manager().slice_has_stabilizer;
			for (const Board_Index idx : group_it.indices())
			{
				if (++local_progress % PROGRESS_BAR_UPDATE_PERIOD == 0 && progress_bar)
					*progress_bar += PROGRESS_BAR_UPDATE_PERIOD;
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
					write_dtm(idx, WHITE, hmc, DTM_Final_Entry::make_illegal());
					write_dtm(idx, BLACK, hmc, DTM_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board());
					if (canon != idx)
					{
						write_dtm(idx, WHITE, hmc, DTM_Final_Entry::make_illegal());
						write_dtm(idx, BLACK, hmc, DTM_Final_Entry::make_illegal());
						continue;
					}
				}
				else
				{
					ASSERT(board_index_of_position(m_epsi, pos_gen.board()) == idx);
				}
				for (Color us : { WHITE, BLACK })
				{
					pos_gen.set_turn(us);
					// Chess-legality is hmc-invariant; reuse opp[k+1]'s ILLEGAL.
					if (hmc + 1 < DTM50_HMC_COUNT
					    && read_dtm(idx, us, hmc + 1).is_illegal())
					{
						write_dtm(idx, us, hmc, DTM_Final_Entry::make_illegal());
						continue;
					}
					write_dtm(idx, us, hmc, make_initial_entry(pos_gen, hmc, tid));
				}
			}
		});
	}

	if (progress_bar) progress_bar->set_finished();
}

// =============================================================================
// gen() orchestrator with paging + checkpoint/resume.
// =============================================================================

void DTM50_Generator::gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

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
			{
				m_table->m_dtm[WHITE][h].remove_disk_files();
				m_table->m_dtm[BLACK][h].remove_disk_files();
			}
		}
	}

	// Outer = topo batch / fusion, inner = hmc 99..0; see header for the
	// read-dependency argument.
	size_t total_fusions = 0;
	for (size_t bi = 0; bi < batches.size(); ++bi)
	{
		if (static_cast<int64_t>(bi) < resume_batch_idx) continue;
		const auto& batch = batches[bi];
		const auto fusions = compute_fusion_groups(m_table->m_dtm[WHITE][0], batch);
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
			    static_cast<int64_t>(fi) < resume_fusion_idx) continue;

			const auto& fusion = fusions[fi];

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
						if (hmc + 1 < DTM50_HMC_COUNT)
						{
							apply_working_set(thread_pool,
								&m_table->m_dtm[WHITE][hmc + 1], &m_table->m_dtm[BLACK][hmc + 1],
								all_needed, all_needed);
						}
					}

					// k+2 was the prior step's read layer; no future step needs it.
					if (hmc + 2 < DTM50_HMC_COUNT)
					{
						m_table->m_dtm[WHITE][hmc + 2].evict_all(*thread_pool);
						m_table->m_dtm[BLACK][hmc + 2].evict_all(*thread_pool);
					}

					refresh_active_metadata(m_table->m_dtm[WHITE][0]);

					init_entries(thread_pool, hmc);
				}
				catch (const DTM50_Interrupted& e)
				{
					for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
					{
						m_table->m_dtm[WHITE][h].evict_all(*thread_pool);
						m_table->m_dtm[BLACK][h].evict_all(*thread_pool);
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

	remove_checkpoint(ckpt_path);
	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (%u hmc layers): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

// save_to_disk: pack 100 hmc layers into one .lzdtm50 plus a .info sidecar
// (hmc=0 stats). Design rationale (2-bit state, draw-end hint, uniform-skip
// sentinel, DRAW excluded from rank table) lives in README "Pack layout".
//
// File layout:
//   uint32  magic = EGTB_Magic::DTM50_MAGIC
//   uint32  key_and_table_num
//   per-color header:
//     uint8  flag (SINGULAR bit set ⇒ singular-WDL color)
//     if SINGULAR: uint8 singular_wdl
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
//                                          uint8 cp_bitmap[16] (100 bits used),
//                                          rank r0..r_{k-1}  (k-1 if draw_end)

// 1 << 19 matches DTC/DTM's 1 MB raw block at 2 B/pos.
constexpr uint32_t RS_BLOCK_POSITIONS = 1u << 19;

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

// Per-position state across the 100 layers: 00=CONST, 01=SINGLE (1 transition),
// 10=DOUBLE (2), 11=MULTI (3+). ILLEGAL is folded into CONST as last_legal_rank
// to keep LZMA runs long.
//
// Draw-end hint: trailing-DRAW is the dominant pattern when W/L runs out of
// 50MR. Non-CONST states carry a hint bit; when set, the final rank is omitted
// and the decoder synthesizes DRAW (stored=0). Hint bit lives in MSB of the
// final h byte (SINGLE/DOUBLE) or of the change-count byte (MULTI, k<=100).
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

		std::array<uint16_t, DTM50_HMC_COUNT> cp{};
		std::array<uint16_t, DTM50_HMC_COUNT> cr{};

		uint16_t last_legal_rank = 0;  // any rank works; ILLEGAL never decodes
		uint32_t num_single = 0;
		uint32_t num_double = 0;

		for (size_t i = 0; i < num_positions; ++i)
		{
			bool is_illegal = false;
			for (int h = 0; h < DTM50_HMC_COUNT; ++h)
			{
				if (values[h * num_positions + i] == DTM_Final_Entry::ILLEGAL_VAL)
				{
					is_illegal = true;
					break;
				}
			}
			if (is_illegal)
			{
				// CONST (state 00). Emit last_legal_rank to extend LZMA runs.
				write_rank_bytes(const_stream, last_legal_rank, eb);
				continue;
			}

			size_t k = 0;
			uint16_t prev = ranks->value_to_rank[values[0 * num_positions + i]];
			cp[k] = 0; cr[k] = prev; ++k;
			for (int h = 1; h < DTM50_HMC_COUNT; ++h)
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
				ASSERT(h1 >= 1 && h1 <= 99);
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
				ASSERT(h1 >= 1 && h2 >= 2 && h1 < h2 && h2 <= 99);
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
	WDL_Entry singular_wdl = WDL_Entry::DRAW;
	DTM50_Rank_Table ranks;
	uint32_t block_positions = 0;
	uint64_t block_cnt = 0;
	uint32_t tail_positions = 0;
	std::vector<std::vector<uint8_t>> compressed_blocks;
	// Plain LZMA has no end-marker, so the decoder needs each block's usz.
	std::vector<uint64_t> usizes;
	size_t total_compressed_size = 0;
};

using DTM50_Save_Cache = Save_Group_Cache<DTM_Final_Entry>;
using DTM50_Pinned_Range = Pinned_Group_Range<DTM_Final_Entry>;

INLINE size_t dtm50_table_idx_of(Color c, int h)
{
	return static_cast<size_t>(c) * static_cast<size_t>(DTM50_HMC_COUNT)
		+ static_cast<size_t>(h);
}

NODISCARD Value_Rank_Table make_trial_rank_table(const DTM50_Rank_Table& ranks)
{
	Value_Rank_Table out;
	out.ranks = ranks.rank_to_value;
	out.value_to_rank.assign(DTM50_Rank_Table::LUT_SIZE, Value_Rank_Table::NO_RANK);
	for (size_t i = 0; i < out.ranks.size(); ++i)
		out.value_to_rank[out.ranks[i]] = static_cast<uint16_t>(i);
	return out;
}

NODISCARD Block_Source make_dtm50_layer0_block_source(
	const Piece_Config_For_Gen& epsi,
	DTM50_Table& table,
	DTM50_Save_Cache& cache,
	Color color,
	size_t num_positions,
	uint32_t index_perm,
	size_t entry_bytes,
	size_t block_positions)
{
	auto& src = table.m_dtm[color][0];
	const size_t bp = block_positions;
	const size_t source_block_bytes = bp * sizeof(DTM_Final_Entry);
	const size_t source_total_bytes = num_positions * sizeof(DTM_Final_Entry);
	const size_t output_total_bytes = num_positions * entry_bytes;

	auto group_id_of_pos = [&src](size_t p) {
		return src.group_id_of(src.slice_id_of(p));
	};

	const auto perm_plan = make_index_permutation_plan(epsi, index_perm);
	return Block_Source{
		output_total_bytes,
		[&src, &cache, color, source_block_bytes, source_total_bytes,
		 perm_plan, group_id_of_pos](size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t> {
			const size_t block_off = block_id * source_block_bytes;
			const size_t this_block = std::min(source_block_bytes, source_total_bytes - block_off);
			ASSERT(scratch.size() >= this_block);
			ASSERT(block_off % sizeof(DTM_Final_Entry) == 0);
			ASSERT(this_block % sizeof(DTM_Final_Entry) == 0);

			const size_t storage_pos_off = block_off / sizeof(DTM_Final_Entry);
			const size_t pos_cnt = this_block / sizeof(DTM_Final_Entry);
			const size_t want_lo = group_id_of_pos(storage_pos_off);
			const size_t want_hi = group_id_of_pos(storage_pos_off + pos_cnt - 1);
			DTM50_Pinned_Range pin(cache, dtm50_table_idx_of(color, 0), want_lo, want_hi);

			for (size_t k = 0; k < pos_cnt; ++k)
			{
				const size_t storage_idx = storage_pos_off + k;
				const size_t logical_idx = storage_index_to_logical_index(perm_plan, storage_idx);
				const DTM_Final_Entry e = src.read(static_cast<Board_Index>(logical_idx));
				std::memcpy(scratch.data() + k * sizeof(DTM_Final_Entry), &e, sizeof(e));
			}

			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

// h=0 feeds EGTB_Info (the canonical reset-50MR view); other layers only
// contribute to the rank score. Returns false iff no W/L storage values appear.
NODISCARD bool gather_dtm50_info(
	const Piece_Config_For_Gen& epsi,
	DTM50_Table& table,
	DTM50_Save_Cache& cache,
	Color color,
	size_t num_positions,
	size_t positions_per_group,
	In_Out_Param<Thread_Pool> thread_pool,
	size_t max_workers,
	EGTB_Info& info,
	DTM50_Rank_Table& ranks)
{
	const size_t ng = table.m_dtm[color][0].num_groups();

	std::array<std::array<uint8_t, DTM50_Rank_Table::LUT_SIZE>, DTM50_HMC_COUNT> per_layer_seen{};

	const size_t n = static_cast<size_t>(DTM50_HMC_COUNT);
	const size_t capped_n = (max_workers == 0) ? n : std::min(n, max_workers);
	const size_t workers = std::min(thread_pool->num_workers(), capped_n);
	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
		for (;;)
		{
			const size_t h = next.fetch_add(1, std::memory_order_relaxed);
			if (h >= n) return;
			const size_t ti = dtm50_table_idx_of(color, static_cast<int>(h));
			auto& tbl = table.m_dtm[color][h];
			auto& seen = per_layer_seen[h];
			const bool gather_info = (h == 0);
			for (size_t g = 0; g < ng; ++g)
			{
				DTM50_Pinned_Range pin(cache, ti, g, g);
				const size_t p_lo = g * positions_per_group;
				const size_t p_hi = std::min(p_lo + positions_per_group, num_positions);
				Decomposed_Board_Index didx{};
				if (gather_info)
					epsi.decompose_board_index(static_cast<Board_Index>(p_lo), out_param(didx));
				for (size_t p = p_lo; p < p_hi; ++p)
				{
					const DTM_Final_Entry e = tbl.read(static_cast<Board_Index>(p));
					// DRAW/ILLEGAL are routed via the WDL companion; the rank
					// table only needs W/L storage values (LOSS-in-0 still adds
					// storage 0 normally).
					if (!e.is_illegal() && !e.is_draw())
						seen[dtm_value_for_storage(e)] = 1;
					if (gather_info)
					{
						const uint64_t w = epsi.orbit_weight(didx);
						info.add_result(color, e.wdl(), w);
						if (e.is_win())
							info.maybe_update_longest_win(color, p, e.value());
						epsi.step_to_next(inout_param(didx));
					}
				}
			}
		}
	});

	// Values present in more layers get smaller ranks (LZMA bias).
	std::array<uint64_t, DTM50_Rank_Table::LUT_SIZE> score{};
	for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
		for (size_t v = 0; v < DTM50_Rank_Table::LUT_SIZE; ++v)
			if (per_layer_seen[h][v]) ++score[v];

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
	Color color,
	size_t num_positions,
	uint32_t block_positions,
	uint32_t index_perm,
	size_t max_workers,
	DTM50_Compressed_Color& out)
{
	const size_t bp = static_cast<size_t>(block_positions);
	const size_t bcnt = ceil_div(num_positions, bp);
	const size_t tail = num_positions - (bcnt - 1) * bp;

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8 * DTM50_HMC_COUNT;
	const size_t raw_block_bytes = bp * DTM50_HMC_COUNT * sizeof(DTM_Final_Entry);
	const size_t print_period = std::max<size_t>(
		1, ceil_div(PRINT_PERIOD_BYTES * thread_pool->num_workers(), raw_block_bytes));
	Concurrent_Progress_Bar progress_bar(bcnt, print_period,
		std::string("save_compress_dtm50 ") + std::to_string(static_cast<int>(color)));

	out.block_positions = block_positions;
	out.block_cnt = bcnt;
	out.tail_positions = (tail == bp) ? 0u : static_cast<uint32_t>(tail);
	out.compressed_blocks.resize(bcnt);
	out.usizes.resize(bcnt);

	auto& src = table.m_dtm[color][0];
	auto group_id_of_pos = [&src](size_t p) {
		return src.group_id_of(src.slice_id_of(p));
	};

	const size_t pool_workers = thread_pool->num_workers();
	const size_t effective_workers = (max_workers == 0)
		? pool_workers : std::min(max_workers, pool_workers);

	const auto perm_plan = make_index_permutation_plan(table.m_epsi, index_perm);

	std::atomic<size_t> next_block_id(0);

	thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
		if (thread_id >= effective_workers) return;

		std::vector<uint16_t> chunk(bp * DTM50_HMC_COUNT);
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
			for (int h = 0; h < DTM50_HMC_COUNT; ++h)
			{
				DTM50_Pinned_Range pin(cache, dtm50_table_idx_of(color, h), want_lo, want_hi);
				auto& tbl = table.m_dtm[color][h];
				uint16_t* row = chunk.data() + static_cast<size_t>(h) * this_bp;
				for (size_t k = 0; k < this_bp; ++k)
				{
					const DTM_Final_Entry e = tbl.read(static_cast<Board_Index>(logical_pos[k]));
					row[k] = dtm_value_for_storage(e);
					if (e.is_win() || e.is_loss())
						uniform_skip = false;
				}
			}

			if (uniform_skip)
			{
				// usz=0 in the offset table is the skip sentinel.
				out.usizes[b] = 0;
				out.compressed_blocks[b].clear();
				progress_bar += 1;
				continue;
			}

			const Const_Span<uint8_t> payload = encoder.encode(chunk.data(), this_bp);
			std::vector<uint8_t> compressed = lzma.compress(payload);
			if (compressed.size() > 0xFFFFFFFFu)
				throw std::runtime_error("rs block too large for offset encoding");

			out.usizes[b] = payload.size();
			out.compressed_blocks[b] = std::move(compressed);
			progress_bar += 1;
		}
	});

	out.total_compressed_size = 0;
	for (const auto& cb : out.compressed_blocks)
		out.total_compressed_size += cb.size();

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
			cur += co.compressed_blocks[k].size();  // skip-block size == 0
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
			// index permutation config(8)+entry_bytes(1)+block_positions(4)+block_cnt(8)+tail_positions(4)+data_size(8)+num_ranks(2)
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
			w.write<uint8_t>(static_cast<uint8_t>(co.singular_wdl));
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
		for (const auto& blk : co.compressed_blocks)
			w.write(Const_Span<uint8_t>(blk.data(), blk.size()));
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
	const size_t group_bytes = positions_per_group * sizeof(DTM_Final_Entry);
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

	std::vector<Sliced_EGTB_File_For_Gen<DTM_Final_Entry>*> all_tables;
	all_tables.reserve(COLOR_NB * DTM50_HMC_COUNT);
	for (Color c : { WHITE, BLACK })
		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			all_tables.push_back(&m_table->m_dtm[c][h]);
	DTM50_Save_Cache cache(std::move(all_tables), cap_groups);

	DTM50_Compressed_Color color_out[COLOR_NB]{};
	uint32_t index_perm[COLOR_NB] = {
		default_index_permutation_config(m_epsi),
		default_index_permutation_config(m_epsi)
	};

	for (Color me : colors)
	{
		const bool any = gather_dtm50_info(
			m_epsi, *m_table, cache, me, num_positions, positions_per_group,
			thread_pool, max_workers, m_info, color_out[me].ranks);

		if (!any)
		{
			std::printf("save dtm50 %d: singular\n", static_cast<int>(me));
			color_out[me].is_singular = true;
			color_out[me].singular_wdl = WDL_Entry::DRAW;
		}
		else
		{
			Value_Rank_Table trial_rank = make_trial_rank_table(color_out[me].ranks);
			index_perm[me] = choose_storage_permutation_config(
				thread_pool,
				m_epsi,
				[&](uint32_t perm) {
					return make_dtm50_layer0_block_source(
						m_epsi, *m_table, cache, me, num_positions,
						perm, color_out[me].ranks.entry_bytes, RS_BLOCK_POSITIONS);
				},
				static_cast<size_t>(RS_BLOCK_POSITIONS) * color_out[me].ranks.entry_bytes,
				std::make_unique<LZMA_Rank_Compress_Helper>(
					trial_rank, color_out[me].ranks.entry_bytes, &dtm_storage_fn),
				"choose_dtm50_storage");
			save_compress_dtm50(
				thread_pool, *m_table, cache, me, num_positions,
				RS_BLOCK_POSITIONS, index_perm[me], max_workers, color_out[me]);
		}

		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
		{
			cache.purge(dtm50_table_idx_of(me, static_cast<int>(h)));
			m_table->m_dtm[me][h].remove_disk_files();
			m_table->m_dtm[me][h].close();
		}
	}

	const auto out_path = paths.dtm50_save_path(m_epsi);
	std::filesystem::create_directories(out_path.parent_path());
	save_dtm50_table(m_epsi, index_perm, color_out, out_path, colors);

	for (Color me : colors)
	{
		if (m_info.longest_win[me] == 0) continue;
		Position_For_Gen pos_gen(m_epsi,
			static_cast<Board_Index>(m_info.longest_idx[me]), me);
		pos_gen.board().to_fen(Span(m_info.longest_fen[me]));
	}
	std::ofstream fp(paths.dtm50_info_save_path(m_epsi),
		std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
