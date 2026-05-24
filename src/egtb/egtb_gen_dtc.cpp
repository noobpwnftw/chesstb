#include "egtb/egtb_gen_dtc.h"
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
#include <variant>

namespace {

struct Checkpoint_File
{
	static constexpr uint64_t MAGIC = 0x4B43544454434843ull;  // 'CHCTDTCK'
	static constexpr uint32_t VERSION = 3;
	uint64_t magic = MAGIC;
	uint32_t version = VERSION;
	uint32_t batch_idx = 0;
	uint32_t fusion_idx = 0;
	uint8_t  phase = 0;
	bool     pending_cursed = false;
	uint16_t finished_ply = 0;
	uint8_t  _pad[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
};
static_assert(sizeof(Checkpoint_File) == 32, "Checkpoint_File size");

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

} // namespace

DTC_Generator::DTC_Generator(
	const Piece_Config& ps,
	const std::map<Material_Key, std::shared_ptr<WDL_File_For_Probe>>& sub_wdl,
	const std::filesystem::path& tmp_dir,
	size_t budget_bytes) :
	EGTB_Generator(ps),
	m_table(std::make_shared<DTC_Table>(ps, tmp_dir)),
	m_sub_wdl(sub_wdl)
{
	m_paging_budget_bytes = budget_bytes;
	std::filesystem::create_directories(tmp_dir);
	m_table->m_is_symmetric = m_is_symmetric;

	const size_t bytes_per_color =
		m_table->m_dtc[WHITE].num_slices()
		* m_table->m_dtc[WHITE].within_slice_size()
		* sizeof(DTC_Final_Entry);
	const size_t total_bytes = bytes_per_color * 2;
	if (m_paging_budget_bytes >= total_bytes) m_paging_budget_bytes = 0;

	init_group_state(
		m_table->m_dtc[WHITE].num_groups(),
		m_table->m_dtc[WHITE].num_entries());

	for (Color c : { WHITE, BLACK })
	for (Piece captured = PIECE_NONE; captured < PIECE_NB; captured = static_cast<Piece>(captured + 1))
	for (Piece_Type promo = PIECE_TYPE_NONE; promo < PIECE_TYPE_NB; promo = static_cast<Piece_Type>(promo + 1))
	{
		const Piece_Config_For_Gen* sub = m_sub_epsi_by_move[c][captured][promo];
		if (sub == nullptr) continue;
		if (sub->num_pieces() <= 2) continue;
		auto it = m_sub_wdl.find(sub->min_material_key());
		m_sub_wdl_by_move[c][captured][promo] = it->second.get();
	}
}

WDL_Entry DTC_Generator::read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
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
	const WDL_File_For_Probe* sub = m_sub_wdl_by_move[parent.turn()][captured][promo];

	return (sub == nullptr) ? WDL_Entry::DRAW : sub->read(sub_color, sub_idx, thread_id);
}

WDL_Entry DTC_Generator::effective_opp_wdl_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const
{
	const WDL_Entry no_ep = read_post_move_wdl(pos_gen, dp_move, thread_id);

	const Position& parent = pos_gen.board_unchecked();
	const Color mover = parent.turn();
	const Color opp = color_opp(mover);
	Position p = parent;
	(void)p.do_move(dp_move);

	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(dp_move.to());

	WDL_Entry best_ep_for_opp = WDL_Entry::LOSE;
	bool any_ep = false;

	// Post-DP position is the same for both ep targets; lazy-build once.
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
		const WDL_Entry w_after_ep = read_sub_tb(*p_gen_for_ep, ep_move, thread_id);
		WDL_Entry w_opp;
		switch (w_after_ep)
		{
			case WDL_Entry::WIN:          w_opp = WDL_Entry::LOSE;         break;
			case WDL_Entry::CURSED_WIN:   w_opp = WDL_Entry::BLESSED_LOSS; break;
			case WDL_Entry::DRAW:         w_opp = WDL_Entry::DRAW;         break;
			case WDL_Entry::BLESSED_LOSS: w_opp = WDL_Entry::CURSED_WIN;   break;
			case WDL_Entry::LOSE:         w_opp = WDL_Entry::WIN;          break;
			default:                      w_opp = WDL_Entry::DRAW;         break;
		}
		if (static_cast<int>(w_opp) > static_cast<int>(best_ep_for_opp))
			best_ep_for_opp = w_opp;
		any_ep = true;
	}

	if (!any_ep) return no_ep;
	return static_cast<int>(best_ep_for_opp) > static_cast<int>(no_ep)
		? best_ep_for_opp : no_ep;
}

WDL_Entry DTC_Generator::read_post_move_wdl(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move, thread_id);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	if (post_idx == BOARD_INDEX_NONE) return WDL_Entry::ILLEGAL;
	return m_table->m_dtc[color_opp(mover)].read(post_idx).wdl();
}

DTC_Any_Entry DTC_Generator::make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id) const
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

	Position& pos = pos_gen.board();
	const bool in_check = pos.is_in_check(pos.turn());

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
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	bool any_legal = false;
	bool any_quiet_legal = false;
	Value best = ValueNone;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const bool is_cap   = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo = m.is_promotion();
		const bool is_pawn  = piece_type(pos.piece_at(m.from())) == PAWN;

		if (is_cap || is_promo) {
			fold(best, read_sub_tb(pos_gen, m, thread_id));
		} else if (is_pawn) {
			fold(best, is_pawn_double_push(m)
				? effective_opp_wdl_after_dp(pos_gen, m, thread_id)
				: read_post_move_wdl(pos_gen, m, thread_id));
		} else {
			any_quiet_legal = true;
		}

		if (best >= ValueClassicWin) break;
	}
	if (!any_legal)
	{
		// Stalemate stored as plain Intermediate (m_data=0): no pre_quiet
		// pred can land on a no-move position, so it's never reclassified.
		return in_check ? DTC_Final_Entry::make_loss(0)
		                : DTC_Intermediate_Entry{};
	}

	if (best == ValueClassicWin)
		return DTC_Final_Entry::make_win(1);

	if (!any_quiet_legal)
	{
		static constexpr uint16_t CURSED_BOUND = DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1;
		switch (best)
		{
		case ValueCursedWin:   return DTC_Final_Entry::make_win(CURSED_BOUND).with_cap_cwin();
		case ValueDraw:        return DTC_Intermediate_Entry{};  // bit-identical to Final DRAW
		case ValueCursedLoss:  return DTC_Final_Entry::make_loss(CURSED_BOUND).with_cap_closs();
		case ValueClassicLoss: return DTC_Final_Entry::make_loss(1);
		default: break;
		}
	}

	// Hint the zeroing class on the Intermediate so the cursed-transition can act on it.
	switch (best)
	{
	case ValueCursedWin:   return DTC_Intermediate_Entry::make_cap_cwin();
	case ValueCursedLoss:  return DTC_Intermediate_Entry::make_cap_closs();
	default: break;
	}
	return DTC_Intermediate_Entry{};
}

bool DTC_Generator::init_entries(In_Out_Param<Thread_Pool> thread_pool)
{
	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = m_table->m_dtc[WHITE].slices_per_group();
	const size_t ntotal = m_epsi.num_slices();
	const size_t ngroups = m_table->m_dtc[WHITE].num_groups();
	const auto& pid_in_pair = m_pid_in_pair;
	bool pending_cursed = false;

	// push_target_slices allocates; cache once per init call.
	std::vector<std::vector<int32_t>> targets_by_pid(m_epsi.num_pawn_slices());
	for (int32_t pid : m_active_pawn_slices)
		targets_by_pid[static_cast<size_t>(pid)] = psm.push_target_slices(pid);

	// Init working set: both colors at g, plus (push_target_pid, same_kid)
	// on opp (pushes don't move kings).
	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
		m_scratch_need[WHITE][g] = 1;
		const size_t g_start = g * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		// pid-major loop: a slice-major filter wastes ~spg/nks iterations
		// per active pid on tiny-within materials.
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

		const auto rets = thread_pool->run_sync_task_on_all_threads([&](size_t tid) {
			constexpr size_t PROGRESS_BAR_UPDATE_PERIOD = 64 * 64;
			Position_For_Gen pos_gen(m_epsi, BOARD_INDEX_ZERO, WHITE);
			Board_Index prev = BOARD_INDEX_NONE;
			bool any_cursed_hint = false;
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
					write_dtc(idx, WHITE, DTC_Final_Entry::make_illegal());
					write_dtc(idx, BLACK, DTC_Final_Entry::make_illegal());
					continue;
				}
				if (slice_has_stab[pos_gen.index().king_slice_id])
				{
					const Board_Index canon = board_index_of_position(m_epsi, pos_gen.board());
					if (canon != idx)
					{
						write_dtc(idx, WHITE, DTC_Final_Entry::make_illegal());
						write_dtc(idx, BLACK, DTC_Final_Entry::make_illegal());
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
					std::visit(overload{
						[&](DTC_Final_Entry entry) {
							write_dtc(idx, us, entry);
						},
						[&](DTC_Intermediate_Entry entry) {
							write_dtc(idx, us, entry);
							static constexpr auto CAP_HINTS =
								static_cast<DTC_Intermediate_Entry_Flag>(
									DTC_FLAG_CAP_CWIN | DTC_FLAG_CAP_CLOSS);
							if (entry.has_flag(CAP_HINTS))
								any_cursed_hint = true;
						},
					}, make_initial_entry(pos_gen, tid));
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
	const auto& kingsm = m_epsi.slice_manager();

	const size_t g_start = group_id * spg;
	const size_t g_end   = std::min(g_start + spg, ntotal);

	const size_t ngroups = m_table->m_dtc[WHITE].num_groups();
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
		}
	}

	apply_working_set(thread_pool, &m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], m_scratch_need[WHITE], m_scratch_need[BLACK]);
}

namespace {

INLINE bool is_cursed_class_entry(DTC_Final_Entry e)
{
	if (e.is_win())
		return e.has_cap_cwin()  || e.value() > DTC_Final_Entry::MAX_NON_CURSED_DTZ;
	if (e.is_loss())
		return e.has_cap_closs() || e.value() > DTC_Final_Entry::MAX_NON_CURSED_DTZ;
	return false;
}

// Clean pass: only WDL::WIN. Cursed pass: WIN or CURSED_WIN.
INLINE bool wdl_is_opp_win(WDL_Entry w, bool cursed)
{
	if (w == WDL_Entry::WIN) return true;
	return cursed && w == WDL_Entry::CURSED_WIN;
}

} // namespace

// Syzygy-style class-driven iterate: WIN(dtz=ply) retro-flags preds with
// CHANGE; flagged entries reverify; verified LOSS retros preds as WIN(ply+1).
DTC_Generator::Iter_Action DTC_Generator::action_for_entry(
	DTC_Final_Entry e, uint16_t ply, Iter_Phase phase) const
{
	if (e.is_illegal()) return Iter_Action::SKIP;

	if (e.is_draw())
	{
		// Cursed-gate fires regardless of CHANGE: a cap_cwin cell's check_loss
		// always fails (the cap/promo move is mover-winning, so sub_e fails
		// is_win), so a coincident CHANGE bit at ply MAX_NON_CURSED_DTZ + 1
		// would otherwise route the cell into CHANGE_REVERIFY → clear CHANGE
		// → SKIP forever, missing PROMOTE_CWIN. cap_closs has the symmetric
		// problem with check_loss returning a sub-clamp dtz. PROMOTE_CWIN /
		// CAPT_CLOSS_REVERIFY both overwrite the cell anyway, so dropping
		// a stale CHANGE bit is fine.
		if (phase == Iter_Phase::CURSED
		    && ply == DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1)
		{
			if (e.has_cap_cwin()) return Iter_Action::PROMOTE_CWIN;
			if (e.has_cap_closs()) return Iter_Action::CAPT_CLOSS_REVERIFY;
		}
		if (e.has_change())
		{
			return (phase == Iter_Phase::CURSED && e.has_cap_closs())
				? Iter_Action::CAPT_CLOSS_REVERIFY
				: Iter_Action::CHANGE_REVERIFY;
		}
		return Iter_Action::SKIP;
	}

	if (ply == 0)
	{
		// MATE at ply 0: preds become WIN(1).
		if (e.is_loss() && e.value() == 0) return Iter_Action::MARK_WIN_IN_1;
		return Iter_Action::SKIP;
	}

	// ply >= 1 here since the ply == 0 branch returned above.
	if (e.is_win() && (e.value() == ply || e.value() == ply - 1))
		return Iter_Action::MARK_CHANGED;

	if (e.is_loss() && e.value() == ply)
		return Iter_Action::MARK_WIN_PREDS;

	return Iter_Action::SKIP;
}

DTC_Generator::Loss_Verification_Result DTC_Generator::check_loss(
	Position_For_Gen& pos_gen,
	Move_List& ml,
	uint16_t ply, Iter_Phase phase, size_t thread_id) const
{
	const bool cursed_phase = (phase == Iter_Phase::CURSED);
	Position& pos = pos_gen.board_unchecked();
	const Color opp = color_opp(pos.turn());
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	Loss_Verification_Result r;
	bool any_legal = false;
	uint16_t max_contribution = 0;
	bool any_cursed_child = false;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const bool is_cap_or_ep = m.is_ep_capture() || !pos.is_empty(m.to());
		const bool is_promo     = m.is_promotion();
		const bool is_pawn_move = piece_type(pos.piece_at(m.from())) == PAWN;
		const bool is_zeroing   = is_cap_or_ep || is_promo || is_pawn_move;

		uint16_t contribution;
		if (is_zeroing)
		{
			WDL_Entry child_wdl;
			if (is_pawn_move && !is_promo && !is_cap_or_ep)
			{
				if (is_pawn_double_push(m))
					child_wdl = effective_opp_wdl_after_dp(pos_gen, m, thread_id);
				else
					child_wdl = read_post_move_wdl(pos_gen, m, thread_id);
			}
			else
			{
				child_wdl = read_sub_tb(pos_gen, m, thread_id);
			}
			if (!wdl_is_opp_win(child_wdl, cursed_phase))
				return r;  // is_loss=false
			if (child_wdl == WDL_Entry::CURSED_WIN)
			{
				any_cursed_child = true;
				contribution = static_cast<uint16_t>(
					DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1);
			}
			else
			{
				contribution = 1;
			}
		}
		else
		{
			const Board_Index child = next_quiet_index(pos_gen, m);
			if (child == BOARD_INDEX_NONE) return r;
			const DTC_Final_Entry ce = read_dtc(child, opp);
			if (!ce.is_win()) return r;
			if (is_cursed_class_entry(ce))
			{
				if (!cursed_phase) return r;
				any_cursed_child = true;
			}
			// Strict ply-ordering: child win_loss[v] must already be set,
			// i.e. child dtz < ply.
			if (ce.value() >= ply) return r;
			contribution = static_cast<uint16_t>(ce.value() + 1);
		}
		update_max(max_contribution, contribution);
	}

	if (!any_legal) return r;
	// Verify only when natural LOSS-DTZ matches the current ply.
	if (max_contribution != ply) return r;

	r.is_loss  = true;
	r.loss_dtz = max_contribution;
	r.cursed   = cursed_phase || any_cursed_child
	             || r.loss_dtz > DTC_Final_Entry::MAX_NON_CURSED_DTZ;
	return r;
}

void DTC_Generator::retro_mark_win_in_1(Position_For_Gen& pos_gen,
                                        Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<false>(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		// Never overwrite a Final pred.
		if (!read_dtc(pred, opp).is_draw()) continue;
		write_dtc(pred, opp, DTC_Final_Entry::make_win(1));
	}
}

void DTC_Generator::retro_mark_changed(Position_For_Gen& pos_gen,
                                       Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<false>(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtc(pred, opp);
		if (!e.is_draw() || e.has_change()) continue;
		// Atomic OR: a concurrent retro_mark_wins overwrite to Final clears the
		// flag naturally (we always write fresh class+value).
		m_table->m_dtc[opp].lock_add_flags(pred, DTC_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtc[opp]);
	}
}

void DTC_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    Move_List& ml, Color stm,
                                    uint16_t target_dtz, bool cursed)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets<false>(out_param(ml));
	DTC_Final_Entry new_e = DTC_Final_Entry::make_win(target_dtz);
	if (cursed) new_e = new_e.with_cap_cwin();
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		if (!read_dtc(pred, opp).is_draw()) continue;
		// Overwrite drops any CAP_* / CHANGE bits on the Intermediate.
		write_dtc(pred, opp, new_e);
	}
}

bool DTC_Generator::run_iter(In_Out_Param<Thread_Pool> thread_pool,
                             Color stm, uint16_t ply, Iter_Phase phase)
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
					if (!pid_in_pair[pid_of_idx])
						continue;

					const DTC_Final_Entry e = read_dtc(idx, stm);
					if (e.is_illegal()) continue;

					// Only flagged Intermediates (CHANGE | CAP_CWIN | CAP_CLOSS) can
					// fire a future action; bare DRAW will never become actionable
					// unless a write reinstates the bit via mark_iter.
					if (e.is_draw()) { if (e.has_any_flags()) chunk.any_intermediate = true; }
					else
					{
						const uint16_t v = static_cast<uint16_t>(e.value());
						update_max(chunk.max_classified, v);
					}

					const Iter_Action action = action_for_entry(e, ply, phase);

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
						retro_mark_win_in_1(pos_gen, ml, stm);
						local.any = true;
						break;
					case Iter_Action::MARK_CHANGED:
						retro_mark_changed(pos_gen, ml, stm);
						local.any = true;
						break;
					case Iter_Action::PROMOTE_CWIN:
						write_dtc(idx, stm,
							DTC_Final_Entry::make_win(ply).with_cap_cwin());
						retro_mark_changed(pos_gen, ml, stm);
						local.any = true;
						break;
					case Iter_Action::MARK_WIN_PREDS:
					{
						const bool cursed = is_cursed_class_entry(e)
						                    || phase == Iter_Phase::CURSED;
						retro_mark_wins(pos_gen, ml, stm,
						                static_cast<uint16_t>(ply + 1), cursed);
						local.any = true;
						break;
					}
					case Iter_Action::CHANGE_REVERIFY:
					case Iter_Action::CAPT_CLOSS_REVERIFY:
					{
						const auto res = check_loss(pos_gen, ml, ply, phase, tid);
						if (!res.is_loss)
						{
							// Failed verify: clear CHANGE only, preserve CAP_*.
							write_dtc(idx, stm, e.without_change());
							break;
						}
						uint16_t loss_dtz = res.loss_dtz;
						const bool cursed = res.cursed || phase == Iter_Phase::CURSED;
						if (action == Iter_Action::CAPT_CLOSS_REVERIFY
						    && loss_dtz < DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1)
							loss_dtz = DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1;
						DTC_Final_Entry new_e = DTC_Final_Entry::make_loss(loss_dtz);
						if (cursed) new_e = new_e.with_cap_closs();
						write_dtc(idx, stm, new_e);
						retro_mark_wins(pos_gen, ml, stm,
						                static_cast<uint16_t>(loss_dtz + 1),
						                cursed);
						local.any = true;
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
			if (r.any) any_global = true;
			if (r.any_intermediate) any_intermediate = true;
			update_max(max_classified, r.max_classified);
		}
		// Evict: no Intermediate cells remain and every classified cell's value
		// is strictly behind ply-1, so no action_for_entry can fire at this ply
		// or any future ply. Any later write reinstates the bit via mark_iter.
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
			std::printf("  iterate %4u\r", finished_ply); std::fflush(stdout);
			const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CLEAN);
			const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CLEAN);
			if (wrote_w || wrote_b) finished = false;
			check_interrupt(Iter_Phase::CLEAN, pending_cursed, finished_ply);
		}
		if (pending_cursed)
		{
			finished_ply = DTC_Final_Entry::MAX_NON_CURSED_DTZ;  // 100
			finished = false;
		}
		else if (finished)
			return;
	}

	while (!finished && finished_ply < DTC_SCORE_MAX)
	{
		++finished_ply;
		finished = true;
		std::printf("  iterate %4u\r", finished_ply); std::fflush(stdout);
		const bool wrote_w = run_iter(thread_pool, WHITE, finished_ply, Iter_Phase::CURSED);
		const bool wrote_b = run_iter(thread_pool, BLACK, finished_ply, Iter_Phase::CURSED);
		if (wrote_w || wrote_b) finished = false;
		check_interrupt(Iter_Phase::CURSED, false, finished_ply);
	}
}

void DTC_Generator::gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths)
{
	const auto t_total_start = std::chrono::steady_clock::now();
	const auto& psm = m_epsi.pawn_slice_manager();
	const auto& batches = psm.pair_topo_batches();
	const bool pawnful = psm.has_pawns();
	size_t total_pairs = 0;
	for (const auto& batch : batches) total_pairs += batch.size();

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
			resume_phase = (ckpt.phase == 0) ? Iter_Phase::CLEAN : Iter_Phase::CURSED;
			resume_finished_ply = ckpt.finished_ply;
			resume_pending_cursed = ckpt.pending_cursed;
		}
		else
		{
			m_table->m_dtc[WHITE].remove_disk_files();
			m_table->m_dtc[BLACK].remove_disk_files();
		}
	}

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
		const auto fusions = compute_fusion_groups(m_table->m_dtc[WHITE], batch);
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
			refresh_active_metadata(m_table->m_dtc[WHITE]);
			seed_iter_groups();

			bool pending_cursed = false;
			if (!is_resume_fusion)
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
				m_table->m_dtc[WHITE].evict_all(*thread_pool);
				m_table->m_dtc[BLACK].evict_all(*thread_pool);
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
	const auto t_total_end = std::chrono::steady_clock::now();
	std::printf("  gen (init + build): done in %s (%zu pawn-slice pairs in %zu batches, %zu fusion groups)\n",
		format_elapsed_time(t_total_start, t_total_end).c_str(),
		total_pairs, batches.size(), total_fusions);
}

void DTC_Generator::save_slices(const EGTB_Paths& paths)
{
	for (Color c : { WHITE, BLACK })
	{
		save_slice_file(m_table->m_dtc[c],
		                paths.dtc_slice_save_path(m_epsi, c),
		                static_cast<uint64_t>(EGTB_Magic::DTC_SLICE_MAGIC));
	}
}

namespace {

using DTC_Save_Cache = Save_Group_Cache<DTC_Final_Entry>;
using DTC_Pinned_Range = Pinned_Group_Range<DTC_Final_Entry>;

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
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry>& src,
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
			const auto* const raw = src.slice_data(s);
			if (!didx_init)
			{
				epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
				didx_init = true;
			}
			for (size_t i = 0; i < end_in_slice; ++i)
			{
				DTC_Final_Entry e;
				std::memcpy(&e, &raw[i], sizeof(e));
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
	Gather_Sink& sink)
{
	ASSERT(sink.color == color);
	auto& src = table.m_dtc[color];
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
	return Block_Source{
		total_packed_bytes,
		[&epsi, &src, &cache, &sink, within, spg, num_positions, total_packed_bytes](
			size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t>
		{
			const size_t byte_off = block_id * WDL_BLOCK_SIZE;
			const size_t byte_sz  = std::min<size_t>(WDL_BLOCK_SIZE, total_packed_bytes - byte_off);
			ASSERT(scratch.size() >= byte_sz);

			std::memset(scratch.data(), 0, byte_sz);
			auto* packed = reinterpret_cast<Packed_WDL_Entries*>(scratch.data());

			const size_t first_raw = byte_off * WDL_ENTRY_PACK_RATIO;
			const size_t end_raw   = std::min(first_raw + byte_sz * WDL_ENTRY_PACK_RATIO, num_positions);

			const size_t first_g = (first_raw / within) / spg;
			const size_t last_g  = (end_raw == first_raw ? first_g
			                                             : ((end_raw - 1) / within) / spg);
			DTC_Pinned_Range pin(cache, sink.color, first_g, last_g);

			thread_local std::vector<uint32_t> hist1_local;
			thread_local std::vector<uint32_t> hist2_local;
			hist1_local.assign(Value_Histogram::HIST_BINS, 0);
			hist2_local.assign(Value_Histogram::HIST_BINS, 0);
			uint64_t win_cnt = 0, lose_cnt = 0, draw_cnt = 0, illegal_cnt = 0;
			uint16_t longest_value = 0;
			uint64_t longest_idx   = 0;

			Decomposed_Board_Index didx{};
			epsi.decompose_board_index(static_cast<Board_Index>(first_raw), out_param(didx));
			size_t raw = first_raw;
			while (raw < end_raw)
			{
				const size_t s = raw / within;
				const size_t in_slice_start = raw - s * within;
				const size_t in_slice_end   = std::min(within, in_slice_start + (end_raw - raw));

				for (size_t i = in_slice_start; i < in_slice_end; ++i)
				{
					const Board_Index idx = static_cast<Board_Index>(s * within + i);
					const DTC_Final_Entry e = src.read(idx);
					const WDL_Entry w = e.wdl();
					const size_t cur_raw = raw + (i - in_slice_start);
					const size_t packed_byte = cur_raw / WDL_ENTRY_PACK_RATIO - byte_off;
					const size_t in_packed   = cur_raw % WDL_ENTRY_PACK_RATIO;
					set_wdl_entry(packed[packed_byte], in_packed, w);

					const uint64_t ow = epsi.orbit_weight(didx);
					switch (w)
					{
						case WDL_Entry::DRAW:         draw_cnt    += ow; break;
						case WDL_Entry::LOSE:
						case WDL_Entry::BLESSED_LOSS: lose_cnt    += ow; break;
						case WDL_Entry::WIN:
						case WDL_Entry::CURSED_WIN:   win_cnt     += ow; break;
						case WDL_Entry::ILLEGAL:      illegal_cnt += ow; break;
					}
					if (e.is_win())
					{
						const uint16_t v = static_cast<uint16_t>(e.value());
						if (v > longest_value) { longest_value = v; longest_idx = cur_raw; }
					}
					if (!e.is_illegal())
					{
						++hist1_local[static_cast<size_t>(dtc_value_for_storage(e))];
						++hist2_local[static_cast<size_t>(static_cast<uint16_t>(e.value()))];
					}
					epsi.step_to_next(inout_param(didx));
				}

				raw += (in_slice_end - in_slice_start);
			}

			{
				std::lock_guard<std::mutex> lk(sink.mu);
				if (!sink.merged[block_id])
				{
					sink.merged[block_id] = 1;
					const Color c = sink.color;
					sink.info->win_cnt[c]     += win_cnt;
					sink.info->lose_cnt[c]    += lose_cnt;
					sink.info->draw_cnt[c]    += draw_cnt;
					sink.info->illegal_cnt[c] += illegal_cnt;
					if (longest_value > sink.info->longest_win[c]
					    || (longest_value == sink.info->longest_win[c]
					        && longest_value > 0
					        && longest_idx < sink.info->longest_idx[c]))
					{
						sink.info->longest_win[c] = longest_value;
						sink.info->longest_idx[c] = longest_idx;
					}
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

static Block_Source make_dtc_block_source(
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry>& src,
	DTC_Save_Cache& cache,
	Color color,
	size_t block_size,
	size_t entry_bytes)
{
	constexpr size_t kEntry = sizeof(DTC_Final_Entry);
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
			DTC_Pinned_Range pin(cache, color, first_g, last_g);

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

static void gather_dtc_info(
	const Piece_Config_For_Gen& epsi,
	DTC_Save_Cache& cache,
	DTC_Table& table,
	Color color,
	size_t num_positions,
	EGTB_Info& info,
	Value_Histogram& hist)
{
	auto& src = table.m_dtc[color];
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t ng  = src.num_groups();
	const size_t ns  = src.num_slices();
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
				const DTC_Final_Entry e = src.read(static_cast<Board_Index>(idx));
				const uint64_t w = epsi.orbit_weight(didx);
				info.add_result(color, e.wdl(), w);
				if (e.is_win())
					info.maybe_update_longest_win(color, idx, e.value());

				if (!e.is_illegal())
				{
					++hist.hist_1b[static_cast<size_t>(dtc_value_for_storage(e))];
					++hist.hist_2b[static_cast<size_t>(static_cast<uint16_t>(e.value()))];
				}
				epsi.step_to_next(inout_param(didx));
			}
		}

		cache.release(color, g);
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

	static constexpr size_t DTC_BLOCK_SIZE = 1024 * 1024;
	Compressed_EGTB wdl_save[COLOR_NB];
	Compressed_EGTB dtc_save[COLOR_NB];
	Value_Histogram dtc_hist[COLOR_NB];

	DTC_Save_Cache cache(&m_table->m_dtc[WHITE], &m_table->m_dtc[BLACK], cap_groups);

	for (Color me : colors)
	{
		const Singular_Probe_Result probe = singular_probe(
			m_epsi, m_table->m_dtc[me], cache, me, m_epsi.num_positions());

		if (probe.singular == WDL_Entry::DRAW)
		{
			m_info.draw_cnt[me]    = probe.legal_cnt;
			m_info.illegal_cnt[me] = probe.illegal_cnt;
			std::printf("save_compress_wdl %d: singular\n", static_cast<int>(me));
			wdl_save[me] = Compressed_EGTB::make_singular(WDL_Entry::DRAW);
		}
		else if (probe.singular != WDL_Entry::ILLEGAL)
		{
			gather_dtc_info(m_epsi, cache, *m_table, me, m_epsi.num_positions(),
				m_info, dtc_hist[me]);
			std::printf("save_compress_wdl %d: singular\n", static_cast<int>(me));
			wdl_save[me] = Compressed_EGTB::make_singular(probe.singular);
		}
		else
		{
			const size_t num_positions = m_epsi.num_positions();
			const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
			const size_t num_blocks = ceil_div(total_packed_bytes, WDL_BLOCK_SIZE);
			Gather_Sink sink{me, &m_info, &dtc_hist[me], {}, std::vector<uint8_t>(num_blocks, 0)};
			Block_Source src = make_wdl_block_source(
				m_epsi, *m_table, me, cache, num_positions, sink);
			wdl_save[me] = save_compress_wdl(
				thread_pool, src, me, max_workers);
		}

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	Value_Rank_Table dtc_rank_1b[COLOR_NB];
	Value_Rank_Table dtc_rank_2b[COLOR_NB];
	size_t dtc_entry_bytes[COLOR_NB]{};
	for (Color me : colors)
	{
		dtc_rank_1b[me] = Value_Rank_Table::build_1b(dtc_hist[me].hist_1b);
		dtc_rank_2b[me] = Value_Rank_Table::build_2b(dtc_hist[me].hist_2b);
		dtc_entry_bytes[me] = (dtc_rank_1b[me].ranks.size() <= 256) ? 1 : 2;
	}

	for (Color me : colors)
	{
		Value_Rank_Table& chosen = (dtc_entry_bytes[me] == 1) ? dtc_rank_1b[me] : dtc_rank_2b[me];
		Block_Source src = make_dtc_block_source(
			m_table->m_dtc[me], cache, me, DTC_BLOCK_SIZE, dtc_entry_bytes[me]);
		dtc_save[me] = save_compress_egtb(
			thread_pool, src, me, m_info, dtc_entry_bytes[me], DTC_BLOCK_SIZE, max_workers,
			chosen);
	}

	save_wdl_table(m_epsi, wdl_save, paths.wdl_save_path(m_epsi), colors, EGTB_Magic::WDL_MAGIC);
	save_egtb_table(m_epsi, dtc_save, paths.dtc_save_path(m_epsi), colors, EGTB_Magic::DTC_MAGIC);

	std::ofstream fp(paths.dtc_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	remove_checkpoint(paths.dtc_checkpoint_path(m_epsi));
	m_table->m_dtc[WHITE].remove_disk_files();
	m_table->m_dtc[BLACK].remove_disk_files();

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
