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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>

namespace {

struct Checkpoint_File
{
	static constexpr uint64_t MAGIC = 0x30354D44544D4843ull;  // 'CHMTDM50'
	static constexpr uint32_t VERSION = 1;
	uint64_t magic = MAGIC;
	uint32_t version = VERSION;
	uint32_t batch_idx = 0;
	uint32_t fusion_idx = 0;
	uint16_t finished_ply = 0;
	uint16_t max_dtm = 0;
	uint16_t hmc = 0;
	uint8_t  _pad[6] = { 0, 0, 0, 0, 0, 0 };
};
static_assert(sizeof(Checkpoint_File) == 32, "DTM50 Checkpoint_File size");

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
	// Resident overhead for already-built layers is handled separately by the
	// layer build loop (evict-to-disk after each layer finishes).
	const size_t bytes_per_color =
		m_table->m_dtm[WHITE][0].num_slices()
		* m_table->m_dtm[WHITE][0].within_slice_size()
		* sizeof(DTM_Final_Entry);
	const size_t total_bytes = bytes_per_color * 2;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_group_state(
		m_table->m_dtm[WHITE][0].num_groups(),
		m_table->m_dtm[WHITE][0].num_entries());

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

	// Pawn push (zeroing) → opp hmc=0. Non-pawn quiet:
	//   hmc=0 build: self-ref opp hmc=0 (phase tape tracks the chain).
	//   hmc=k>0:    opp hmc=k+1; k=99 has no k+1 so the move targets virtual
	//               hmc=100 (50MR draw unless the move itself is mate).
	const bool is_pawn_push = piece_type(parent.piece_at(move.from())) == PAWN;
	if (is_pawn_push)
		return m_table->m_dtm[opp][0].read(post_idx);

	if (m_current_hmc == 0)
		return m_table->m_dtm[opp][0].read(post_idx);

	const uint16_t target_hmc = static_cast<uint16_t>(m_current_hmc + 1);
	if (target_hmc >= DTM50_HMC_COUNT)
	{
		// hmc=99 quiet → virtual hmc=100; 50MR draws unless the move is mate
		// (mate ends the game before 50MR fires).
		Position child = parent;
		(void)child.do_move(move);
		if (!child.is_in_check(child.turn())) return DTM_Final_Entry::make_draw();
		Move_List ml;
		child.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
		for (size_t i = 0; i < ml.size(); ++i)
			if (child.is_pseudo_legal_move_legal(ml[i])) return DTM_Final_Entry::make_draw();
		return DTM_Final_Entry::make_loss(0);
	}
	return m_table->m_dtm[opp][target_hmc].read(post_idx);
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

DTM_Any_Entry DTM50_Generator::make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id,
                                                Out_Param<uint16_t> worst_loss_dtm) const
{
	*worst_loss_dtm = 0;
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM_Final_Entry::make_illegal();

	Position& pos = pos_gen.board();
	const bool in_check = pos.is_in_check(pos.turn());
	// hmc=0: in-M children share this layer (unclassified during init), so seed
	// only from cap/promo and let retro fill the rest.
	// hmc>0: every child lives in an already-finalized layer; classify in one shot.
	const bool layered_reads = (m_current_hmc != 0);

	uint16_t best_win_dtm  = std::numeric_limits<uint16_t>::max();
	uint16_t best_loss_dtm = 0;
	bool saw_win  = false;
	bool saw_draw = false;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	bool any_legal = false;
	bool any_in_material = false;
	bool any_pawn_eval = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;
		const bool is_cap = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_in_material = !is_cap && !m.is_promotion();
		const bool is_pawn = piece_type(pos.piece_at(m.from())) == PAWN;

		if (is_in_material)
		{
			any_in_material = true;
			if (is_pawn) any_pawn_eval = true;
			if (!layered_reads) continue;
		}
		else
		{
			any_pawn_eval = true;
		}

		// Resolve the child's value via the layer-aware dispatch.
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
			update_max(*worst_loss_dtm, cand);
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
		return DTM_Intermediate_Entry{};
	}

	if (saw_win)
		return DTM_Final_Entry::make_win(best_win_dtm);

	if (layered_reads)
	{
		// All children resolved: any DRAW → DRAW, else LOSS at the max child dtm.
		if (saw_draw) return DTM_Final_Entry::make_draw();
		if (best_loss_dtm > 0) return DTM_Final_Entry::make_loss(best_loss_dtm);
		return DTM_Final_Entry::make_draw();
	}

	if (!any_in_material && !saw_draw)
	{
		if (*worst_loss_dtm > 0)
			return DTM_Final_Entry::make_loss(*worst_loss_dtm);
	}
	return any_pawn_eval
		? DTM_Intermediate_Entry::make_pawn_eval()
		: DTM_Intermediate_Entry{};
}

uint16_t DTM50_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
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

	// Init working set: both colors at g, plus (push_target_pid, same_kid) for
	// the read_post_move_dtm path. Init writes both colors, so needs[B] == needs[W].
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
				for (int32_t tpid : targets_by_pid[static_cast<size_t>(pid)])
				{
					const size_t target_slice =
						static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
					m_scratch_need[WHITE][target_slice / spg] = 1;
				}
			}
		}
		m_scratch_need[BLACK] = m_scratch_need[WHITE];
		apply_working_set(thread_pool, &cur_layer(WHITE), &cur_layer(BLACK), m_scratch_need[WHITE], m_scratch_need[BLACK]);
		// Layer-k init also reads opp hmc=0 / hmc=k+1 at the same indices.
		if (m_current_hmc != 0)
		{
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
		}
		else
		{
			// Layer-0 init writes phase=0 seeds — page the phase slices in.
			apply_working_set(thread_pool,
				&m_table->m_phase[WHITE], &m_table->m_phase[BLACK],
				m_scratch_need[WHITE], m_scratch_need[BLACK]);
		}
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

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t tid) -> uint16_t {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			uint16_t local_max = 0;
			size_t local_progress = 0;
			const auto& slice_has_stab = m_epsi.slice_manager().slice_has_stabilizer;
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
					// Chess-legality is hmc-invariant; reuse layer-0's ILLEGAL flag.
					if (m_current_hmc != 0
					    && m_table->m_dtm[us][0].read(idx).is_illegal())
					{
						write_dtm(idx, us, DTM_Final_Entry::make_illegal());
						write_phase(idx, us, 0);
						continue;
					}
					uint16_t worst_loss_dtm = 0;
					std::visit(overload{
						[&](DTM_Final_Entry entry) {
							write_dtm(idx, us, entry);
							// Seeds are zeroing or terminal → phase=0.
							write_phase(idx, us, 0);
							if (entry.is_win() || entry.is_loss())
							{
								const uint16_t v = static_cast<uint16_t>(entry.value());
								update_max(local_max, v);
							}
						},
						[&](DTM_Intermediate_Entry entry) {
							write_dtm(idx, us, entry);
							write_phase(idx, us, 0);
							update_max(local_max, worst_loss_dtm);
						},
					}, make_initial_entry(pos_gen, tid, out_param(worst_loss_dtm)));
				}
			}
			return local_max;
		});
		for (uint16_t v : rets) update_max(max_init, v);
	}

	progress_bar.set_finished();

	return max_init;
}

// =============================================================================
// Retrograde sweep.
// =============================================================================

DTM50_Generator::Iter_Action DTM50_Generator::action_for_entry(DTM_Final_Entry e, uint16_t ply) const
{
	if (e.is_illegal()) return Iter_Action::SKIP;

	const bool has_pawns = m_epsi.pawn_slice_manager().has_pawns();

	if (e.is_draw())
	{
		if (e.has_change()) return Iter_Action::CHANGE_REVERIFY;
		// PAWN_EVAL: forward-read for retro-blind edges (cap/promo + pawn push).
		if (ply > 0 && e.has_pawn_eval()) return Iter_Action::PAWN_EVAL;
		return Iter_Action::SKIP;
	}

	if (ply == 0)
	{
		if (e.is_loss() && e.value() == 0) return Iter_Action::MARK_WIN_IN_1;
		return Iter_Action::SKIP;
	}

	// WIN(ply) and WIN(ply-1) both fire MARK_CHANGED so predecessors' check_loss
	// finds a fully-classified child. The two plies cover same-ply (e.g. just
	// overwritten by retro at this ply) and prior-ply settlings. ply >= 1 here
	// since the ply == 0 branch returned above.
	if (e.is_win() && (e.value() == ply || e.value() == ply - 1))
		return Iter_Action::MARK_CHANGED;

	if (e.is_loss() && e.value() == ply)
		return Iter_Action::MARK_WIN_PREDS;

	// Pawnful stale WIN: init may have seeded WIN(value > ply) from cap/promo.
	// A faster mate via in-material pawn pushes can overwrite this. Same handler
	// as PAWN_EVAL — it just won't fall through to check_loss for non-draw e.
	if (has_pawns && e.is_win() && e.value() > ply)
		return Iter_Action::PAWN_EVAL;

	return Iter_Action::SKIP;
}

DTM50_Generator::Loss_Verification_Result DTM50_Generator::check_loss(
	Position_For_Gen& pos_gen,
	Move_List& ml,
	uint16_t ply, size_t thread_id) const
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp_color = color_opp(pos.turn());
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	Loss_Verification_Result r;
	bool any_legal = false;
	uint16_t max_contribution = 0;
	uint8_t  worst_phase = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const bool is_cap_or_ep = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo     = m.is_promotion();
		const bool is_pawn_move = piece_type(pos.piece_at(m.from())) == PAWN;

		uint16_t contribution;
		uint8_t  move_phase;
		if (is_cap_or_ep || is_promo)
		{
			// Sub-TB (zeroing) — phase resets.
			const DTM_Final_Entry sub_e = read_sub_tb(pos_gen, m, thread_id);
			if (!sub_e.is_win()) return r;
			contribution = static_cast<uint16_t>(sub_e.value() + 1);
			move_phase   = 0;
		}
		else if (is_pawn_move && is_pawn_double_push(m))
		{
			// Double push (zeroing) — phase resets.
			const DTM_Final_Entry opp_e = effective_opp_dtm_after_dp(pos_gen, m, thread_id);
			if (!opp_e.is_win()) return r;
			if (m_current_hmc == 0 && opp_e.value() >= ply) return r;
			contribution = static_cast<uint16_t>(opp_e.value() + 1);
			move_phase   = 0;
		}
		else
		{
			// Quiet in-M. Pawn push zeros; non-pawn extends the chain.
			const DTM_Final_Entry ce = read_post_move_dtm(pos_gen, m, thread_id);
			if (!ce.is_win()) return r;
			if (m_current_hmc == 0 && ce.value() >= ply) return r;
			contribution = static_cast<uint16_t>(ce.value() + 1);
			if (is_pawn_move) move_phase = 0;
			else {
				const Board_Index child_idx = next_quiet_index(pos_gen, m);
				const uint8_t child_phase = (child_idx == BOARD_INDEX_NONE)
					? 0 : read_phase(child_idx, opp_color);
				move_phase = static_cast<uint8_t>(child_phase + 1);
			}
		}
		// phase > 100 means STM can claim a 50MR draw — cell is DRAW, not LOSS.
		if (move_phase > 100) return r;
		if (contribution > max_contribution)
		{
			max_contribution = contribution;
			worst_phase = move_phase;
		}
		else if (contribution == max_contribution && move_phase < worst_phase)
		{
			// Tied max-DTM: keep min phase to leave more 50MR budget upstream.
			worst_phase = move_phase;
		}
	}

	if (!any_legal) return r;
	if (max_contribution != ply) return r;

	r.is_loss = true;
	r.loss_dtm = max_contribution;
	r.loss_phase = worst_phase;
	return r;
}

bool DTM50_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen,
                                        Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<true>(out_param(ml));
	bool wrote = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const DTM_Final_Entry cur = read_dtm(pred, opp);
		if (cur.is_illegal()) continue;
		if (cur.is_loss()) continue;
		if (cur.is_win() && cur.value() <= 1) continue;
		write_dtm(pred, opp, DTM_Final_Entry::make_win(1));
		write_phase(pred, opp, 1);  // 1 quiet ply ending at mate
		wrote = true;
	}
	return wrote;
}

void DTM50_Generator::retro_mark_changed(Position_For_Gen& pos_gen,
                                       Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<true>(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtm(pred, opp);
		if (!e.is_draw() || e.has_change()) continue;
		// Atomic OR: a concurrent retro_mark_wins overwrite to Final clears the
		// flag naturally (we always write fresh class+value).
		cur_layer(opp).lock_add_flags(pred, DTM_FLAG_CHANGE);
		mark_iter(opp, pred, cur_layer(opp));
	}
}

bool DTM50_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    Move_List& ml, Color stm,
                                    uint16_t target_dtm, uint8_t cell_phase)
{
	const Color opp = color_opp(stm);
	// pred extends the chain by one quiet ply: pred_phase = 1 + cell_phase.
	// cell_phase is always ≤ 100 in storage (writes past 100 short-circuit).
	if (cell_phase >= 100) return false;  // pred would bust 50MR
	const uint8_t pred_phase = static_cast<uint8_t>(cell_phase + 1);

	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<true>(out_param(ml));
	const DTM_Final_Entry new_e = DTM_Final_Entry::make_win(target_dtm);
	bool wrote = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const DTM_Final_Entry cur = read_dtm(pred, opp);
		if (cur.is_illegal()) continue;
		if (cur.is_loss()) continue;  // never demote a classified LOSS
		if (cur.is_win() && cur.value() <= target_dtm) continue;
		write_dtm(pred, opp, new_e);
		write_phase(pred, opp, pred_phase);
		wrote = true;
	}
	return wrote;
}

DTM50_Generator::Iter_Result DTM50_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                                                   Color stm, uint16_t ply)
{
	const size_t spg = cur_layer(stm).slices_per_group();
	const auto& pid_in_pair = m_pid_in_pair;
	const bool has_pawns = m_epsi.pawn_slice_manager().has_pawns();

	Iter_Result global;

	for (size_t g : m_pair_group_ids)
	{
		if (m_iter_groups[stm][g] == 0) continue;

		page_in_for_group(thread_pool, stm, g);

		Shared_Board_Index_Iterator cell_it = make_slice_group_iterator(g, spg);

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t tid) -> Iter_Result {
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, stm);
			Board_Index prev = BOARD_INDEX_NONE;
			Move_List ml;
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

					const DTM_Final_Entry e = read_dtm(idx, stm);
					if (e.is_illegal()) continue;

					// Only flagged Intermediates revisit; bare DRAW SKIPs forever.
					if (e.is_draw() && (e.has_change() || e.has_pawn_eval()))
						chunk.any_intermediate = true;
					else
					{
						const uint16_t v = static_cast<uint16_t>(e.value());
						update_max(chunk.max_classified, v);
					}

					const Iter_Action action = action_for_entry(e, ply);
					if (action == Iter_Action::SKIP) continue;

					if (prev != BOARD_INDEX_NONE
					    && static_cast<size_t>(idx) == static_cast<size_t>(prev) + 1)
						++pos_gen;
					else
						pos_gen.set_board_index(idx);
					prev = idx;
					pos_gen.set_turn(stm);

					switch (action)
					{
					case Iter_Action::MARK_WIN_IN_1:
						if (retro_mark_win_in_1(pos_gen, ml, stm)) { local.wrote = true; update_max<uint16_t>(local.max_v, 1); }
						break;
					case Iter_Action::MARK_CHANGED:
						retro_mark_changed(pos_gen, ml, stm);
						local.wrote = true;
						break;
					case Iter_Action::MARK_WIN_PREDS:
					{
						const uint16_t target = static_cast<uint16_t>(ply + 1);
						const uint8_t cell_phase = read_phase(idx, stm);
						if (retro_mark_wins(pos_gen, ml, stm, target, cell_phase)) { local.wrote = true; update_max(local.max_v, target); }
						break;
					}
					case Iter_Action::CHANGE_REVERIFY:
					case Iter_Action::PAWN_EVAL:
					{
						// Forward pawn-WIN: a push reaching opp-LOSS(ply-1) makes
						// this cell WIN(ply); also improves stale WIN(value > ply).
						bool pawn_marked = false;
						if (has_pawns)
						{
							Position& pos = pos_gen.board();
							pos.gen_pseudo_legal_moves<Position::Move_Kind::PAWN_PUSHES>(out_param(ml));
							for (size_t i = 0; i < ml.size(); ++i)
							{
								const Move m = ml[i];
								if (!pos.is_pseudo_legal_move_legal(m)) continue;
								const DTM_Final_Entry opp_e = is_pawn_double_push(m)
									? effective_opp_dtm_after_dp(pos_gen, m, tid)
									: read_post_move_dtm(pos_gen, m, tid);
								if (opp_e.is_loss()
								    && static_cast<uint16_t>(opp_e.value()) + 1 == ply)
								{
									pawn_marked = true;
									break;
								}
							}
							if (pawn_marked)
							{
								write_dtm(idx, stm, DTM_Final_Entry::make_win(ply));
								write_phase(idx, stm, 0);  // pawn push zeros phase
								update_max(local.max_v, ply);
								retro_mark_changed(pos_gen, ml, stm);
								local.wrote = true;
								break;
							}
						}

						if (!e.is_draw()) break;  // stale-WIN path; nothing else to try

						const auto res = check_loss(pos_gen, ml, ply, tid);
						if (!res.is_loss)
						{
							if (e.has_change())
							{
								write_dtm(idx, stm, e.without_change());
								local.wrote = true;
							}
							break;
						}
						// 50MR collapse: worst-resisting route exceeds 100 plies →
						// drawn before the loss materializes; reclassify DRAW.
						if (res.loss_phase > 100)
						{
							if (e.has_change())
							{
								write_dtm(idx, stm, e.without_change());
								local.wrote = true;
							}
							break;
						}
						write_dtm(idx, stm, DTM_Final_Entry::make_loss(res.loss_dtm));
						write_phase(idx, stm, res.loss_phase);
						update_max<uint16_t>(local.max_v, res.loss_dtm + 1);
						(void)retro_mark_wins(pos_gen, ml, stm, res.loss_dtm + 1, res.loss_phase);
						local.wrote = true;
						break;
					}
					default:
						break;
					}
				}

				if (chunk.any_intermediate) local.any_intermediate = true;
				update_max(local.max_classified, chunk.max_classified);

				// Evict only full-CHUNK_SIZE chunks — head/tail share their bit.
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
		// Evict: no Intermediate cells remain and every classified cell's value
		// is strictly behind ply-1, so no action_for_entry can fire at this ply
		// or any future ply. Any later write reinstates the bit via mark_iter.
		if (!any_intermediate && max_classified + 1 < ply)
			m_iter_groups[stm][g] = 0;
	}
	return global;
}

void DTM50_Generator::iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply)
{
	auto check_interrupt = [&](uint16_t fp) {
		if (egtb_is_interrupt_requested())
			throw DTM50_Interrupted{ 0, 0, fp, m_max_dtm, m_current_hmc };
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
		// Stop only after passing every classified dtm — init-seeded WIN(d)
		// from cap/promo sits dormant until ply hits d-1.
		if (!rw.wrote && !rb.wrote && finished_ply > m_max_dtm) break;
		check_interrupt(finished_ply);
	}
}

// =============================================================================
// Paging (working-set load/evict). apply_working_set lives in egtb_gen.h
// (templated, shared with DTC); page_in_for_group is DTM-specific because it
// folds opp's push-target groups into the working set for PAWN_EVAL inside
// run_iter.
// =============================================================================

void DTM50_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_paging_budget_bytes == 0) return;
	const Color opp = color_opp(me);
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = cur_layer(me).slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const auto& kingsm = m_epsi.slice_manager();
	const auto& psm = m_epsi.pawn_slice_manager();
	const bool has_pawns = psm.has_pawns();

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, ntotal);

	const size_t ngroups = m_table->m_dtm[WHITE][0].num_groups();
	auto& need_me  = m_scratch_need[me];
	auto& need_opp = m_scratch_need[opp];
	need_me.assign(ngroups, 0);
	need_opp.assign(ngroups, 0);
	need_me[group_id] = 1;

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
				need_opp[same / spg] = 1;
				for (int32_t k : m_scratch_nbrs)
				{
					const size_t neigh = static_cast<size_t>(pp) * nks + static_cast<size_t>(k);
					need_opp[neigh / spg] = 1;
				}
			}
			if (has_pawns)
			{
				// PAWN_EVAL inside run_iter reads opp's push-target groups at
				// the same king-slice. DTC doesn't need this — pawns there are
				// handled forward-only at init.
				for (int32_t tpid : psm.push_target_slices(pid))
				{
					const size_t target =
						static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
					need_opp[target / spg] = 1;
				}
			}
		}
	}

	apply_working_set(thread_pool, &cur_layer(WHITE), &cur_layer(BLACK), m_scratch_need[WHITE], m_scratch_need[BLACK]);
	// Same need bitmaps page opp hmc=0 / hmc=k+1 for layer-k retro; phase
	// slices come along for layer-0 retro.
	if (m_current_hmc != 0)
	{
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
	}
	else
	{
		apply_working_set(thread_pool,
			&m_table->m_phase[WHITE], &m_table->m_phase[BLACK],
			m_scratch_need[WHITE], m_scratch_need[BLACK]);
	}
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
	uint16_t resume_hmc = 0;
	int64_t resume_batch_idx = -1;
	int64_t resume_fusion_idx = -1;
	uint16_t resume_finished_ply = 0;
	uint16_t resume_max_dtm = 0;
	{
		Checkpoint_File ckpt{};
		if (read_checkpoint(ckpt_path, &ckpt))
		{
			resume_hmc = ckpt.hmc;
			resume_batch_idx = static_cast<int64_t>(ckpt.batch_idx);
			resume_fusion_idx = static_cast<int64_t>(ckpt.fusion_idx);
			resume_finished_ply = ckpt.finished_ply;
			resume_max_dtm = ckpt.max_dtm;
		}
		else
		{
			for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
			{
				m_table->m_dtm[WHITE][h].remove_disk_files();
				m_table->m_dtm[BLACK][h].remove_disk_files();
			}
			m_table->m_phase[WHITE].remove_disk_files();
			m_table->m_phase[BLACK].remove_disk_files();
		}
	}

	// Build order: hmc=0 (retro, self-referential in-M reads), then 99..1
	// top-down. hmc=0 leads so k>0 sees a finalized opp[hmc=0] for zeroing edges.
	auto build_one_layer = [&](uint16_t hmc) {
		m_current_hmc = hmc;

		// Unbounded budget: pre-load this layer plus the read-only layers it
		// consumes (opp hmc=0 for in-M push, opp hmc=k+1 for quiet). At hmc=0
		// also pre-load the phase slices. Bounded paths page those alongside
		// the write layer inside page_in_for_*.
		if (m_paging_budget_bytes == 0)
		{
			const size_t ng = cur_layer(WHITE).num_groups();
			std::vector<uint8_t> all_needed(ng, 1);
			apply_working_set(thread_pool, &cur_layer(WHITE), &cur_layer(BLACK), all_needed, all_needed);
			if (hmc != 0)
			{
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
			else
			{
				apply_working_set(thread_pool,
					&m_table->m_phase[WHITE], &m_table->m_phase[BLACK],
					all_needed, all_needed);
			}
		}

		// k+2 was the previous build's quiet-read layer; no future consumer.
		if (hmc != 0 && hmc + 2 < DTM50_HMC_COUNT)
		{
			m_table->m_dtm[WHITE][hmc + 2].evict_all(*thread_pool);
			m_table->m_dtm[BLACK][hmc + 2].evict_all(*thread_pool);
		}

		size_t total_fusions = 0;
		for (size_t bi = 0; bi < batches.size(); ++bi)
		{
			if (hmc == resume_hmc && static_cast<int64_t>(bi) < resume_batch_idx) continue;
			const auto& batch = batches[bi];
			const auto fusions = compute_fusion_groups(cur_layer(WHITE), batch);
			total_fusions += fusions.size();
			if (pawnful)
			{
				std::printf("  hmc=%u batch %zu/%zu (%zu pairs in %zu fusion%s)\n",
					hmc, bi + 1, batches.size(), batch.size(),
					fusions.size(), fusions.size() == 1 ? "" : "s");
				std::fflush(stdout);
			}
			for (size_t fi = 0; fi < fusions.size(); ++fi)
			{
				const bool is_resume_fusion =
					hmc == resume_hmc &&
					static_cast<int64_t>(bi) == resume_batch_idx &&
					static_cast<int64_t>(fi) == resume_fusion_idx;
				if (hmc == resume_hmc &&
				    static_cast<int64_t>(bi) == resume_batch_idx &&
				    static_cast<int64_t>(fi) < resume_fusion_idx)
					continue;

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
				refresh_active_metadata(cur_layer(WHITE));

				seed_iter_groups();
				m_max_dtm = 0;

				uint16_t start_ply = 0;
				if (is_resume_fusion)
				{
					start_ply = resume_finished_ply;
					update_max(m_max_dtm, resume_max_dtm);
				}
				else
				{
					const uint16_t init_max = init_entries(thread_pool);
					update_max(m_max_dtm, init_max);
				}

				try
				{
					// k>0: init fully classifies (no in-layer chain). k=0: retro.
					if (hmc == 0) iterate(thread_pool, start_ply);
				}
				catch (const DTM50_Interrupted& e)
				{
					for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
					{
						m_table->m_dtm[WHITE][h].evict_all(*thread_pool);
						m_table->m_dtm[BLACK][h].evict_all(*thread_pool);
					}
					// Phase slices spill same as data layers; resume re-pages.
					m_table->m_phase[WHITE].evict_all(*thread_pool);
					m_table->m_phase[BLACK].evict_all(*thread_pool);
					Checkpoint_File ckpt{};
					ckpt.hmc = hmc;
					ckpt.batch_idx = static_cast<uint32_t>(bi);
					ckpt.fusion_idx = static_cast<uint32_t>(fi);
					ckpt.finished_ply = e.finished_ply;
					ckpt.max_dtm = e.max_dtm;
					write_checkpoint(ckpt_path, ckpt);
					std::printf("\n  interrupted: checkpoint written (hmc=%u)\n", hmc);
					std::fflush(stdout);
					throw;
				}
			}
		}
		if (hmc == 0)
		{
			// Phase tape is layer-0-only. remove_disk_files before close so
			// the paths are still populated; close discards in-memory groups
			// without flushing dirty pages (we're throwing them away anyway).
			m_table->m_phase[WHITE].remove_disk_files();
			m_table->m_phase[BLACK].remove_disk_files();
			m_table->m_phase[WHITE].close();
			m_table->m_phase[BLACK].close();
		}
		(void)total_fusions;
	};

	// Per-layer schedule: 0, then 99 down to 1.
	std::vector<uint16_t> hmc_schedule;
	hmc_schedule.reserve(DTM50_HMC_COUNT);
	hmc_schedule.push_back(0);
	for (uint16_t h = DTM50_HMC_COUNT - 1; h >= 1; --h)
		hmc_schedule.push_back(h);

	bool seen_resume = false;
	for (uint16_t hmc : hmc_schedule)
	{
		if (!seen_resume)
		{
			if (hmc == resume_hmc) seen_resume = true;
			else if (resume_hmc != 0 || resume_batch_idx >= 0) continue;
			else seen_resume = true;
		}
		build_one_layer(hmc);
	}

	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build, %zu hmc layers): done in %s (%zu pawn-slice pairs in %zu batches)\n",
		DTM50_HMC_COUNT,
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size());
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
				dtm_rank[me], &dtm_storage_fn);
		}

		save_egtb_table(m_epsi, dtm_save, paths.dtm50_save_path(m_epsi, hmc), colors, EGTB_Magic::DTM50_MAGIC);

		std::ofstream fp(paths.dtm50_info_save_path(m_epsi, hmc), std::ios::binary | std::ios::trunc);
		fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));
	}

	remove_checkpoint(paths.dtm50_checkpoint_path(m_epsi));
	m_table->m_phase[WHITE].remove_disk_files();
	m_table->m_phase[BLACK].remove_disk_files();
	for (size_t h = 0; h < DTM50_HMC_COUNT; ++h)
	{
		m_table->m_dtm[WHITE][h].remove_disk_files();
		m_table->m_dtm[BLACK][h].remove_disk_files();
	}

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
