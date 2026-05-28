#include "egtb/egtb_gen_dtm.h"
#include "egtb/egtb_compress.h"
#include "egtb/slice_storage.h"
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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>

namespace {

struct Checkpoint_File
{
	static constexpr uint64_t MAGIC = 0x4B4D5444544D4843ull;  // 'CHMTDTMK'
	static constexpr uint32_t VERSION = 1;
	uint64_t magic = MAGIC;
	uint32_t version = VERSION;
	uint32_t batch_idx = 0;
	uint32_t fusion_idx = 0;
	uint16_t finished_ply = 0;
	uint16_t max_dtm = 0;
	uint8_t  _pad[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
};
static_assert(sizeof(Checkpoint_File) == 32, "DTM Checkpoint_File size");

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

DTM_Generator::DTM_Generator(
	const Piece_Config& ps,
	const std::map<Material_Key, std::shared_ptr<DTM_Sub_File_Flat>>& sub_dtm,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTM_Table>(ps, tmp_dir)),
	m_sub_dtm(sub_dtm)
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	const size_t bytes_per_color =
		m_table->m_dtm[WHITE].num_slices()
		* m_table->m_dtm[WHITE].within_slice_size()
		* sizeof(DTM_Final_Entry);
	const size_t total_bytes = bytes_per_color * 2;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_group_state(m_table->m_dtm[WHITE].num_groups());
	init_iter_state(
		m_table->m_dtm[WHITE].num_groups(),
		m_table->m_dtm[WHITE].num_entries());

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

DTM_Final_Entry DTM_Generator::read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
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
	const DTM_Sub_File_Flat* sub = m_sub_dtm_by_move[parent.turn()][captured][promo];

	// Resulting K vs K or sub-table unavailable → bare-king draw.
	return (sub == nullptr) ? DTM_Final_Entry::make_draw() : sub->read(sub_color, sub_idx, thread_id);
}

DTM_Final_Entry DTM_Generator::read_post_move_dtm(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move, thread_id);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	if (post_idx == BOARD_INDEX_NONE) return DTM_Final_Entry::make_illegal();
	return read_dtm(post_idx, color_opp(mover));
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

DTM_Final_Entry DTM_Generator::effective_opp_dtm_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const
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
			if (child_idx == BOARD_INDEX_NONE) continue;
			p_gen_for_ep.emplace(m_epsi, child_idx, opp);
		}
		// Post-EP DTM is from new STM (mover) — invert class and +1 ply for opp's EP.
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

DTM_Any_Entry DTM_Generator::make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id,
                                                Out_Param<uint16_t> worst_loss_dtm) const
{
	*worst_loss_dtm = 0;
	if (!pos_gen.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
		return DTM_Final_Entry::make_illegal();

	Position& pos = pos_gen.board();
	const bool in_check = pos.is_in_check(pos.turn());

	// Seeds derived from sub-DTM values on cap/promo moves.
	uint16_t best_win_dtm  = std::numeric_limits<uint16_t>::max();
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
		if (!is_cap && !m.is_promotion())
		{
			any_in_material = true;
			// Pawn push crosses slices; child lives in a non-iterating built slice → retro can't bridge.
			if (piece_type(pos.piece_at(m.from())) == PAWN)
				any_pawn_eval = true;
			continue;
		}
		any_pawn_eval = true;
		const DTM_Final_Entry sub_e = read_sub_tb(pos_gen, m, thread_id);
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
		if (in_check) return DTM_Final_Entry::make_loss(0);
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
	return any_pawn_eval
		? DTM_Intermediate_Entry::make_pawn_eval()
		: DTM_Intermediate_Entry{};
}

uint16_t DTM_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtm[WHITE].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtm[WHITE].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;

	// push_target_slices allocates; cache once per init call.
	std::vector<std::vector<int32_t>> targets_by_pid(m_epsi.num_pawn_slices());
	for (int32_t pid : m_active_pawn_slices)
		targets_by_pid[static_cast<size_t>(pid)] = psm.push_target_slices(pid);

	// Working set: both colors at g + (push_target_pid, same_kid) for read_post_move_dtm.
	// Init writes both colors, so needs[B] == needs[W].
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
					uint16_t worst_loss_dtm = 0;
					std::visit(overload{
						[&](DTM_Final_Entry entry) {
							write_dtm(idx, us, entry);
							if (entry.is_win() || entry.is_loss())
							{
								const uint16_t v = static_cast<uint16_t>(entry.value());
								update_max(local_max, v);
							}
						},
						[&](DTM_Intermediate_Entry entry) {
							write_dtm(idx, us, entry);
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

// Dispatch on Any_Entry via std::visit: Intermediate handles draws with flags
// (CHANGE / PAWN_EVAL routing); Final handles classified WIN/LOSS retros and
// stale-WIN bookkeeping.
DTM_Generator::Iter_Action DTM_Generator::action_for_entry(
	DTM_Any_Entry e, uint16_t ply) const
{
	const bool has_pawns = m_epsi.pawn_slice_manager().has_pawns();
	return std::visit(overload{
		[&](DTM_Intermediate_Entry ie) -> Iter_Action {
			if (ie.has_change()) return Iter_Action::CHANGE_REVERIFY;
			// PAWN_EVAL: forward-read for retro-blind edges (cap/promo + pawn push).
			if (ply > 0 && ie.has_pawn_eval()) return Iter_Action::PAWN_EVAL;
			return Iter_Action::SKIP;
		},
		[&](DTM_Final_Entry fe) -> Iter_Action {
			if (fe.is_illegal()) return Iter_Action::SKIP;
			if (ply == 0)
			{
				if (fe.is_loss() && fe.value() == 0) return Iter_Action::MARK_WIN_IN_1;
				return Iter_Action::SKIP;
			}
			// WIN(ply) and WIN(ply-1) both MARK_CHANGED so pred check_loss sees a
			// fully-classified child (same-ply just-overwritten + prior-ply settled).
			if (fe.is_win() && (fe.value() == ply || fe.value() == ply - 1))
				return Iter_Action::MARK_CHANGED;
			if (fe.is_loss() && fe.value() == ply)
				return Iter_Action::MARK_WIN_PREDS;
			// Stale WIN(value > ply) from init cap/promo seed; faster in-material
			// pawn push can overwrite. Routes through PAWN_EVAL handler
			// (no check_loss fallthrough).
			if (has_pawns && fe.is_win() && fe.value() > ply)
				return Iter_Action::PAWN_EVAL;
			return Iter_Action::SKIP;
		}
	}, e);
}

DTM_Generator::Loss_Verification_Result DTM_Generator::check_loss(
	Position_For_Gen& pos_gen,
	Move_List& ml,
	uint16_t ply, size_t thread_id) const
{
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	Loss_Verification_Result r;
	bool any_legal = false;
	uint16_t max_contribution = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const bool is_cap_or_ep = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo     = m.is_promotion();
		const bool is_pawn_move = piece_type(pos.piece_at(m.from())) == PAWN;

		uint16_t contribution;
		if (is_cap_or_ep || is_promo)
		{
			// Sub-TB move.
			const DTM_Final_Entry sub_e = read_sub_tb(pos_gen, m, thread_id);
			if (!sub_e.is_win()) return r;
			contribution = static_cast<uint16_t>(sub_e.value() + 1);
		}
		else if (is_pawn_move && is_pawn_double_push(m))
		{
			const DTM_Final_Entry opp_e = effective_opp_dtm_after_dp(pos_gen, m, thread_id);
			if (!opp_e.is_win()) return r;
			if (opp_e.value() >= ply) return r;  // strict ply ordering
			contribution = static_cast<uint16_t>(opp_e.value() + 1);
		}
		else
		{
			// Quiet in-material (incl. single pawn push); child is in current table.
			const Board_Index child = next_quiet_index(pos_gen, m);
			if (child == BOARD_INDEX_NONE) return r;
			const DTM_Final_Entry ce = read_dtm(child, opp);
			if (!ce.is_win()) return r;
			if (ce.value() >= ply) return r;
			contribution = static_cast<uint16_t>(ce.value() + 1);
		}
		update_max(max_contribution, contribution);
	}

	if (!any_legal) return r;
	if (max_contribution != ply) return r;

	r.is_loss = true;
	r.loss_dtm = max_contribution;
	return r;
}

bool DTM_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen,
                                        Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
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
		wrote = true;
	}
	return wrote;
}

void DTM_Generator::retro_mark_changed(Position_For_Gen& pos_gen,
                                       Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtm<DTM_Intermediate_Entry>(pred, opp);
		if (!e.is_draw() || e.has_change()) continue;
		// Atomic OR: a racing retro_mark_wins Final write keeps its bits; the
		// CHANGE bit lands on top harmlessly (Final dispatch ignores it).
		m_table->m_dtm[opp].lock_add_flags(pred, DTM_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtm[opp]);
	}
}

bool DTM_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    Move_List& ml, Color stm,
                                    uint16_t target_dtm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
	const DTM_Final_Entry new_e = DTM_Final_Entry::make_win(target_dtm);
	bool wrote = false;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const DTM_Final_Entry cur = read_dtm(pred, opp);
		if (cur.is_illegal()) continue;
		if (cur.is_loss()) continue;  // never demote a classified LOSS
		// Overwrite Intermediate OR stale WIN with larger dtm. The stale-WIN
		// case is DTM-specific: init seeds WIN(sub_dtm+1) from cap/promo; retro
		// may find a smaller dtm via in-material quiets and must improve it.
		if (cur.is_win() && cur.value() <= target_dtm) continue;
		write_dtm(pred, opp, new_e);
		wrote = true;
	}
	return wrote;
}

DTM_Generator::Iter_Result DTM_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                                                   Color stm, uint16_t ply)
{
	const size_t spg = m_table->m_dtm[stm].slices_per_group();
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

					const DTM_Any_Entry cell = read_dtm_any(idx, stm);

					std::visit(overload{
						[&](DTM_Final_Entry fe) {
							if (!fe.is_illegal())
								update_max(chunk.max_classified, static_cast<uint16_t>(fe.value()));
						},
						[&](DTM_Intermediate_Entry ie) {
							// Only flagged Intermediates revisit; bare DRAW skips forever.
							if (ie.has_any_flags()) chunk.any_intermediate = true;
						}
					}, cell);

					const Iter_Action action = action_for_entry(cell, ply);
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
						if (retro_mark_wins(pos_gen, ml, stm, target)) { local.wrote = true; update_max(local.max_v, target); }
						break;
					}
					case Iter_Action::CHANGE_REVERIFY:
					case Iter_Action::PAWN_EVAL:
					{
						// Forward pawn-WIN: push to opp-LOSS(ply-1) → WIN(ply);
						// also improves stale WIN(value > ply).
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
								update_max(local.max_v, ply);
								retro_mark_changed(pos_gen, ml, stm);
								local.wrote = true;
								break;
							}
						}

						if (std::holds_alternative<DTM_Final_Entry>(cell))
							break;  // stale-WIN path; nothing else to try

						// Intermediate fallback: check_loss for both CHANGE_REVERIFY
						// and PAWN_EVAL (forward edge may have just settled).
						const auto res = check_loss(pos_gen, ml, ply, tid);
						if (!res.is_loss)
						{
							// Only CHANGE clears count as wrote — keeps iterate
							// alive one more ply. PAWN_EVAL-no-change must not
							// bump wrote, else iterate overshoots.
							auto ie = std::get<DTM_Intermediate_Entry>(cell);
							if (ie.has_change())
							{
								ie.clear_flag(DTM_FLAG_CHANGE);
								write_dtm(idx, stm, ie);
								local.wrote = true;
							}
							break;
						}
						write_dtm(idx, stm, DTM_Final_Entry::make_loss(res.loss_dtm));
						update_max<uint16_t>(local.max_v, res.loss_dtm + 1);
						(void)retro_mark_wins(pos_gen, ml, stm, res.loss_dtm + 1);
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
		// action_for_entry will fire here again. Later writes reinstate via mark_iter.
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

// =============================================================================
// Paging. apply_working_set is shared (egtb_gen.h); page_in_for_group is
// DTM-specific because it folds opp's push-target groups in for PAWN_EVAL.
// =============================================================================

void DTM_Generator::page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
                                      Color me, size_t group_id)
{
	if (m_paging_budget_bytes == 0) return;
	const Color opp = color_opp(me);
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtm[me].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const auto& kingsm = m_epsi.slice_manager();
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
				// PAWN_EVAL reads opp's push-target groups at same king-slice.
				// DTC handles pawns forward-only at init, so doesn't need this.
				for (int32_t tpid : psm.push_target_slices(pid))
				{
					const size_t target =
						static_cast<size_t>(tpid) * nks + static_cast<size_t>(kid);
					need_opp[target / spg] = 1;
				}
			}
		}
	}

	apply_working_set(thread_pool, &m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

// =============================================================================
// gen() orchestrator with paging + checkpoint/resume.
// =============================================================================

void DTM_Generator::gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

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

	// Unbounded budget: pre-load every group; paging is a no-op.
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
		const auto fusions = compute_fusion_groups(m_table->m_dtm[WHITE], batch);
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
			refresh_active_metadata(m_table->m_dtm[WHITE]);

			seed_iter_groups();

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
				iterate(thread_pool, start_ply);
			}
			catch (const DTM_Interrupted& e)
			{
				m_table->m_dtm[WHITE].evict_all(*thread_pool);
				m_table->m_dtm[BLACK].evict_all(*thread_pool);
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

	remove_checkpoint(ckpt_path);
	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
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

using DTM_Save_Cache = Save_Group_Cache<DTM_Final_Entry, DTM_Intermediate_Entry>;
using DTM_Pinned_Range = Pinned_Group_Range<DTM_Final_Entry, DTM_Intermediate_Entry>;

NODISCARD DTM_Singular_Probe_Result dtm_singular_probe(
	const Piece_Config_For_Gen& epsi,
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>& src,
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
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>& src,
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
				const DTM_Final_Entry e = src.template read<DTM_Final_Entry>(static_cast<Board_Index>(idx));
				const uint64_t w = epsi.orbit_weight(didx);
				info.add_result(color, e.wdl(), w);
				if (e.is_win())
					info.maybe_update_longest_win(color, idx, e.value());
				// DRAW/ILLEGAL: WDL companion is authoritative — exclude from
				// histogram so rank table spends short codes on W/L values.
				if (!e.is_illegal() && !e.is_draw())
				{
					// hist_2b unused (DTM halves in both tiers); stays zeroed.
					const uint16_t v = dtm_value_for_storage(e);
					if (v < Value_Histogram::HIST_BINS) ++hist.hist_1b[v];
				}
				epsi.step_to_next(inout_param(didx));
			}
		}
		cache.release(color, g);
	}
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
		cap_groups = std::max<size_t>(1, m_paging_budget_bytes / bytes_per_group);
		max_workers = cap_groups;
	}

	static constexpr size_t DTM_BLOCK_SIZE = 1024 * 1024;
	Compressed_EGTB dtm_save[COLOR_NB];
	Value_Histogram dtm_hist[COLOR_NB];

	DTM_Save_Cache cache(&m_table->m_dtm[WHITE], &m_table->m_dtm[BLACK], cap_groups);

	const size_t num_positions = m_epsi.num_positions();

	for (Color me : colors)
	{
		const auto probe = dtm_singular_probe(m_epsi, m_table->m_dtm[me], cache, me, num_positions);
		if (probe.singular == WDL_Entry::DRAW)
		{
			m_info.draw_cnt[me]    = probe.legal_cnt;
			m_info.illegal_cnt[me] = probe.illegal_cnt;
			std::printf("save dtm %d: singular DRAW\n", static_cast<int>(me));
			dtm_save[me] = Compressed_EGTB::make_singular(WDL_Entry::DRAW);
		}
		else
		{
			gather_dtm_info(m_epsi, cache, m_table->m_dtm[me], me, num_positions, m_info, dtm_hist[me]);
		}

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	// Both tiers store the same halved value; tier only affects rank-index width.
	Value_Rank_Table dtm_rank[COLOR_NB];
	size_t dtm_entry_bytes[COLOR_NB]{};
	for (Color me : colors)
	{
		dtm_rank[me] = Value_Rank_Table::build_1b(dtm_hist[me].hist_1b);
		dtm_entry_bytes[me] = (dtm_rank[me].ranks.size() <= 256) ? 1 : 2;
	}

	for (Color me : colors)
	{
		if (!dtm_save[me].is_singular())
		{
			Block_Source src = make_entry_block_source(m_table->m_dtm[me], cache, me, DTM_BLOCK_SIZE, dtm_entry_bytes[me]);
			dtm_save[me] = save_compress_egtb(
				thread_pool, src, me, m_info, dtm_entry_bytes[me], DTM_BLOCK_SIZE, max_workers,
				dtm_rank[me], &dtm_storage_fn);
		}
		cache.purge(me);
		m_table->m_dtm[me].remove_disk_files();
		m_table->m_dtm[me].close();
	}

	save_egtb_table(m_epsi, dtm_save, paths.dtm_save_path(m_epsi), colors, EGTB_Magic::DTM_MAGIC);

	std::ofstream fp(paths.dtm_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
