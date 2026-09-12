#pragma once

#include "transcribe/source_tables.h"

#include "egtb/egtb_entry.h"
#include "egtb/egtb_gen.h"
#include "egtb/egtb_probe.h"
#include "egtb/relax_bound.h"

#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/param.h"
#include "util/thread_pool.h"

#include <optional>

// A winning position has DTZ 1 if a zeroing move preserves its WDL class.
// Relaxed DTZ tables omit those values; the prober repeats the same WDL test.
// No child DTZ value is needed.
struct Relax_DTZ : private EGTB_Generator
{
	Relax_DTZ(const Piece_Config& ps, const EGTB_Paths& paths,
	          In_Out_Param<Thread_Pool> thread_pool, const Source_WDL& own_wdl) :
		EGTB_Generator(ps),
		m_sub_wdl(open_probes<WDL_File_For_Probe>(
			enumerate_sub_materials(ps), paths, thread_pool)),
		m_own_wdl(&own_wdl) {}

	using EGTB_Generator::epsi;

	// `pos_gen` must already be positioned at `idx`.
	NODISCARD bool can_omit(Position_For_Gen& pos_gen, Color color, Board_Index idx) const
	{
		const WDL_Entry cell = wdl_from_storage(m_own_wdl->read(color, idx));
		if (cell != WDL_Entry::WIN && cell != WDL_Entry::CURSED_WIN) return false;

		Position& board = pos_gen.board_unchecked();
		return board.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
			const bool is_cap   = board.move_is_capture(m);
			const bool is_promo = m.is_promotion();
			const bool is_push  = !is_cap && !is_promo
			                   && piece_type(board.piece_at(m.from())) == PAWN;
			if (!is_cap && !is_promo && !is_push) return false;

			const WDL_Entry child = (is_cap || is_promo)
				? read_sub_wdl(pos_gen, m)
				: (is_pawn_double_push(m) ? own_wdl_after_double_push(pos_gen, m)
				                          : own_wdl_after_push(pos_gen, m));
			return invert_wdl_entry(child) == cell;
		});
	}

private:
	Table_Reader_Map<WDL_Entry> m_sub_wdl;
	const Source_WDL* m_own_wdl;

	NODISCARD WDL_Entry read_sub_wdl(Position_For_Gen& pos_gen, Move move) const
	{
		Color sub_color;
		const Piece_Config_For_Gen* sub_epsi = nullptr;
		const Board_Index sub_idx =
			next_sub_index(pos_gen, move, out_param(sub_color), out_param(sub_epsi));

		if (sub_epsi == nullptr) return WDL_Entry::DRAW;
		auto it = m_sub_wdl.find(sub_epsi->min_material_key());
		if (it == m_sub_wdl.end()) return WDL_Entry::DRAW;
		return it->second->read(sub_color, sub_idx);
	}

	NODISCARD WDL_Entry own_wdl_after_push(Position_For_Gen& pos_gen, Move move) const
	{
		const Color opp = color_opp(pos_gen.board_unchecked().turn());
		return wdl_from_storage(m_own_wdl->read(opp, next_quiet_index(pos_gen, move)));
	}

	// The table index has no en-passant state. Include every legal en-passant
	// reply and use the result the opponent would choose.
	NODISCARD WDL_Entry own_wdl_after_double_push(Position_For_Gen& pos_gen, Move dp_move) const
	{
		const WDL_Entry no_ep = own_wdl_after_push(pos_gen, dp_move);

		Position& p = pos_gen.board_unchecked();
		const Color opp = color_opp(p.turn());
		const uint8_t rights_before = p.castling();
		const Square ep_sq = ep_square_of_double_push(dp_move);
		const Piece captured_by_dp = p.do_move(dp_move);

		WDL_Entry best_for_opp = WDL_Entry::LOSE;
		bool any_ep = false;
		std::optional<Position_For_Gen> child_gen;

		(void)p.visit_legal_ep_captures(ep_sq, [&](Move ep_move) FORCE_INLINE_LAMBDA {
			if (!child_gen)
				child_gen.emplace(m_epsi, board_index_of_position(m_epsi, p), opp);
			const WDL_Entry after = read_sub_wdl(*child_gen, ep_move);
			if (after == WDL_Entry::ILLEGAL) return false;
			const WDL_Entry for_opp = invert_wdl_entry(after);
			if (wdl_class_rank(for_opp) > wdl_class_rank(best_for_opp)) best_for_opp = for_opp;
			any_ep = true;
			return false;
		});

		p.undo_move(dp_move, captured_by_dp, rights_before);

		if (!any_ep) return no_ep;
		return wdl_class_rank(best_for_opp) > wdl_class_rank(no_ep) ? best_for_opp : no_ep;
	}
};
