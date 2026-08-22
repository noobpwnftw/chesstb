#pragma once

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"
#include "egtb/piece_group.h"
#include "egtb/piece_config_for_gen.h"

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/thread_pool.h"

struct DTZ_Interrupted
{
	uint8_t phase;
	bool pending_cursed;
	uint16_t finished_ply;
};

struct DTZ_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry> m_dtz[COLOR_NB];

	DTZ_Table(const Piece_Config& ps, const std::filesystem::path& tmp_dir) :
		m_epsi(ps)
	{
		const size_t per = m_epsi.within_slice_size();
		const size_t np = m_epsi.num_positions();
		const size_t ns = (per == 0) ? 0 : (np / per);
		const std::string name = m_epsi.name();
		const uint64_t magic = static_cast<uint64_t>(EGTB_Magic::DTZ_SLICE_MAGIC);
		m_dtz[WHITE].create(ns, per, tmp_dir, magic, name + ".w.%05zu.dtzs");
		m_dtz[BLACK].create(ns, per, tmp_dir, magic, name + ".b.%05zu.dtzs");
	}

	template <typename EntryT>
	NODISCARD EntryT read(Color stm, Board_Index pos) const
	{
		return m_dtz[stm].template view_at<EntryT>(pos);
	}
	template <typename EntryT>
	void write(Color stm, Board_Index pos, EntryT e) { m_dtz[stm].write(e, pos); }

	DTZ_Table(const DTZ_Table&) = delete;
	DTZ_Table& operator=(const DTZ_Table&) = delete;
};

class DTZ_Generator : public EGTB_Generator
{
public:
	DTZ_Generator(
		const Piece_Config& ps,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(
		Sub_Reader_Map<WDL_Entry> sub_wdl,
		In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTZ_Table>& table() const { return m_table; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

private:
	struct Checkpoint_File
	{
		static constexpr uint64_t MAGIC = 0x4B5A544454434843ull;  // 'CHCTDTZK'
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

	std::shared_ptr<DTZ_Table> m_table;
	Sub_Reader_Map<WDL_Entry> m_sub_wdl_by_material;

	template <typename EntryT>
	NODISCARD EntryT read_dtz(Color stm, Board_Index pos) const
	{
		return m_table->template read<EntryT>(stm, pos);
	}
	void write_dtz(Color stm, Board_Index pos, DTZ_Final_Entry e)
	{
		m_table->write(stm, pos, e);
	}
	void write_dtz(Color stm, Board_Index pos, DTZ_Intermediate_Entry e)
	{
		m_table->write(stm, pos, e);
		mark_iter(stm, pos, m_table->m_dtz[stm]);
	}

	NODISCARD WDL_Entry read_sub_tb(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move) const;

	NODISCARD DTZ_Any_Entry make_initial_entry(Position_For_Gen& pos_gen) const;
	bool init_entries(In_Out_Param<Thread_Pool> thread_pool);

	enum class Iter_Phase : uint8_t { CLEAN, CURSED };
	void iterate(In_Out_Param<Thread_Pool> thread_pool, bool pending_cursed,
	             Iter_Phase start_phase = Iter_Phase::CLEAN,
	             uint16_t finished_ply = 0);

	enum class Iter_Action : uint8_t {
		MARK_WIN_IN_1,
		MARK_WIN_PREDS,
		MARK_CHANGED,
		REVERIFY,
		PROMOTE_CWIN,
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
		uint16_t ply, DTZ_Intermediate_Entry hint) const;

	void retro_mark_win_in_1(Position_For_Gen& pos_gen);
	void retro_mark_changed(Position_For_Gen& pos_gen);
	template <Iter_Phase Phase>
	void retro_mark_wins(Position_For_Gen& pos_gen, uint16_t target_dtz);

	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);
};
