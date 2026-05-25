#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_compress.h"
#include "egtb/slice_storage.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/attack.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/defines.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/memory.h"
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

	// Gen holds 1–3 layers at the hmc peak (k, k+1, opp[0]); save_to_disk
	// holds all HMC_COUNT layers × 2 colors at once to assemble the change-
	// point encoding — the 100× amplifier between the two phases. We clamp
	// the budget to "unbounded" only when it can fit the save peak, so save's
	// Save_Group_Cache doesn't silently page-thrash on budgets that were
	// "more than enough" for gen alone.
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

DTM_Final_Entry DTM50_Generator::read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
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

DTM_Final_Entry DTM50_Generator::read_post_move_dtm(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move, thread_id);

	const Color mover = parent.turn();
	const Color opp = color_opp(mover);
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	if (post_idx == BOARD_INDEX_NONE) return DTM_Final_Entry::make_illegal();

	// Pawn push → opp[hmc=0]; non-pawn quiet → opp[hmc=k+1]; k=99 has no k+1
	// so the move targets virtual hmc=100 (50MR draw unless mate). See header
	// for the build-order argument that makes both reads finalized.
	const bool is_pawn_push = piece_type(parent.piece_at(move.from())) == PAWN;
	if (is_pawn_push)
		return read_dtm(post_idx, opp, 0);

	const uint16_t target_hmc = static_cast<uint16_t>(m_current_hmc + 1);
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

// Comparator: returns true if `a` is strictly better for the side that's about
// to MAKE the move (i.e., we want the highest-class outcome; tiebreak by smaller
// dtm if WIN, larger dtm if LOSS).
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

DTM_Final_Entry DTM50_Generator::effective_opp_dtm_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const
{
	const DTM_Final_Entry no_ep = read_post_move_dtm(pos_gen, dp_move, thread_id);

	const Position& parent = pos_gen.board_unchecked();
	const Color mover = parent.turn();
	const Color opp = color_opp(mover);
	Position p = parent;
	(void)p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	DTM_Final_Entry best_ep_for_opp = DTM_Final_Entry::make_loss(0);  // worst possible for opp; bumped on first EP
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
		// read_sub_tb returns the post-EP DTM from the new STM's (mover's)
		// perspective. Invert class AND add 1 ply to account for opp's EP move.
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

	if (!any_ep) return no_ep;
	return dtm_better_for_mover(best_ep_for_opp, no_ep) ? best_ep_for_opp : no_ep;
}

// =============================================================================
// Initial classification.
// =============================================================================

DTM_Final_Entry DTM50_Generator::make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id) const
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
			? effective_opp_dtm_after_dp(pos_gen, m, thread_id)
			: read_post_move_dtm(pos_gen, m, thread_id);
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
		return in_check ? DTM_Final_Entry::make_loss(0) : DTM_Final_Entry::make_draw();
	if (saw_win)  return DTM_Final_Entry::make_win(best_win_dtm);
	if (saw_draw) return DTM_Final_Entry::make_draw();
	if (best_loss_dtm > 0) return DTM_Final_Entry::make_loss(best_loss_dtm);
	return DTM_Final_Entry::make_draw();
}

void DTM50_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	// Slice topology is layer-independent (every layer has the same shape) —
	// any layer can provide slices_per_group/num_groups metadata.
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	// push_target_slices allocates; cache once per init call.
	std::vector<std::vector<int32_t>> targets_by_pid(m_epsi.num_pawn_slices());
	for (int32_t pid : m_active_pawn_slices)
		targets_by_pid[static_cast<size_t>(pid)] = psm.push_target_slices(pid);

	// Per-group working set: cur group + opp's king-neighbor groups (non-pawn
	// quiet reads cross king-slices) + opp's push-target groups (in-M pawn
	// push reads). Init writes both colors, so needs[B] == needs[W]. Same
	// pattern as page_in_for_group in DTM/DTC.
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
		apply_working_set(thread_pool, &cur_layer(WHITE), &cur_layer(BLACK), m_scratch_need[WHITE], m_scratch_need[BLACK]);
		// opp[0] for pawn-push reads, opp[k+1] for non-pawn quiet + ILLEGAL.
		apply_working_set(thread_pool,
			&m_table->m_dtm[WHITE][0], &m_table->m_dtm[BLACK][0],
			m_scratch_need[WHITE], m_scratch_need[BLACK]);
		if (m_current_hmc + 1 < DTM50_HMC_COUNT)
		{
			apply_working_set(thread_pool,
				&m_table->m_dtm[WHITE][m_current_hmc + 1],
				&m_table->m_dtm[BLACK][m_current_hmc + 1],
				m_scratch_need[WHITE], m_scratch_need[BLACK]);
		}
	};

	// Progress bar only on hmc=99: the full legality check per cell dominates
	// its runtime. Every other layer piggybacks ILLEGAL from opp[k+1].
	std::optional<Concurrent_Progress_Bar> progress_bar;
	if (m_current_hmc == DTM50_HMC_COUNT - 1)
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
		std::printf("  build layer %2u\r", m_current_hmc);
		std::fflush(stdout);
	}

	for (size_t g : m_pair_group_ids)
	{
		if (egtb_is_interrupt_requested())
			throw DTM50_Interrupted{ 0, 0, m_current_hmc };
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
					write_dtm(idx, WHITE, DTM_Final_Entry::make_illegal());
					write_dtm(idx, BLACK, DTM_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board());
					if (canon != idx)
					{
						write_dtm(idx, WHITE, DTM_Final_Entry::make_illegal());
						write_dtm(idx, BLACK, DTM_Final_Entry::make_illegal());
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
					// Chess-legality is hmc-invariant; reuse opp[k+1]'s ILLEGAL
					// flag. hmc=99 has no k+1 and computes fresh.
					if (m_current_hmc + 1 < DTM50_HMC_COUNT
					    && read_dtm(idx, us, m_current_hmc + 1).is_illegal())
					{
						write_dtm(idx, us, DTM_Final_Entry::make_illegal());
						continue;
					}
					write_dtm(idx, us, make_initial_entry(pos_gen, tid));
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

	// Outer = topo batch / fusion, inner = hmc 99..0; see header for the read
	// dependency argument that this order satisfies.
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

			for (size_t i = DTM50_HMC_COUNT; i-- > 0; )
			{
				const uint16_t hmc = static_cast<uint16_t>(i);
				// Within the resume fusion, layers 99..(resume_hmc+1) were
				// already finished; resume at resume_hmc and run the rest fresh.
				if (is_resume_fusion && static_cast<int64_t>(hmc) > resume_hmc) continue;

				m_current_hmc = hmc;

				// Unbounded budget: pre-load every group of the layers this
				// step reads (cur layer + opp[0] + opp[k+1]). Bounded mode
				// does the per-group equivalent in page_in_for_init_group.
				if (m_paging_budget_bytes == 0)
				{
					const size_t ng = cur_layer(WHITE).num_groups();
					std::vector<uint8_t> all_needed(ng, 1);
					apply_working_set(thread_pool, &cur_layer(WHITE), &cur_layer(BLACK), all_needed, all_needed);
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

				// k+2 was the previous step's read layer; no future step in
				// this fusion needs it.
				if (hmc + 2 < DTM50_HMC_COUNT)
				{
					m_table->m_dtm[WHITE][hmc + 2].evict_all(*thread_pool);
					m_table->m_dtm[BLACK][hmc + 2].evict_all(*thread_pool);
				}

				refresh_active_metadata(m_table->m_dtm[WHITE][0]);

				try
				{
					init_entries(thread_pool);
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

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (%u hmc layers): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

namespace {

// =============================================================================
// save_to_disk: pack 100 hmc layers into a single .lzdtm50 via a layer-axis
// change-point encoding, plus a sidecar .info with hmc=0 stats.
//
// File layout:
//   uint32  magic = EGTB_Magic::DTM50_MAGIC
//   uint32  key_and_table_num
//   per-color:
//     uint8  flag (SINGULAR bit set ⇒ singular-WDL color)
//     if SINGULAR:
//       uint8 singular_wdl
//     else:
//       uint8  entry_bytes  (1 or 2)
//       uint32 block_positions
//       uint32 block_cnt
//       uint32 tail_positions
//       uint64 data_size
//       uint16 num_ranks
//       uint16 rank_to_value[num_ranks]
//   per-color (non-singular): offset_tb[block_cnt] of uint64 = (doff << 20) | dsz
//   align 64
//   per-color (non-singular): compressed data, ceil64-aligned tail
//   end-checksum (8 bytes, xxhash with EGTB_CHECKSUM_INIT_VALUE)
//
// Per-block uncompressed payload (LZMA-compressed before write):
//   uint32 num_positions
//   uint32 num_nontrivial
//   uint8  T_bitmap[ceil(num_positions/8)]    bit i ⇒ position i has ≥1 transition
//   uint8  trivial_ranks[(num_positions - num_nontrivial) * entry_bytes]
//   uint32 nontrivial_dir[num_nontrivial + 1]  cumulative byte offsets
//   uint8  nontrivial_data[...]
//     per non-trivial position:
//       uint8  change_count           (1..HMC_COUNT)
//       uint8  changepoint_bitmap[16] (100 bits used)
//       uint8  value_ranks[change_count * entry_bytes]
// =============================================================================

// Uniform block stride so probe's block lookup is `pos / RS_BLOCK_POSITIONS`.
// 1 << 19 matches DTC/DTM's per-block scale (1 MB raw at 2 B/pos) so the
// LZMA window and cold-decompress latency are comparable.
constexpr uint32_t RS_BLOCK_POSITIONS = 1u << 19;

struct Color_Ranks_RS
{
	static constexpr size_t LUT_SIZE = 2048;
	static constexpr uint16_t NO_RANK = 0xFFFFu;
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

// ILLEGAL positions emit the most recent legal rank into trivial_ranks so
// LZMA sees long same-rank runs spanning illegal/legal stretches.
struct RS_Block_Builder
{
	const Color_Ranks_RS* ranks = nullptr;
	std::vector<uint8_t> out;

	void build(const std::vector<uint16_t>& values, size_t num_positions)
	{
		ASSERT(ranks != nullptr);
		const uint8_t eb = ranks->entry_bytes;
		const size_t tbm_bytes = (num_positions + 7) / 8;
		std::vector<uint8_t> T_bitmap(tbm_bytes, 0);
		std::vector<uint8_t> trivial_ranks;
		std::vector<uint32_t> nontrivial_dir;
		std::vector<uint8_t> nontrivial_data;
		nontrivial_dir.push_back(0);

		std::array<uint16_t, DTM50_HMC_COUNT> changepoints{};
		std::array<uint16_t, DTM50_HMC_COUNT> change_ranks{};

		uint16_t last_legal_rank = 0;

		for (size_t i = 0; i < num_positions; ++i)
		{
			bool is_illegal = false;
			for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			{
				if (values[h * num_positions + i] == DTM_Final_Entry::ILLEGAL_VAL)
				{
					is_illegal = true;
					break;
				}
			}
			if (is_illegal)
			{
				write_rank_bytes(trivial_ranks, last_legal_rank, eb);
				continue;
			}

			size_t k = 0;
			uint16_t prev = ranks->rank_of(values[0 * num_positions + i]);
			changepoints[k] = 0;
			change_ranks[k] = prev;
			++k;
			for (size_t h = 1; h < DTM50_HMC_COUNT; ++h)
			{
				const uint16_t r = ranks->rank_of(values[h * num_positions + i]);
				if (r != prev)
				{
					changepoints[k] = static_cast<uint16_t>(h);
					change_ranks[k] = r;
					prev = r;
					++k;
				}
			}

			if (k == 1)
			{
				last_legal_rank = change_ranks[0];
				write_rank_bytes(trivial_ranks, last_legal_rank, eb);
				continue;
			}

			T_bitmap[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
			const size_t base = nontrivial_data.size();
			nontrivial_data.push_back(static_cast<uint8_t>(k));
			uint8_t bm[16] = { 0 };
			for (size_t j = 0; j < k; ++j)
			{
				const uint16_t h = changepoints[j];
				bm[h / 8] |= static_cast<uint8_t>(1u << (h % 8));
			}
			nontrivial_data.insert(nontrivial_data.end(), bm, bm + 16);
			for (size_t j = 0; j < k; ++j)
				write_rank_bytes(nontrivial_data, change_ranks[j], eb);
			nontrivial_dir.push_back(static_cast<uint32_t>(nontrivial_data.size() - base));
		}

		for (size_t i = 1; i < nontrivial_dir.size(); ++i)
			nontrivial_dir[i] += nontrivial_dir[i - 1];

		const uint32_t num_nontrivial = static_cast<uint32_t>(nontrivial_dir.size() - 1);
		const uint32_t num_trivial = static_cast<uint32_t>(num_positions) - num_nontrivial;

		const size_t total =
			8
			+ tbm_bytes
			+ static_cast<size_t>(num_trivial) * eb
			+ nontrivial_dir.size() * 4
			+ nontrivial_data.size();
		out.assign(total, 0);
		size_t off = 0;
		const uint32_t np32 = static_cast<uint32_t>(num_positions);
		std::memcpy(out.data() + off, &np32, 4); off += 4;
		std::memcpy(out.data() + off, &num_nontrivial, 4); off += 4;
		std::memcpy(out.data() + off, T_bitmap.data(), tbm_bytes); off += tbm_bytes;
		if (!trivial_ranks.empty())
		{
			std::memcpy(out.data() + off, trivial_ranks.data(), trivial_ranks.size());
			off += trivial_ranks.size();
		}
		std::memcpy(out.data() + off, nontrivial_dir.data(), nontrivial_dir.size() * 4);
		off += nontrivial_dir.size() * 4;
		if (!nontrivial_data.empty())
		{
			std::memcpy(out.data() + off, nontrivial_data.data(), nontrivial_data.size());
			off += nontrivial_data.size();
		}
		ASSERT(off == total);
	}
};

struct RS_Color_Output
{
	bool is_singular = false;
	uint8_t singular_wdl = 0;
	Color_Ranks_RS ranks;
	uint32_t block_positions = 0;
	uint32_t block_cnt = 0;
	uint32_t tail_positions = 0;
	// Each block stores (dso, usz) on disk. usz is needed because plain LZMA
	// has no end-marker and the decoder must know the exact output size.
	std::vector<uint64_t> offsets;   // (doff << 20) | dsz, one per block
	std::vector<uint64_t> usizes;
	std::vector<uint8_t> data;
};

}  // namespace

void DTM50_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_save_start = std::chrono::steady_clock::now();
	const auto colors = table_colors();
	const size_t num_positions = m_epsi.num_positions();
	if (num_positions == 0) return;

	m_epsi.prepare_orbit_weight_table();
	m_info.clear();

	const size_t ng = m_table->m_dtm[WHITE][0].num_groups();
	const size_t spg = m_table->m_dtm[WHITE][0].slices_per_group();
	const size_t wss = m_table->m_dtm[WHITE][0].within_slice_size();
	const size_t positions_per_group = spg * wss;

	// One cache across all 2 × HMC_COUNT layer tables shares the paging
	// budget; cap_groups = SIZE_MAX when unbounded keeps everything resident.
	const size_t group_bytes = positions_per_group * sizeof(DTM_Final_Entry);
	const size_t cap_groups = (m_paging_budget_bytes == 0 || group_bytes == 0)
		? std::numeric_limits<size_t>::max()
		: std::max<size_t>(1, m_paging_budget_bytes / group_bytes);

	std::vector<Sliced_EGTB_File_For_Gen<DTM_Final_Entry>*> all_tables;
	all_tables.reserve(COLOR_NB * DTM50_HMC_COUNT);
	for (Color c : { WHITE, BLACK })
		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			all_tables.push_back(&m_table->m_dtm[c][h]);
	Save_Group_Cache<DTM_Final_Entry> cache(std::move(all_tables), cap_groups);

	auto table_idx_of = [](Color c, size_t h) {
		return static_cast<size_t>(c) * DTM50_HMC_COUNT + h;
	};

	auto parallel_for_layers = [&](size_t h_lo, size_t h_hi, auto&& body) {
		const size_t n = h_hi - h_lo;
		if (n == 0) return;
		const size_t workers = std::min(thread_pool->num_workers(), n);
		std::atomic<size_t> next(h_lo);
		thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
			for (;;)
			{
				const size_t h = next.fetch_add(1, std::memory_order_relaxed);
				if (h >= h_hi) return;
				body(h);
			}
		});
	};

	RS_Color_Output color_out[COLOR_NB]{};

	for (Color me : colors)
	{
		// 2× since pre-pass and main-pass each read every (position, layer) cell.
		const size_t color_units = 2u * num_positions * DTM50_HMC_COUNT;
		const size_t print_period = thread_pool->num_workers() * (1u << 20);
		Concurrent_Progress_Bar progress_bar(color_units, print_period,
			std::string("save_to_disk ") + std::to_string(static_cast<int>(me)));

		// Pre-pass: union all layers' distinct halved values into a per-color
		// rank table, scored by how many layers each value appears in.
		std::array<uint64_t, Color_Ranks_RS::LUT_SIZE> score{};
		std::array<std::array<uint8_t, Color_Ranks_RS::LUT_SIZE>, DTM50_HMC_COUNT> per_layer_seen{};

		parallel_for_layers(0, DTM50_HMC_COUNT, [&](size_t h) {
			const size_t ti = table_idx_of(me, h);
			auto& tbl = m_table->m_dtm[me][h];
			auto& seen = per_layer_seen[h];
			// Only h=0 gathers .info — it's the canonical fresh-50MR view;
			// k>0 layers are intermediate states a probe never enters cold.
			const bool gather_info = (h == 0);
			for (size_t g = 0; g < ng; ++g)
			{
				Pinned_Group_Range<DTM_Final_Entry> pin(cache, ti, g, g);
				const size_t p_lo = g * positions_per_group;
				const size_t p_hi = std::min(p_lo + positions_per_group, num_positions);
				Decomposed_Board_Index didx{};
				if (gather_info)
					m_epsi.decompose_board_index(static_cast<Board_Index>(p_lo), out_param(didx));
				for (size_t p = p_lo; p < p_hi; ++p)
				{
					const DTM_Final_Entry e = tbl.read(static_cast<Board_Index>(p));
					const uint16_t v = dtm_value_for_storage(e);
					if (v != DTM_Final_Entry::ILLEGAL_VAL) seen[v] = 1;
					if (gather_info)
					{
						const uint64_t w = m_epsi.orbit_weight(didx);
						m_info.add_result(me, e.wdl(), w);
						if (e.is_win())
							m_info.maybe_update_longest_win(me, p, e.value());
						m_epsi.step_to_next(inout_param(didx));
					}
				}
				progress_bar += (p_hi - p_lo);
			}
		});

		// Values present in more layers get smaller ranks (LZMA bias).
		for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			for (size_t v = 0; v < Color_Ranks_RS::LUT_SIZE; ++v)
				if (per_layer_seen[h][v]) ++score[v];

		std::vector<uint16_t> values;
		values.reserve(64);
		for (size_t v = 0; v < Color_Ranks_RS::LUT_SIZE; ++v)
			if (score[v] != 0) values.push_back(static_cast<uint16_t>(v));

		// All-ILLEGAL across every layer — degenerate but possible.
		if (values.empty())
		{
			color_out[me].is_singular = true;
			color_out[me].singular_wdl = static_cast<uint8_t>(WDL_Entry::DRAW);
			progress_bar.set_finished();
			continue;
		}

		std::sort(values.begin(), values.end(),
			[&](uint16_t a, uint16_t b) {
				if (score[a] != score[b]) return score[a] > score[b];
				return a < b;
			});

		auto& ranks = color_out[me].ranks;
		ranks.reset();
		ranks.rank_to_value = std::move(values);
		ranks.entry_bytes = (ranks.rank_to_value.size() <= 256) ? 1 : 2;
		for (size_t i = 0; i < ranks.rank_to_value.size(); ++i)
			ranks.value_to_rank[ranks.rank_to_value[i]] = static_cast<uint16_t>(i);

		// Main pass: fill the [layer × position] matrix one block at a time,
		// pack it, LZMA the result. The shared cache handles cross-block
		// reuse vs spill round-trip under tight budgets transparently.
		const size_t bcnt = (num_positions + RS_BLOCK_POSITIONS - 1) / RS_BLOCK_POSITIONS;
		color_out[me].block_positions = RS_BLOCK_POSITIONS;
		color_out[me].block_cnt = static_cast<uint32_t>(bcnt);
		const size_t tail = num_positions - (bcnt - 1) * RS_BLOCK_POSITIONS;
		color_out[me].tail_positions =
			(tail == RS_BLOCK_POSITIONS) ? 0u : static_cast<uint32_t>(tail);
		color_out[me].offsets.assign(bcnt, 0);
		color_out[me].usizes.assign(bcnt, 0);

		auto group_id_of_pos = [&](size_t p) {
			return positions_per_group == 0 ? size_t{ 0 } : p / positions_per_group;
		};

		const size_t batch_size = std::max<size_t>(thread_pool->num_workers(), 1);

		std::vector<uint16_t> chunk(static_cast<size_t>(RS_BLOCK_POSITIONS) * DTM50_HMC_COUNT);
		RS_Block_Builder builder;
		builder.ranks = &color_out[me].ranks;

		std::vector<std::vector<uint8_t>> raw_payloads(batch_size);
		std::vector<std::vector<uint8_t>> compressed(batch_size);

		for (size_t batch_start = 0; batch_start < bcnt; batch_start += batch_size)
		{
			const size_t batch_end = std::min(batch_start + batch_size, bcnt);
			const size_t batch_n = batch_end - batch_start;

			for (size_t i = 0; i < batch_n; ++i)
			{
				const size_t b = batch_start + i;
				const size_t p_base = b * RS_BLOCK_POSITIONS;
				const size_t this_bp =
					(b == bcnt - 1 && color_out[me].tail_positions != 0)
					? color_out[me].tail_positions : RS_BLOCK_POSITIONS;

				const size_t want_lo = group_id_of_pos(p_base);
				const size_t want_hi = group_id_of_pos(p_base + this_bp - 1);

				parallel_for_layers(0, DTM50_HMC_COUNT, [&](size_t h) {
					Pinned_Group_Range<DTM_Final_Entry> pin(
						cache, table_idx_of(me, h), want_lo, want_hi);
					auto& tbl = m_table->m_dtm[me][h];
					uint16_t* row = chunk.data() + h * this_bp;
					for (size_t k = 0; k < this_bp; ++k)
					{
						const DTM_Final_Entry e = tbl.read(static_cast<Board_Index>(p_base + k));
						row[k] = dtm_value_for_storage(e);
					}
					progress_bar += this_bp;
				});

				builder.build(chunk, this_bp);
				raw_payloads[i] = std::move(builder.out);
			}

			std::atomic<size_t> next(0);
			thread_pool->run_sync_task_on_all_threads([&](size_t) {
				LZMA_Compress_Helper local;
				for (;;)
				{
					const size_t i = next.fetch_add(1, std::memory_order_relaxed);
					if (i >= batch_n) return;
					compressed[i] = local.compress(
						Const_Span<uint8_t>(raw_payloads[i].data(), raw_payloads[i].size()));
				}
			});

			// Append in block order so file offsets stay deterministic.
			for (size_t i = 0; i < batch_n; ++i)
			{
				const size_t b = batch_start + i;
				const size_t doff = color_out[me].data.size();
				const size_t dsz = compressed[i].size();
				if (dsz >= (1u << 20))
					throw std::runtime_error("rs block too large for offset encoding");
				color_out[me].offsets[b] =
					(static_cast<uint64_t>(doff) << 20) | static_cast<uint64_t>(dsz);
				color_out[me].usizes[b] = raw_payloads[i].size();
				color_out[me].data.insert(color_out[me].data.end(),
					compressed[i].begin(), compressed[i].end());
				raw_payloads[i].clear();
				compressed[i].clear();
			}
		}

		progress_bar.set_finished();
	}

	const auto out_path = paths.dtm50_save_path(m_epsi);
	std::filesystem::create_directories(out_path.parent_path());

	size_t header_bytes = 4 + 4;  // magic + key_and_table_num
	const uint32_t key_and_table_num =
		(m_epsi.min_material_key().value() << 2) | static_cast<uint32_t>(colors.size());
	for (Color c : colors)
	{
		header_bytes += 1;  // flag
		if (color_out[c].is_singular) header_bytes += 1;
		else {
			header_bytes += 1 + 4 + 4 + 4 + 8 + 2;
			header_bytes += color_out[c].ranks.rank_to_value.size() * 2;
		}
	}
	size_t out_size = header_bytes;
	for (Color c : colors)
		if (!color_out[c].is_singular)
			out_size += color_out[c].block_cnt * 16;  // dso + usz per block
	out_size = ceil_to_multiple(out_size, size_t{ 64 });
	for (Color c : colors)
	{
		if (color_out[c].is_singular) continue;
		out_size += color_out[c].data.size();
		out_size = ceil_to_multiple(out_size, size_t{ 64 });
	}

	const auto tmp = out_path.string() + ".tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
		throw std::runtime_error("Failed to create " + tmp);
	Serial_Memory_Writer w(out.data_span());

	w.write<uint32_t>(narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC));
	w.write<uint32_t>(key_and_table_num);

	for (Color c : colors)
	{
		const RS_Color_Output& co = color_out[c];
		if (co.is_singular)
		{
			w.write<uint8_t>(EGTB_SINGULAR_FLAG);
			w.write<uint8_t>(co.singular_wdl);
		}
		else
		{
			w.write<uint8_t>(0);
			w.write<uint8_t>(co.ranks.entry_bytes);
			w.write<uint32_t>(co.block_positions);
			w.write<uint32_t>(co.block_cnt);
			w.write<uint32_t>(co.tail_positions);
			w.write<uint64_t>(static_cast<uint64_t>(co.data.size()));
			const uint16_t nr = static_cast<uint16_t>(co.ranks.rank_to_value.size());
			w.write<uint16_t>(nr);
			for (uint16_t v : co.ranks.rank_to_value) w.write<uint16_t>(v);
		}
	}
	for (Color c : colors)
	{
		const RS_Color_Output& co = color_out[c];
		if (co.is_singular) continue;
		ASSERT(co.offsets.size() == co.usizes.size());
		for (size_t b = 0; b < co.offsets.size(); ++b)
		{
			w.write<uint64_t>(co.offsets[b]);
			w.write<uint64_t>(co.usizes[b]);
		}
	}
	w.zero_align(64);
	for (Color c : colors)
	{
		const RS_Color_Output& co = color_out[c];
		if (co.is_singular) continue;
		w.write(Const_Span<uint8_t>(co.data.data(), co.data.size()));
		w.zero_align(64);
	}
	if (w.num_bytes_written() != out_size)
		throw std::runtime_error("rs file size mismatch");
	w.write_end_checksum(EGTB_CHECKSUM_INIT_VALUE);
	out.close();

	std::error_code ec;
	std::filesystem::rename(tmp, out_path, ec);
	if (ec) throw std::runtime_error("rename failed: " + ec.message());

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

	for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
	{
		m_table->m_dtm[WHITE][h].remove_disk_files();
		m_table->m_dtm[BLACK][h].remove_disk_files();
		m_table->m_dtm[WHITE][h].close();
		m_table->m_dtm[BLACK][h].close();
	}

	remove_checkpoint(paths.dtm50_checkpoint_path(m_epsi));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
