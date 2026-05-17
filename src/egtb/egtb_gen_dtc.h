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
#include <optional>
#include <vector>

struct DTC_Interrupted
{
	uint8_t phase;          // 0 = CLEAN, 1 = CURSED
	bool pending_cursed;    // pending cursed phase
	uint16_t finished_ply;  // last fully-completed ply in `phase`
};

struct DTC_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry> m_dtc[COLOR_NB];
	bool m_is_symmetric = false;

	DTC_Table(const Piece_Config& ps, const std::filesystem::path& tmp_dir) :
		m_epsi(ps)
	{
		const size_t per = m_epsi.within_slice_size();
		const size_t np = m_epsi.num_positions();
		const size_t ns = (per == 0) ? 0 : (np / per);
		const std::string name = m_epsi.name();
		const uint64_t magic = static_cast<uint64_t>(EGTB_Magic::DTC_SLICE_MAGIC);
		m_dtc[WHITE].create(ns, per, tmp_dir, magic, name + ".w.%05zu.dtcs");
		m_dtc[BLACK].create(ns, per, tmp_dir, magic, name + ".b.%05zu.dtcs");
	}

	DTC_Table(const DTC_Table&) = delete;
	DTC_Table& operator=(const DTC_Table&) = delete;

	NODISCARD INLINE DTC_Final_Entry read(Color stm, Board_Index pos) const
	{
		return m_dtc[stm].read(pos);
	}
};

class DTC_Generator : public EGTB_Generator
{
public:
	DTC_Generator(
		const Piece_Config& ps,
		const std::map<Material_Key, std::shared_ptr<WDL_File_For_Probe>>& sub_wdl,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTC_Table>& table() const { return m_table; }
	NODISCARD const Piece_Config_For_Gen& epsi() const { return m_epsi; }

	void save_slices(const EGTB_Paths& paths);

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const EGTB_Info& info() const { return m_info; }

	NODISCARD WDL_Entry probe_read_post_move_wdl(const Position_For_Gen& pos_gen, Move move) const
	{
		return read_post_move_wdl(pos_gen, move, 0);
	}

private:
	std::shared_ptr<DTC_Table> m_table;
	std::map<Material_Key, std::shared_ptr<WDL_File_For_Probe>> m_sub_wdl;
	const WDL_File_For_Probe* m_sub_wdl_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	EGTB_Info m_info;

	std::vector<uint8_t> m_scratch_need[COLOR_NB];
	std::vector<int32_t> m_scratch_nbrs;

	template <typename EntryT = DTC_Final_Entry>
	NODISCARD INLINE EntryT read_dtc(Board_Index pos, Color stm) const
	{
		return EntryT(m_table->m_dtc[stm].read(pos));
	}
	template <typename EntryT>
	INLINE void write_dtc(Board_Index pos, Color stm, EntryT e)
	{
		m_table->m_dtc[stm].write(static_cast<DTC_Final_Entry>(e), pos);
		mark_iter(stm, pos, m_table->m_dtc[stm]);
	}

	NODISCARD WDL_Entry read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;

	NODISCARD WDL_Entry read_post_move_wdl(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;

	NODISCARD WDL_Entry effective_opp_wdl_after_dp(const Position_For_Gen& pos_gen, Move dp_move, size_t thread_id) const;

	NODISCARD DTC_Any_Entry make_initial_entry(Position_For_Gen& pos_gen, size_t thread_id) const;
	bool init_entries(In_Out_Param<Thread_Pool> thread_pool);

	enum class Iter_Phase : uint8_t { CLEAN, CURSED };
	void iterate(In_Out_Param<Thread_Pool> thread_pool, bool pending_cursed,
	             Iter_Phase start_phase = Iter_Phase::CLEAN,
	             uint16_t finished_ply = 0);

	enum class Iter_Action : uint8_t {
		SKIP,
		MARK_WIN_IN_1,
		MARK_WIN_PREDS,
		MARK_CHANGED,
		CHANGE_REVERIFY,        // verify all children win; result LOSS(p)
		CAPT_CLOSS_REVERIFY,    // like CHANGE but result clamped to LOSS(DRAW_RULE+1)
		PROMOTE_CWIN,           // cursed-gate: draw+cap_cwin -> WIN(ply)+cap_cwin, retro CHANGE
	};

	struct Loss_Verification_Result {
		bool is_loss = false;
		uint16_t loss_dtz = 0;
		bool cursed = false;
	};

	NODISCARD Iter_Action action_for_entry(DTC_Final_Entry e,
	                                       uint16_t ply,
	                                       Iter_Phase phase) const;

	NODISCARD bool run_iter(In_Out_Param<Thread_Pool> thread_pool,
	                        Color stm, uint16_t ply, Iter_Phase phase);

	NODISCARD Loss_Verification_Result check_loss(
		Position_For_Gen& pos_gen,
		Move_List& ml,
		uint16_t ply, Iter_Phase phase, size_t thread_id) const;

	void retro_mark_win_in_1(Position_For_Gen& pos_gen, Move_List& ml,
	                          Color stm);
	void retro_mark_changed(Position_For_Gen& pos_gen, Move_List& ml,
	                         Color stm);
	void retro_mark_wins(Position_For_Gen& pos_gen, Move_List& ml,
	                      Color stm, uint16_t target_dtz, bool cursed);

	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);
};

