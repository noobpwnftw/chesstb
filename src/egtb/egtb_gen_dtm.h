#pragma once

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"
#include "egtb/piece_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/symmetry.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/move.h"
#include "chess/attack.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/thread_pool.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

struct DTM_Interrupted
{
	uint32_t batch_idx;
	uint32_t fusion_idx;
	uint16_t finished_ply;
	uint16_t max_dtm;
};

// DTM = exact plies to mate, no 50mr, no zeroing reset. Sub-tables must expose
// DTM values (not just WDL) since the ply count carries across captures and
// promotions; that's the central divergence from DTC.

struct DTM_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry> m_dtm[COLOR_NB];
	bool m_is_symmetric = false;

	DTM_Table(const Piece_Config& ps, const std::filesystem::path& tmp_dir) :
		m_epsi(ps)
	{
		const size_t per = m_epsi.within_slice_size();
		const size_t np = m_epsi.num_positions();
		const size_t ns = (per == 0) ? 0 : (np / per);
		const std::string name = m_epsi.name();
		const uint64_t magic = static_cast<uint64_t>(EGTB_Magic::DTM_SLICE_MAGIC);
		m_dtm[WHITE].create(ns, per, tmp_dir, magic, name + ".w.%05zu.dtms");
		m_dtm[BLACK].create(ns, per, tmp_dir, magic, name + ".b.%05zu.dtms");
	}

	DTM_Table(const DTM_Table&) = delete;
	DTM_Table& operator=(const DTM_Table&) = delete;

	NODISCARD INLINE DTM_Final_Entry read(Color stm, Board_Index pos) const
	{
		return m_dtm[stm].read(pos);
	}
};

class DTM_Generator : public EGTB_Generator
{
public:
	DTM_Generator(
		const Piece_Config& ps,
		const std::map<Material_Key, std::shared_ptr<DTM_Sub_File_Flat>>& sub_dtm,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTM_Table>& table() const { return m_table; }
	NODISCARD const Piece_Config_For_Gen& epsi() const { return m_epsi; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const EGTB_Info& info() const { return m_info; }

private:
	std::shared_ptr<DTM_Table> m_table;
	std::map<Material_Key, std::shared_ptr<DTM_Sub_File_Flat>> m_sub_dtm;
	const DTM_Sub_File_Flat* m_sub_dtm_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	EGTB_Info m_info;

	std::vector<uint8_t> m_scratch_need[COLOR_NB];
	std::vector<int32_t> m_scratch_nbrs;

	// Highest classified dtm value ever written to the table (init + retro).
	// iterate() uses this as the termination floor: a silent ply is only safe
	// to break on if it's past every classified entry's value, since init may
	// seed WIN(d_cap) with values above the natural retrograde frontier. Not
	// atomic: each worker reports a thread-local max via the run-on-all-threads
	// return value, and iterate folds them after the join.
	uint16_t m_max_dtm = 0;

	NODISCARD INLINE DTM_Final_Entry read_dtm(Board_Index pos, Color stm) const
	{
		return m_table->m_dtm[stm].read(pos);
	}
	INLINE void write_dtm(Board_Index pos, Color stm, DTM_Final_Entry e)
	{
		m_table->m_dtm[stm].write(e, pos);
		mark_iter(stm, pos, m_table->m_dtm[stm]);
	}

	NODISCARD DTM_Final_Entry read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;
	NODISCARD DTM_Final_Entry read_post_move_dtm(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;
	NODISCARD DTM_Final_Entry effective_opp_dtm_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const;

	// Out `worst_loss_dtm` is the largest `sub_e.value()+1` contribution from
	// cap/promo children when the return is Intermediate; 0 otherwise.
	// iterate() floors m_max_dtm with it so silent-ply termination can't stop
	// before check_loss reaches the ply where the cap-path contribution
	// classifies the cell.
	NODISCARD DTM_Any_Entry make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id,
	                                           Out_Param<uint16_t> worst_loss_dtm) const;
	// Returns the max classified dtm seeded during init (0 if only Intermediates
	// and ILLEGAL/LOSS(0) were emitted).
	uint16_t init_entries(In_Out_Param<Thread_Pool> thread_pool);

	enum class Iter_Action : uint8_t {
		SKIP,
		MARK_WIN_IN_1,
		MARK_WIN_PREDS,
		MARK_CHANGED,
		CHANGE_REVERIFY,
		// Pawnful-only: forward pawn-move eval at this cell, since
		// gen_pseudo_legal_pre_quiets omits pawn pushes and the reverse-mark
		// pipeline can't propagate dtm through them. Fires for any Intermediate
		// without CHANGE (LOSS-verify candidates) or stale WIN(value > ply)
		// (improvement candidates). Folded with CHANGE_REVERIFY in run_iter.
		PAWN_EVAL,
	};

	struct Loss_Verification_Result {
		bool is_loss = false;
		uint16_t loss_dtm = 0;
	};

	NODISCARD Iter_Action action_for_entry(DTM_Final_Entry e, uint16_t ply) const;

	struct Iter_Result {
		bool wrote = false;     // any write this iter (incl. CHANGE-flag flips)
		uint16_t max_v = 0;     // max classified dtm value written this iter
		bool any_intermediate = false;  // any non-illegal, unclassified cell read
		uint16_t max_classified = 0;     // max value of any WIN/LOSS cell read
	};
	NODISCARD Iter_Result run_iter(In_Out_Param<Thread_Pool> thread_pool,
	                               Color stm, uint16_t ply);


	NODISCARD Loss_Verification_Result check_loss(
		Position_For_Gen& pos_gen,
		Move_List& ml,
		uint16_t ply, size_t thread_id) const;

	// Returns true if any predecessor was overwritten.
	bool retro_mark_win_in_1(Position_For_Gen& pos_gen, Move_List& ml, Color stm);
	void retro_mark_changed(Position_For_Gen& pos_gen, Move_List& ml, Color stm);
	bool retro_mark_wins(Position_For_Gen& pos_gen, Move_List& ml,
	                     Color stm, uint16_t target_dtm);

	void iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply = 0);

	// Pages in (me=g) + (opp = king-neighbors ∪ push-targets) at each (pid,kid)
	// in the active pair-pid set. Push-targets are DTM-specific vs. DTC's
	// page_in_for_group: PAWN_EVAL inside run_iter reads opp's push-target groups.
	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);
};

