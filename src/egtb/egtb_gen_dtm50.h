#pragma once

#include "egtb/egtb_compress.h"
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

// DTM50 classifies HMC layers from 99 down. Quiet moves read hmc+1, pawn pushes
// read hmc=0 in an earlier slice, and zeroing moves read subtables. Legality is
// shared by all HMC layers.

struct DTM50_Interrupted
{
	uint32_t batch_idx;
	uint32_t fusion_idx;
	uint16_t hmc;
};

struct DTM50_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry> m_dtm[COLOR_NB][DTM50_HMC_COUNT];

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
	}

	template <typename EntryT>
	NODISCARD EntryT read(Color stm, uint16_t hmc, Board_Index pos) const
	{
		return m_dtm[stm][hmc].template view_at<EntryT>(pos);
	}
	template <typename EntryT>
	void write(Color stm, uint16_t hmc, Board_Index pos, EntryT e)
	{
		m_dtm[stm][hmc].write(e, pos);
	}

	DTM50_Table(const DTM50_Table&) = delete;
	DTM50_Table& operator=(const DTM50_Table&) = delete;
};

class DTM50_Generator : public EGTB_Generator
{
public:
	DTM50_Generator(
		const Piece_Config& ps,
		const std::filesystem::path& tmp_dir,
		size_t budget_bytes);

	void gen(
		Table_Reader_Map<DTM50_Final_Entry> sub_dtm,
		Table_Reader_Map<DTM50_Final_Entry> exit_dtm,
		In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTM50_Table>& table() const { return m_table; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

private:
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

	std::shared_ptr<DTM50_Table> m_table;
	Table_Reader_Map<DTM50_Final_Entry> m_sub_dtm_by_material;
	Table_Reader_Map<DTM50_Final_Entry> m_exit_dtm_by_material;
	Table_Reader<DTM50_Final_Entry>* m_exit_reader[COLOR_NB][Castling_Group::MAX_RIGHTS + 1]{};

	template <typename EntryT>
	NODISCARD EntryT read_dtm50(Color stm, uint16_t hmc, Board_Index pos) const
	{
		return m_table->template read<EntryT>(stm, hmc, pos);
	}
	template <typename EntryT>
	void write_dtm50(Color stm, uint16_t hmc, Board_Index pos, EntryT e)
	{
		m_table->write(stm, hmc, pos, e);
	}

	NODISCARD DTM50_Final_Entry read_sub_tb(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD DTM50_Final_Entry read_exit_dtm(const Exit_Site& site,
	                                          uint16_t target_hmc) const;
	NODISCARD DTM50_Final_Entry read_post_move_dtm(Position_For_Gen& pos_gen, Move move, uint16_t hmc) const;
	NODISCARD DTM50_Final_Entry effective_opp_dtm_after_dp(Position_For_Gen& pos_gen, Move dp_move, uint16_t hmc) const;

	NODISCARD DTM50_Final_Entry make_initial_entry(Position_For_Gen& pos_gen, uint16_t hmc);
	NODISCARD DTM50_Final_Entry make_layer_entry(Position_For_Gen& pos_gen, DTM50_Intermediate_Entry inv, uint16_t hmc) const;
	void init_entries(In_Out_Param<Thread_Pool> thread_pool);
	template <Color stm>
	void build_layer(In_Out_Param<Thread_Pool> thread_pool, uint16_t hmc);
	template <Color me>
	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool, size_t group_id, uint16_t hmc);
};
