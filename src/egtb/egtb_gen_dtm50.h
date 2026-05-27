#pragma once

#include "egtb/egtb_compress.h"
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

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

// DTM50 = plies-to-mate under 50MR. 100 per-hmc layers per color.
//
// Forward classification, one pass per (slice, hmc). Pawn slices in topo order;
// hmc loop runs 99..0. Every read targets an already-finalized cell:
//   - non-pawn quiet at hmc=k → opp[k+1] same slice (k=99 falls back to inline
//     mate check standing in for virtual hmc=100 = 50MR draw),
//   - pawn push at any hmc → opp[0] of a push-destination slice in a prior fusion,
//   - cap/promo → sub-tablebase.
//
// Only hmc=99 runs full chess-legality per cell; lower hmcs inherit ILLEGAL
// from opp[k+1] at the same idx.

struct DTM50_Interrupted
{
	uint32_t batch_idx;
	uint32_t fusion_idx;
	uint16_t hmc;
};

struct DTM50_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry> m_dtm[COLOR_NB][DTM50_HMC_COUNT];
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

	NODISCARD INLINE DTM_Final_Entry read_dtm(Board_Index pos, Color stm, uint16_t hmc) const
	{
		return m_table->m_dtm[stm][hmc].read(pos);
	}
	INLINE void write_dtm(Board_Index pos, Color stm, uint16_t hmc, DTM_Final_Entry e)
	{
		m_table->m_dtm[stm][hmc].write(e, pos);
	}

	NODISCARD DTM_Final_Entry read_sub_tb(const Position_For_Gen& pos_gen, Move move, size_t thread_id) const;
	NODISCARD DTM_Final_Entry read_post_move_dtm(const Position_For_Gen& pos_gen, Move move, uint16_t hmc, size_t thread_id) const;
	NODISCARD DTM_Final_Entry effective_opp_dtm_after_dp(const Position_For_Gen& pos_gen, Move dp_move, uint16_t hmc, size_t thread_id) const;

	NODISCARD DTM_Final_Entry make_initial_entry(Position_For_Gen& pos_gen, uint16_t hmc, size_t thread_id) const;
	void init_entries(In_Out_Param<Thread_Pool> thread_pool, uint16_t hmc);
};
