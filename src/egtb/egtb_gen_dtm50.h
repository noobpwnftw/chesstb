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

#include "util/allocation.h"
#include "util/defines.h"
#include "util/param.h"
#include "util/thread_pool.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

// DTM50 = plies-to-mate under 50MR. 100 per-hmc layers/color. hmc=0 is a retro
// fixed-point (in-M pawn pushes self-reference); hmc=99..1 build top-down from
// already-finalized layers (opp k+1 for quiet, opp 0 for in-M push, sub-TB 0
// for cap/promo). Phase tape is per-layer scratch tracking plies-since-zeroing;
// cells whose route would exceed 100 plies collapse to DRAW at write time, so
// no per-layer phase ever persists.

inline constexpr size_t DTM50_HMC_COUNT = 100;

struct DTM50_Interrupted
{
	uint32_t batch_idx;
	uint32_t fusion_idx;
	uint16_t finished_ply;
	uint16_t max_dtm;
	uint16_t hmc;
};

struct DTM50_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry> m_dtm[COLOR_NB][DTM50_HMC_COUNT];
	// Per-color phase scratch: plies-since-last-zeroing along chosen route.
	// Reused across layers; reset at start of each layer's build.
	Huge_Array<uint8_t> m_phase[COLOR_NB];
	bool m_is_symmetric = false;

	DTM50_Table(const Piece_Config& ps, const std::filesystem::path& tmp_dir) :
		m_epsi(ps)
	{
		const size_t per = m_epsi.within_slice_size();
		const size_t np = m_epsi.num_positions();
		const size_t ns = (per == 0) ? 0 : (np / per);
		const std::string name = m_epsi.name();
		const uint64_t magic = static_cast<uint64_t>(EGTB_Magic::DTM50_SLICE_MAGIC);
		for (size_t hmc = 0; hmc < DTM50_HMC_COUNT; ++hmc)
		{
			char w_fmt[128], b_fmt[128];
			std::snprintf(w_fmt, sizeof(w_fmt), "%s.w.h%02zu.%%05zu.dtm50s", name.c_str(), hmc);
			std::snprintf(b_fmt, sizeof(b_fmt), "%s.b.h%02zu.%%05zu.dtm50s", name.c_str(), hmc);
			m_dtm[WHITE][hmc].create(ns, per, tmp_dir, magic, w_fmt);
			m_dtm[BLACK][hmc].create(ns, per, tmp_dir, magic, b_fmt);
		}
		m_phase[WHITE] = Huge_Array<uint8_t>(np);
		m_phase[BLACK] = Huge_Array<uint8_t>(np);
	}

	DTM50_Table(const DTM50_Table&) = delete;
	DTM50_Table& operator=(const DTM50_Table&) = delete;

	NODISCARD INLINE DTM_Final_Entry read(Color stm, Board_Index pos, uint16_t hmc) const
	{
		return m_dtm[stm][hmc].read(pos);
	}
};

class DTM50_Generator : public EGTB_Generator
{
public:
	DTM50_Generator(
		const Piece_Config& ps,
		const std::map<Material_Key, std::shared_ptr<DTM50_Sub_File_Flat>>& sub_dtm,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTM50_Table>& table() const { return m_table; }
	NODISCARD const Piece_Config_For_Gen& epsi() const { return m_epsi; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

private:
	std::shared_ptr<DTM50_Table> m_table;
	std::map<Material_Key, std::shared_ptr<DTM50_Sub_File_Flat>> m_sub_dtm;
	const DTM50_Sub_File_Flat* m_sub_dtm_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};

	// Layer being built. Helpers dispatch off it; gen() sets it per layer.
	uint16_t m_current_hmc = 0;

	// Termination floor for iterate(); reset per layer.
	uint16_t m_max_dtm = 0;

	NODISCARD INLINE Sliced_EGTB_File_For_Gen<DTM_Final_Entry>& cur_layer(Color c) const
	{
		return m_table->m_dtm[c][m_current_hmc];
	}

	NODISCARD INLINE DTM_Final_Entry read_dtm(Board_Index pos, Color stm) const
	{
		return cur_layer(stm).read(pos);
	}
	INLINE void write_dtm(Board_Index pos, Color stm, DTM_Final_Entry e)
	{
		cur_layer(stm).write(e, pos);
		mark_iter(stm, pos, cur_layer(stm));
	}

	// Phase = plies-since-zeroing along the route. >100 ⇒ cursed, collapse to DRAW.
	NODISCARD INLINE uint8_t read_phase(Board_Index pos, Color stm) const
	{
		return m_table->m_phase[stm][static_cast<size_t>(pos)];
	}
	INLINE void write_phase(Board_Index pos, Color stm, uint8_t v)
	{
		m_table->m_phase[stm][static_cast<size_t>(pos)] = v;
	}

	NODISCARD DTM_Final_Entry read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;
	NODISCARD DTM_Final_Entry read_post_move_dtm(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;
	NODISCARD DTM_Final_Entry effective_opp_dtm_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const;

	NODISCARD DTM_Any_Entry make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id,
	                                           Out_Param<uint16_t> worst_loss_dtm) const;
	uint16_t init_entries(In_Out_Param<Thread_Pool> thread_pool);

	enum class Iter_Action : uint8_t {
		SKIP,
		MARK_WIN_IN_1,
		MARK_WIN_PREDS,
		MARK_CHANGED,
		CHANGE_REVERIFY,
		PAWN_EVAL,
	};

	struct Loss_Verification_Result {
		bool is_loss = false;
		uint16_t loss_dtm = 0;
		// Worst-resisting child's phase contribution. >100 ⇒ DRAW, not LOSS.
		uint8_t  loss_phase = 0;
	};

	NODISCARD Iter_Action action_for_entry(DTM_Final_Entry e, uint16_t ply) const;

	struct Iter_Result {
		bool wrote = false;
		uint16_t max_v = 0;
		bool any_intermediate = false;
		uint16_t max_classified = 0;
	};
	NODISCARD Iter_Result run_iter(In_Out_Param<Thread_Pool> thread_pool,
	                               Color stm, uint16_t ply);

	NODISCARD Loss_Verification_Result check_loss(
		Position_For_Gen& pos_gen,
		Move_List& ml,
		uint16_t ply, size_t thread_id) const;

	// retro_mark_wins gets cell_phase explicitly (pred's phase = 1 + cell_phase);
	// the cell idx is only in scope at the call site.
	bool retro_mark_win_in_1(Position_For_Gen& pos_gen, Move_List& ml, Color stm);
	void retro_mark_changed(Position_For_Gen& pos_gen, Move_List& ml, Color stm);
	bool retro_mark_wins(Position_For_Gen& pos_gen, Move_List& ml,
	                     Color stm, uint16_t target_dtm, uint8_t cell_phase);

	void iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply = 0);

	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);

	void reset_layer_state();
};
