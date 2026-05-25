#include "egtb/egtb_gen_dtm50.h"
#include "egtb/egtb_compress.h"
#include "egtb/slice_storage.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/attack.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/math.h"
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

	// Budget is per-layer: paging operates on a single active layer at a time.
	// Previously-built layers within the same fusion spill to disk via the k+2
	// evict step inside gen().
	const size_t bytes_per_color =
		m_table->m_dtm[WHITE][0].num_slices()
		* m_table->m_dtm[WHITE][0].within_slice_size()
		* sizeof(DTM_Final_Entry);
	const size_t total_bytes = bytes_per_color * 2;
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
	std::printf("  gen (%zu hmc layers): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

// =============================================================================
// save_to_disk: gather DTM info, build rank table, LZMA-compress, write .lzdtm.
// =============================================================================

namespace {

struct DTM_Singular_Probe_Result {
	WDL_Entry singular;
	uint64_t  legal_cnt;
	uint64_t  illegal_cnt;
};

using DTM_Save_Cache = Save_Group_Cache<DTM_Final_Entry>;
using DTM_Pinned_Range = Pinned_Group_Range<DTM_Final_Entry>;

NODISCARD DTM_Singular_Probe_Result dtm_singular_probe(
	const Piece_Config_For_Gen& epsi,
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry>& src,
	DTM_Save_Cache& cache,
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
		DTM_Pinned_Range pin(cache, color, g, g);
		const size_t s_begin = g * spg;
		const size_t s_end   = std::min(s_begin + spg, ns);
		for (size_t s = s_begin; s < s_end; ++s)
		{
			const size_t base = s * within;
			if (base >= num_positions) break;
			const size_t end_in_slice = std::min(within, num_positions - base);
			const auto* const raw = src.slice_data(s);
			if (!didx_init)
			{
				epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
				didx_init = true;
			}
			for (size_t i = 0; i < end_in_slice; ++i)
			{
				DTM_Final_Entry e;
				std::memcpy(&e, &raw[i], sizeof(e));
				const uint64_t w = epsi.orbit_weight(didx);
				switch (e.wdl())
				{
					case WDL_Entry::WIN:  saw_win = true;  legal   += w; break;
					case WDL_Entry::DRAW: saw_draw = true; legal   += w; break;
					case WDL_Entry::LOSE: saw_lose = true; legal   += w; break;
					case WDL_Entry::ILLEGAL: illegal += w; break;
					default: return {WDL_Entry::ILLEGAL, 0, 0};
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

void gather_dtm_info(
	const Piece_Config_For_Gen& epsi,
	DTM_Save_Cache& cache,
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry>& src,
	Color color,
	size_t num_positions,
	EGTB_Info& info,
	Value_Histogram& hist)
{
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t ng = src.num_groups();
	const size_t ns = src.num_slices();
	Decomposed_Board_Index didx{};
	bool didx_init = false;
	for (size_t g = 0; g < ng; ++g)
	{
		cache.acquire(color, g);
		const size_t s_begin = g * spg;
		const size_t s_end   = std::min(s_begin + spg, ns);
		for (size_t s = s_begin; s < s_end; ++s)
		{
			const size_t base = s * within;
			const size_t end  = std::min(base + within, num_positions);
			if (!didx_init)
			{
				epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
				didx_init = true;
			}
			for (size_t idx = base; idx < end; ++idx)
			{
				const DTM_Final_Entry e = src.read(static_cast<Board_Index>(idx));
				const uint64_t w = epsi.orbit_weight(didx);
				info.add_result(color, e.wdl(), w);
				if (e.is_win())
					info.maybe_update_longest_win(color, idx, e.value());
				if (!e.is_illegal())
				{
					// DTM halves storage in both tiers (parity invariant), so a
					// single histogram over halved values suffices. hist_2b is
					// unused for DTM and stays zero-initialized.
					const uint16_t v = dtm_value_for_storage(e);
					if (v < Value_Histogram::HIST_BINS) ++hist.hist_1b[v];
				}
				epsi.step_to_next(inout_param(didx));
			}
		}
		cache.release(color, g);
	}
}

Block_Source make_dtm_block_source(
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry>& src,
	DTM_Save_Cache& cache,
	Color color,
	size_t block_size,
	size_t entry_bytes)
{
	constexpr size_t kEntry = sizeof(DTM_Final_Entry);
	ASSERT(block_size % entry_bytes == 0);
	const size_t source_block_bytes = block_size * kEntry / entry_bytes;
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_entries = src.num_slices() * within;
	const size_t source_total_bytes = total_entries * kEntry;
	const size_t output_total_bytes = total_entries * entry_bytes;
	return Block_Source{
		output_total_bytes,
		[&src, &cache, color, within, spg, source_block_bytes, source_total_bytes](size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t> {
			const size_t block_off = block_id * source_block_bytes;
			const size_t this_block = std::min(source_block_bytes, source_total_bytes - block_off);
			ASSERT(scratch.size() >= this_block);
			ASSERT(block_off % kEntry == 0);
			ASSERT(this_block % kEntry == 0);

			const size_t entry_off = block_off / kEntry;
			const size_t entry_cnt = this_block / kEntry;

			const size_t first_g = (entry_off / within) / spg;
			const size_t last_g  = (entry_cnt == 0 ? first_g
			                                       : ((entry_off + entry_cnt - 1) / within) / spg);
			DTM_Pinned_Range pin(cache, color, first_g, last_g);

			size_t done = 0;
			while (done < entry_cnt)
			{
				const size_t cur = entry_off + done;
				const size_t s = cur / within;
				const size_t in_slice = cur - s * within;
				const size_t take = std::min(entry_cnt - done, within - in_slice);
				const auto* const raw = src.slice_data(s) + in_slice;
				std::memcpy(scratch.data() + done * kEntry, raw, take * kEntry);
				done += take;
			}

			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

}  // namespace

void DTM50_Generator::save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto colors = table_colors();
	const auto t_save_start = std::chrono::steady_clock::now();

	m_epsi.prepare_orbit_weight_table();

	const size_t bytes_per_group = m_table->m_dtm[WHITE][0].slices_per_group()
		* m_table->m_dtm[WHITE][0].within_slice_size() * sizeof(DTM_Final_Entry);
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

	static constexpr size_t DTM_BLOCK_SIZE = 1024 * 1024;
	const size_t num_positions = m_epsi.num_positions();

	// Per-material subfolder dtm50/<name>/ for the layer files.
	std::filesystem::create_directories(paths.dtm50_save_path(m_epsi, 1).parent_path());

	for (uint16_t hmc = 0; hmc < DTM50_HMC_COUNT; ++hmc)
	{
		std::printf("  save layer %2u\r", hmc); std::fflush(stdout);

		auto& dtm_w = m_table->m_dtm[WHITE][hmc];
		auto& dtm_b = m_table->m_dtm[BLACK][hmc];

		m_info.clear();

		Compressed_EGTB dtm_save[COLOR_NB];
		Value_Histogram dtm_hist[COLOR_NB];

		DTM_Save_Cache cache(&dtm_w, &dtm_b, cap_groups);

		for (Color me : colors)
		{
			auto& dtm_me = m_table->m_dtm[me][hmc];
			const auto probe = dtm_singular_probe(m_epsi, dtm_me, cache, me, num_positions);
			if (probe.singular == WDL_Entry::DRAW)
			{
				m_info.draw_cnt[me]    = probe.legal_cnt;
				m_info.illegal_cnt[me] = probe.illegal_cnt;
				dtm_save[me] = Compressed_EGTB::make_singular(WDL_Entry::DRAW);
			}
			else
			{
				gather_dtm_info(m_epsi, cache, dtm_me, me, num_positions, m_info, dtm_hist[me]);
			}

			if (m_info.longest_win[me] > 0)
			{
				Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
				pos_gen.board().to_fen(Span(m_info.longest_fen[me]));
			}
		}

		Value_Rank_Table dtm_rank[COLOR_NB];
		size_t dtm_entry_bytes[COLOR_NB]{};
		for (Color me : colors)
		{
			dtm_rank[me] = Value_Rank_Table::build_1b(dtm_hist[me].hist_1b);
			dtm_entry_bytes[me] = (dtm_rank[me].ranks.size() <= 256) ? 1 : 2;
		}

		for (Color me : colors)
		{
			if (dtm_save[me].is_singular()) continue;
			Block_Source src = make_dtm_block_source(m_table->m_dtm[me][hmc], cache, me, DTM_BLOCK_SIZE, dtm_entry_bytes[me]);
			dtm_save[me] = save_compress_egtb(
				thread_pool, src, me, m_info, dtm_entry_bytes[me], DTM_BLOCK_SIZE, max_workers,
				dtm_rank[me], &dtm_storage_fn, /*silent=*/true);
		}

		save_egtb_table(m_epsi, dtm_save, paths.dtm50_save_path(m_epsi, hmc), colors, EGTB_Magic::DTM50_MAGIC);

		std::ofstream fp(paths.dtm50_info_save_path(m_epsi, hmc), std::ios::binary | std::ios::trunc);
		fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));
	}

	remove_checkpoint(paths.dtm50_checkpoint_path(m_epsi));
	for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
	{
		m_table->m_dtm[WHITE][h].remove_disk_files();
		m_table->m_dtm[BLACK][h].remove_disk_files();
	}

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
