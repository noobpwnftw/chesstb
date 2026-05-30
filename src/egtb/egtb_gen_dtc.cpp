#include "egtb/egtb_gen_dtc.h"
#include "egtb/egtb_compress.h"
#include "egtb/slice_storage.h"
#include "egtb/symmetry.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/index_permutation.h"
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
#include <mutex>
#include <optional>
#include <variant>

namespace {

struct Checkpoint_File
{
	static constexpr uint64_t MAGIC = 0x4B43544454434843ull;  // 'CHCTDTCK'
	static constexpr uint32_t VERSION = 1;
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

	init_group_state(m_table->m_dtc[WHITE].num_groups());
	init_iter_state(
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

WDL_Entry DTC_Generator::read_sub_tb(Position_For_Gen& pos_gen, Move move, size_t thread_id) const
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

WDL_Entry DTC_Generator::effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const
{
	const WDL_Entry no_ep = read_post_move_wdl(pos_gen, dp_move, thread_id);

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

	p.undo_move(dp_move, captured_by_dp);

	if (!any_ep) return no_ep;
	return static_cast<int>(best_ep_for_opp) > static_cast<int>(no_ep)
		? best_ep_for_opp : no_ep;
}

WDL_Entry DTC_Generator::read_post_move_wdl(Position_For_Gen& pos_gen, Move move, size_t thread_id) const
{
	const Position& parent = pos_gen.board_unchecked();
	const bool is_cap = move.is_ep_capture() || !parent.is_empty(move.to());
	if (is_cap || move.is_promotion())
		return read_sub_tb(pos_gen, move, thread_id);

	const Color mover = parent.turn();
	const Board_Index post_idx = next_quiet_index(pos_gen, move);
	if (post_idx == BOARD_INDEX_NONE) return WDL_Entry::ILLEGAL;
	return read_dtc(post_idx, color_opp(mover)).wdl();
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
		// Stalemate: plain Intermediate; no pre_quiet pred lands here, never reclassified.
		if (in_check) return DTC_Final_Entry::make_loss(0);
		return DTC_Intermediate_Entry{};
	}

	if (best == ValueClassicWin) return DTC_Final_Entry::make_win(1);

	if (!any_quiet_legal)
	{
		static constexpr uint16_t CURSED_BOUND = DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1;
		switch (best)
		{
		case ValueCursedWin:
		{
			DTC_Final_Entry e = DTC_Final_Entry::make_win(CURSED_BOUND);
			e.set_flag(DTC_FLAG_CAP_CWIN);
			return e;
		}
		case ValueDraw:        return DTC_Intermediate_Entry{};
		case ValueCursedLoss:
		{
			DTC_Final_Entry e = DTC_Final_Entry::make_loss(CURSED_BOUND);
			e.set_flag(DTC_FLAG_CAP_CLOSS);
			return e;
		}
		case ValueClassicLoss: return DTC_Final_Entry::make_loss(1);
		default: break;
		}
	}

	// Hint zeroing class on the Intermediate for the cursed transition.
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

	// Working set: both colors at g + (push_target_pid, same_kid) on opp.
	auto page_in_for_init_group = [&](size_t g) {
		if (m_paging_budget_bytes == 0) return;
		m_scratch_need[WHITE].assign(ngroups, 0);
		m_scratch_need[WHITE][g] = 1;
		const size_t g_start = g * spg;
		const size_t g_end   = std::min(g_start + spg, ntotal);
		// pid-major: slice-major filter wastes ~spg/nks iters per active pid.
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
								static_cast<DTC_Rule_Flag>(
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
			// Strict ply-ordering: child dtz < ply (child must already be classified).
			if (ce.value() >= ply) return r;
			contribution = static_cast<uint16_t>(ce.value() + 1);
		}
		update_max(max_contribution, contribution);
	}

	if (!any_legal) return r;
	if (max_contribution != ply) return r;  // only verify at natural LOSS-DTZ

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
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		// Never overwrite a Final pred.
		if (!read_dtc<DTC_Intermediate_Entry>(pred, opp).is_draw()) continue;
		write_dtc(pred, opp, DTC_Final_Entry::make_win(1));
	}
}

void DTC_Generator::retro_mark_changed(Position_For_Gen& pos_gen,
                                       Move_List& ml, Color stm)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		const auto e = read_dtc<DTC_Intermediate_Entry>(pred, opp);
		if (!e.is_draw() || e.has_change()) continue;
		// Atomic OR: a racing retro_mark_wins Final write keeps its bits; the
		// CHANGE bit lands on top harmlessly (Final dispatch ignores it).
		m_table->m_dtc[opp].lock_add_flags(pred, DTC_FLAG_CHANGE);
		mark_iter(opp, pred, m_table->m_dtc[opp]);
	}
}

void DTC_Generator::retro_mark_wins(Position_For_Gen& pos_gen,
                                    Move_List& ml, Color stm,
                                    uint16_t target_dtz, bool cursed)
{
	const Color opp = color_opp(stm);
	pos_gen.board_unchecked().gen_pseudo_legal_pre_quiets(out_param(ml));
	DTC_Final_Entry new_e = DTC_Final_Entry::make_win(target_dtz);
	if (cursed) new_e.set_flag(DTC_FLAG_CAP_CWIN);
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Board_Index pred = next_quiet_index(pos_gen, ml[i]);
		if (pred == BOARD_INDEX_NONE) continue;
		if (!read_dtc<DTC_Intermediate_Entry>(pred, opp).is_draw()) continue;
		write_dtc(pred, opp, new_e);  // overwrite drops CAP_* / CHANGE bits
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

					const DTC_Any_Entry cell = read_dtc_any(idx, stm);

					// Syzygy class-driven iterate: WIN(dtz=ply) flags preds CHANGE;
					// flagged preds reverify; verified LOSS retros preds as WIN(ply+1).
					// Visit fuses chunk-state bookkeeping with the action decode;
					// bare DRAW Intermediates stay inert.
					const Iter_Action action = std::visit(overload{
						[&](DTC_Intermediate_Entry ie) -> Iter_Action {
							if (ie.has_any_flags()) chunk.any_intermediate = true;
							// Cursed-gate fires regardless of CHANGE. Otherwise a
							// coincident CHANGE at ply 101 would route
							// cap_cwin/cap_closs through CHANGE_REVERIFY, which
							// can't classify them (check_loss fails / returns
							// sub-clamp dtz), clearing CHANGE and leaving them
							// SKIP forever. PROMOTE_CWIN and CAPT_CLOSS_REVERIFY
							// overwrite anyway, so the stale CHANGE is fine.
							if (phase == Iter_Phase::CURSED
							    && ply == DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1)
							{
								if (ie.has_cap_cwin())  return Iter_Action::PROMOTE_CWIN;
								if (ie.has_cap_closs()) return Iter_Action::CAPT_CLOSS_REVERIFY;
							}
							if (ie.has_change())
							{
								return (phase == Iter_Phase::CURSED && ie.has_cap_closs())
									? Iter_Action::CAPT_CLOSS_REVERIFY
									: Iter_Action::CHANGE_REVERIFY;
							}
							return Iter_Action::SKIP;
						},
						[&](DTC_Final_Entry fe) -> Iter_Action {
							if (fe.is_illegal()) return Iter_Action::SKIP;
							update_max(chunk.max_classified, static_cast<uint16_t>(fe.value()));
							if (ply == 0)
							{
								// Mate: preds become WIN(1).
								if (fe.is_loss() && fe.value() == 0) return Iter_Action::MARK_WIN_IN_1;
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
					{
						// Bridge Intermediate's CAP_CWIN routing hint into Final WIN's cursed marker.
						auto promoted = DTC_Final_Entry::copy_rule(std::get<DTC_Intermediate_Entry>(cell));
						promoted.set_score_win(ply);
						write_dtc(idx, stm, promoted);
						retro_mark_changed(pos_gen, ml, stm);
						local.any = true;
						break;
					}
					case Iter_Action::MARK_WIN_PREDS:
					{
						const bool cursed = is_cursed_class_entry(std::get<DTC_Final_Entry>(cell))
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
							// Failed verify: clear CHANGE, preserve CAP_*.
							auto ie = std::get<DTC_Intermediate_Entry>(cell);
							ie.clear_flag(DTC_FLAG_CHANGE);
							write_dtc(idx, stm, ie);
							break;
						}
						uint16_t loss_dtz = res.loss_dtz;
						const bool cursed = res.cursed || phase == Iter_Phase::CURSED;
						if (action == Iter_Action::CAPT_CLOSS_REVERIFY
						    && loss_dtz < DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1)
							loss_dtz = DTC_Final_Entry::MAX_NON_CURSED_DTZ + 1;
						DTC_Final_Entry new_e = DTC_Final_Entry::make_loss(loss_dtz);
						if (cursed) new_e.set_flag(DTC_FLAG_CAP_CLOSS);
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
	remove_checkpoint(ckpt_path);
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
			uint64_t win_cnt = 0, lose_cnt = 0, draw_cnt = 0, illegal_cnt = 0;
			uint16_t longest_value = 0;
			uint64_t longest_idx   = 0;

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
					switch (e.wdl())
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
						if (v > longest_value) { longest_value = v; longest_idx = logical; }
					}
					// DRAW/ILLEGAL: WDL companion authoritative — exclude so
					// rank table spends short codes on W/L values.
					if (!e.is_illegal() && !e.is_draw())
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
				const auto& e = src.template view_at<DTC_Final_Entry>(static_cast<Board_Index>(idx));
				const uint64_t w = epsi.orbit_weight(didx);
				info.add_result(color, e.wdl(), w);
				if (e.is_win())
					info.maybe_update_longest_win(color, idx, e.value());

				if (!e.is_illegal() && !e.is_draw())
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
				gather_dtc_info(m_epsi, cache, *m_table, me, m_epsi.num_positions(),
					m_info, dtc_hist[me]);
			}
			std::printf("save wdl %d: singular\n", static_cast<int>(me));
			wdl_save[me] = Compressed_EGTB::make_singular(probe.singular);
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
				thread_pool, src, me, max_workers);
		}

		if (m_info.longest_win[me] > 0)
		{
			Position_For_Gen pos_gen(m_epsi, static_cast<Board_Index>(m_info.longest_idx[me]), me);
			pos_gen.board().to_fen(Span(m_info.longest_fen[me]));
		}
	}

	save_wdl_table(m_epsi, wdl_index_perm, wdl_save, paths.wdl_save_path(m_epsi), colors, EGTB_Magic::WDL_MAGIC);

	for (Color me : colors) wdl_save[me] = {};

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
		if (m_info.win_cnt[me] + m_info.lose_cnt[me] != 0)
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
			thread_pool, src, me, m_info, dtc_entry_bytes[me], DTC_BLOCK_SIZE, max_workers,
			chosen);
		cache.purge(me);
		m_table->m_dtc[me].remove_disk_files();
		m_table->m_dtc[me].close();
	}

	save_egtb_table(m_epsi, dtc_index_perm, dtc_save, paths.dtc_save_path(m_epsi), colors, EGTB_Magic::DTC_MAGIC);

	std::ofstream fp(paths.dtc_info_save_path(m_epsi), std::ios::binary | std::ios::trunc);
	fp.write(reinterpret_cast<const char*>(&m_info), sizeof(EGTB_Info));

	const auto t_save_end = std::chrono::steady_clock::now();
	std::printf("  save_to_disk: done in %s\n",
		format_elapsed_time(t_save_start, t_save_end).c_str());
}
