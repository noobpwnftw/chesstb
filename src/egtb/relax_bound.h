#pragma once

#include "egtb/egtb_compress.h"
#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"

#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/param.h"
#include "util/thread_pool.h"

#include <memory>

// Generator and transcribe share this one definition -- the cells must be capped
// against exactly the bound the prober recomputes.

NODISCARD INLINE WDL_Entry invert_wdl_entry(WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::WIN:          return WDL_Entry::LOSE;
		case WDL_Entry::CURSED_WIN:   return WDL_Entry::BLESSED_LOSS;
		case WDL_Entry::DRAW:         return WDL_Entry::DRAW;
		case WDL_Entry::BLESSED_LOSS: return WDL_Entry::CURSED_WIN;
		case WDL_Entry::LOSE:         return WDL_Entry::WIN;
		case WDL_Entry::ILLEGAL:      return WDL_Entry::ILLEGAL;
	}
	return WDL_Entry::ILLEGAL;
}

NODISCARD INLINE bool is_relaxable_class(WDL_Stored s)
{
	return s == WDL_Stored::BLESSED_LOSS || s == WDL_Stored::DRAW
	    || s == WDL_Stored::CURSED_WIN   || s == WDL_Stored::WIN;
}

struct Relax_Bound : private EGTB_Generator
{
	Relax_Bound(const Piece_Config& ps, const EGTB_Paths& paths,
	              In_Out_Param<Thread_Pool> thread_pool) :
		EGTB_Generator(ps),
		m_sub_wdl(open_sub_probes<WDL_File_For_Probe>(ps, paths, thread_pool)) {}

	using EGTB_Generator::epsi;

	NODISCARD WDL_Entry operator()(Position_For_Gen& pos_gen) const
	{
		const Position& board = pos_gen.board_unchecked();
		Move_List ml;
		board.gen_pseudo_legal_moves(out_param(ml));
		const Position::Legality ctx = board.legality_context();

		WDL_Entry best = WDL_Entry::ILLEGAL;
		for (size_t i = 0; i < ml.size(); ++i)
		{
			const Move m = ml[i];
			if (!m.is_promotion() && board.is_empty(m.to())) continue;
			if (!board.is_pseudo_legal_move_legal(m, ctx)) continue;

			const WDL_Entry mine = invert_wdl_entry(read_sub_wdl(pos_gen, m));
			if (wdl_class_rank(mine) > wdl_class_rank(best)) best = mine;
		}
		return best;
	}

	NODISCARD WDL_Stored cap_for(Position_For_Gen& pos_gen, WDL_Stored v) const
	{
		if (!is_relaxable_class(v)) return NOT_RELAXED;
		return wdl_class_rank((*this)(pos_gen)) >= wdl_class_rank(v) ? v : NOT_RELAXED;
	}

private:
	Sub_Reader_Map<WDL_Entry> m_sub_wdl;

	NODISCARD WDL_Entry read_sub_wdl(Position_For_Gen& pos_gen, Move move) const
	{
		Color sub_color;
		const Piece_Config_For_Gen* sub_epsi = nullptr;
		const Board_Index sub_idx =
			next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

		// open_sub_probes skips bare kings and throws on anything else missing.
		if (sub_epsi == nullptr) return WDL_Entry::DRAW;
		auto it = m_sub_wdl.find(sub_epsi->min_material_key());
		if (it == m_sub_wdl.end()) return WDL_Entry::DRAW;
		return it->second->read(sub_color, sub_idx);
	}
};
