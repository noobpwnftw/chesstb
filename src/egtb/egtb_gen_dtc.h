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

// Layer k is DTZ with at most k winning-side pawn pushes. A winner's push reads
// k-1; a loser's push and quiet non-pawn moves stay at k, while zeroing moves
// read subtables. Save appends DTZ as the unbounded layer; probe selects the
// lowest layer compatible with the clock.
struct DTC_Interrupted
{
	uint16_t finished_ply;
	uint16_t max_dtc;
};

struct DTC_Table
{
	Piece_Config_For_Gen m_epsi;
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry> m_dtc[COLOR_NB][DTC_BUDGET_LAYERS];
	// Requested budget to stored layer, per batch.
	std::vector<uint8_t> m_slice_batch;
	std::vector<std::array<uint8_t, DTC_BUDGET_LAYERS>> m_batch_real;

	DTC_Table(const Piece_Config& ps, const std::filesystem::path& tmp_dir) :
		m_epsi(ps)
	{
		const size_t per = m_epsi.within_slice_size();
		const size_t np = m_epsi.num_positions();
		const size_t ns = (per == 0) ? 0 : (np / per);
		const std::string name = m_epsi.name();
		const uint64_t magic = static_cast<uint64_t>(EGTB_Magic::DTC_SLICE_MAGIC);
		for (size_t k = 0; k < DTC_BUDGET_LAYERS; ++k)
		{
			char w_fmt[128], b_fmt[128];
			std::snprintf(w_fmt, sizeof(w_fmt), "%s.w.k%02zu.%%05zu.dtcs", name.c_str(), k);
			std::snprintf(b_fmt, sizeof(b_fmt), "%s.b.k%02zu.%%05zu.dtcs", name.c_str(), k);
			m_dtc[WHITE][k].create(ns, per, tmp_dir, magic, w_fmt);
			m_dtc[BLACK][k].create(ns, per, tmp_dir, magic, b_fmt);
		}
		m_slice_batch.assign(m_epsi.pawn_slice_manager().num_slices(), 0);
		m_batch_real.assign(1, {});
	}

	NODISCARD size_t layer_of(size_t layer, size_t pid) const
	{
		const size_t k = std::min(layer, DTC_BUDGET_LAYERS - 1);
		return m_batch_real[m_slice_batch[pid]][k];
	}

	template <typename EntryT>
	NODISCARD EntryT read(Color stm, size_t layer, Board_Index pos) const
	{
		return m_dtc[stm][layer].template view_at<EntryT>(pos);
	}
	template <typename EntryT>
	NODISCARD EntryT read_at_budget(Color stm, size_t budget, size_t pid, Board_Index pos) const
	{
		return read<EntryT>(stm, layer_of(budget, pid), pos);
	}
	template <typename EntryT>
	void write(Color stm, size_t layer, Board_Index pos, EntryT e)
	{
		m_dtc[stm][layer].write(e, pos);
	}

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
		Table_Reader_Map<WDL_Entry> sub_wdl,
		Table_Reader_Map<DTC_Final_Entry> exit_dtc,
		In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

	NODISCARD const std::shared_ptr<DTC_Table>& table() const { return m_table; }

	void save_to_disk(In_Out_Param<Thread_Pool> thread_pool, const EGTB_Paths& paths);

private:
	struct Checkpoint_File
	{
		static constexpr uint64_t MAGIC = 0x4B43544443484843ull;  // 'CHCHDTCK'
		static constexpr uint32_t VERSION = 1;
		static constexpr size_t MAX_BATCHES = 40;
		static_assert(MAX_BATCHES >= (static_cast<size_t>(RANK_8) - static_cast<size_t>(RANK_2))
		                            * MAX_TOTAL_PAWNS + 1,
		              "a checkpoint must hold every batch");
		uint64_t magic = MAGIC;
		uint32_t version = VERSION;
		uint32_t batch_idx = 0;
		uint32_t fusion_idx = 0;
		uint16_t layer = 0;
		uint16_t finished_ply = 0;
		uint16_t max_dtc = 0;
		uint8_t  real[MAX_BATCHES + 1][DTC_BUDGET_LAYERS] = {};
		uint8_t  _pad[65] = {};
	};
	static_assert(sizeof(Checkpoint_File) == 1280, "Checkpoint_File size");

	std::shared_ptr<DTC_Table> m_table;
	Table_Reader_Map<WDL_Entry> m_sub_wdl_by_material;
	Table_Reader_Map<DTC_Final_Entry> m_exit_dtc_by_material;
	Table_Reader<DTC_Final_Entry>* m_exit_reader[COLOR_NB][Castling_Group::MAX_RIGHTS + 1]{};

	size_t m_layer = 0;
	size_t m_num_layers = 1;
	mutable bool m_layer_committed = false;

	template <typename EntryT>
	NODISCARD EntryT read_dtc(Color stm, Board_Index pos) const
	{
		return m_table->template read<EntryT>(stm, m_layer, pos);
	}
	template <typename EntryT>
	NODISCARD EntryT read_dtc_at(Color stm, size_t budget, size_t pid, Board_Index pos) const
	{
		return m_table->template read_at_budget<EntryT>(stm, budget, pid, pos);
	}
	void write_dtc(Color stm, Board_Index pos, DTC_Final_Entry e)
	{
		m_table->write(stm, m_layer, pos, e);
	}
	void write_dtc(Color stm, Board_Index pos, DTC_Intermediate_Entry e)
	{
		m_table->write(stm, m_layer, pos, e);
		mark_iter(stm, pos, m_table->m_dtc[stm][m_layer]);
	}

	void commit_if_new(Color stm, Board_Index pos, size_t pid, DTC_Final_Entry e) const
	{
		if (m_layer_committed || m_layer == 0) return;
		const DTC_Final_Entry below =
			read_dtc_at<DTC_Final_Entry>(stm, m_layer - 1, pid, pos);
		if (below.is_win() == e.is_win() && below.is_loss() == e.is_loss()
			&& below.value() == e.value())
			return;
		m_layer_committed = true;
	}

	NODISCARD bool active_layer_matches_dtz(In_Out_Param<Thread_Pool> thread_pool,
	                                        const DTZ_File_For_Probe& dtz, size_t layer);

	NODISCARD WDL_Entry read_sub_tb(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry read_post_move_wdl(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD WDL_Entry read_post_push_wdl(Position_For_Gen& pos_gen, Move move) const;
	NODISCARD DTC_Final_Entry read_exit_dtc(const Exit_Site& site,
	                                        size_t budget) const;
	NODISCARD WDL_Entry effective_opp_wdl_after_dp(Position_For_Gen& pos_gen, Move dp_move) const;

	// `layer_dependent` reports a verdict read off a pawn push or an exit, the
	// only children that move with the budget.
	NODISCARD DTC_Final_Entry make_initial_entry(Position_For_Gen& pos_gen,
	                                             Out_Param<bool> layer_dependent) const;
	NODISCARD uint16_t init_entries(In_Out_Param<Thread_Pool> thread_pool);

	void iterate(In_Out_Param<Thread_Pool> thread_pool, uint16_t finished_ply = 0);

	uint16_t m_max_dtc = 0;
	uint16_t m_layer_max_dtc = 0;

	struct Loss_Verification_Result {
		bool is_loss = false;
		uint16_t loss_dtz = 0;
	};

	NODISCARD bool run_iter(In_Out_Param<Thread_Pool> thread_pool, Color stm, uint16_t ply);

	NODISCARD Loss_Verification_Result check_loss(
		Position_For_Gen& pos_gen,
		uint16_t ply, DTC_Intermediate_Entry hint) const;

	void retro_mark_win_in_1(Position_For_Gen& pos_gen);
	void retro_mark_changed(Position_For_Gen& pos_gen);
	void retro_mark_wins(Position_For_Gen& pos_gen, uint16_t target_dtz);

	void page_in_for_group(In_Out_Param<Thread_Pool> thread_pool,
	                       Color me, size_t group_id);

	NODISCARD bool pin_layers(In_Out_Param<Thread_Pool> thread_pool,
	                          const std::vector<uint8_t>& groups, size_t ngroups, size_t count,
	                          const std::vector<size_t>& layers);
	void build_init_need(const std::vector<uint8_t>& write_groups, size_t ngroups,
	                     size_t g_start, size_t g_end, size_t spg);
	void mark_push_target_layers(uint8_t* need, size_t ngroups,
	                             size_t g_start, size_t g_end, size_t spg) const;

	struct Resume_Point
	{
		int64_t batch_idx = -1;
		int64_t layer = -1;
		int64_t fusion_idx = -1;
		uint16_t finished_ply = 0;
		uint16_t max_dtc = 0;
	};

	void claim_batch_slices(const std::vector<int32_t>& batch, size_t batch_row);

	NODISCARD size_t build_batch(In_Out_Param<Thread_Pool> thread_pool,
	                             const DTZ_File_For_Probe& dtz,
	                             const std::vector<int32_t>& batch, size_t batch_idx,
	                             size_t batch_row,
	                             size_t batches_total,
	                             const std::filesystem::path& ckpt_path,
	                             const Resume_Point& resume);
};
