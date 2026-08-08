#pragma once

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"
#include "egtb/piece_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/symmetry.h"

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/thread_pool.h"

struct DTC_Interrupted
{
	uint8_t phase;          // 0 = CLEAN, 1 = CURSED
	bool pending_cursed;    // pending cursed phase
	uint16_t finished_ply;  // last fully-completed ply in `phase`
};

struct DTC_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry> m_dtc[COLOR_NB];
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

	template <typename EntryT>
	NODISCARD INLINE EntryT read(Color stm, Board_Index pos) const
	{
		return m_dtc[stm].template view_at<EntryT>(pos);
	}
	template <typename EntryT>
	INLINE void write(Color stm, Board_Index pos, EntryT e) { m_dtc[stm].write(e, pos); }

	DTC_Table(const DTC_Table&) = delete;
	DTC_Table& operator=(const DTC_Table&) = delete;
};

class DTC_Generator : public EGTB_Generator
{
public:
	DTC_Generator(
		const Piece_Config& ps,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(
		std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<WDL_Entry>>> sub_wdl,
		In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTC_Table>& table() const { return m_table; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

private:
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

	std::shared_ptr<DTC_Table> m_table;
	std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<WDL_Entry>>> m_sub_wdl_by_material;

	template <typename EntryT>
	NODISCARD INLINE EntryT read_dtc(Color stm, Board_Index pos) const
	{
		return m_table->template read<EntryT>(stm, pos);
	}
	INLINE void write_dtc(Color stm, Board_Index pos, DTC_Final_Entry e)
	{
		m_table->write(stm, pos, e);
	}
	INLINE void write_dtc(Color stm, Board_Index pos, DTC_Intermediate_Entry e)
	{
		m_table->write(stm, pos, e);
		mark_iter(stm, pos, m_table->m_dtc[stm]);
	}

	NODISCARD WDL_Entry read_sub_tb(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move) const;

	NODISCARD DTC_Any_Entry make_initial_entry(Position_For_Gen& pos_gen) const;
	bool init_entries(In_Out_Param<Thread_Pool> thread_pool);

	enum class Iter_Phase : uint8_t { CLEAN, CURSED };
	void iterate(In_Out_Param<Thread_Pool> thread_pool, bool pending_cursed,
	             Iter_Phase start_phase = Iter_Phase::CLEAN,
	             uint16_t finished_ply = 0);

	enum class Iter_Action : uint8_t {
		MARK_WIN_IN_1,
		MARK_WIN_PREDS,
		MARK_CHANGED,
		REVERIFY,               // re-run check_loss; Phase decides the CAP_CLOSS tag
		PROMOTE_CWIN,           // cursed-gate: draw+cap_cwin -> WIN(ply)+cap_cwin, retro CHANGE
	};

	struct Loss_Verification_Result {
		bool is_loss = false;
		uint16_t loss_dtz = 0;
	};

	NODISCARD bool run_iter(In_Out_Param<Thread_Pool> thread_pool,
	                        Color stm, uint16_t ply, Iter_Phase phase);

	template <Iter_Phase Phase>
	NODISCARD bool run_iter_impl(In_Out_Param<Thread_Pool> thread_pool,
	                             Color stm, uint16_t ply);

	template <Iter_Phase Phase>
	NODISCARD Loss_Verification_Result check_loss(
		Position_For_Gen& pos_gen,
		uint16_t ply, DTC_Intermediate_Entry hint) const;

	void retro_mark_win_in_1(Position_For_Gen& pos_gen);
	void retro_mark_changed(Position_For_Gen& pos_gen);
	template <Iter_Phase Phase>
	void retro_mark_wins(Position_For_Gen& pos_gen, uint16_t target_dtz);

	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);
};
