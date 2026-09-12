#include "probe/layered_file.h"
#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/memory.h"

#include <cstdint>
#include <stdexcept>

// The value at `hmc`, and the layer where the cell turns DRAW; the terminal rank
// decides both. Only the draw-end hint says DRAW; a stored 0 is a mate.
DTM50_Cell DTM50_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos,
                              WDL_Entry wdl, unsigned hmc)
{
	const bool flat = (hmc == IGNORE_50MR);
	if (!flat && (wdl == WDL_Entry::CURSED_WIN || wdl == WDL_Entry::BLESSED_LOSS))
		return {};
	if (is_singular) return {};

	Layered_Entry_Ref ref;
	if (!layered_locate_entry<DTM50_Traits>(pc, pos, ref)) return {};

	const size_t eb = pc.entry_bytes;

	const uint16_t layer = flat ? 0u : static_cast<uint16_t>(hmc + 1);
	uint16_t stored;
	uint16_t draw_flip = 0;
	switch (ref.state)
	{
		case 0:
			// CONST: one rank for all hmc.
			stored = rank_value(pc, ref.entry);
			break;
		case 1: {
			// SINGLE: one transition at h.
			const uint16_t h = ref.entry[0] & 0x7Fu;
			if      (ref.draw_end) draw_flip = h;
			if      (layer < h)    stored = rank_value(pc, ref.entry + 1);
			else if (ref.draw_end) return { DRAWN_AT_LAYER, draw_flip };
			else                   stored = rank_value(pc, ref.entry + 1 + eb);
			break;
		}
		case 2: {
			// DOUBLE: transitions at h1 < h2.
			const uint16_t h1 = ref.entry[0];
			const uint16_t h2 = ref.entry[1] & 0x7Fu;
			if      (ref.draw_end) draw_flip = h2;
			if      (layer < h1)   stored = rank_value(pc, ref.entry + 2);
			else if (layer < h2)   stored = rank_value(pc, ref.entry + 2 + eb);
			else if (ref.draw_end) return { DRAWN_AT_LAYER, draw_flip };
			else                   stored = rank_value(pc, ref.entry + 2 + 2 * eb);
			break;
		}
		default: {
			// MULTI: changepoint bitmap; rsel = popcount(bits ≤ layer) - 1.
			const size_t k = ref.entry[0] & 0x7Fu;
			constexpr size_t bm = DTM50_MULTI_BITMAP_BYTES;
			const size_t rsel = layered_multi_rank_slot<bm>(ref.entry, layer);
			if (ref.draw_end) draw_flip = layered_multi_last_changepoint<bm>(ref.entry);
			if (rsel == k - 1 && ref.draw_end) return { DRAWN_AT_LAYER, draw_flip };
			stored = rank_value(pc, ref.entry + 1 + bm + rsel * eb);
			break;
		}
	}

	const uint16_t value = flat
		? dtm_value_from_storage(stored, wdl)
		: dtm50_value_from_storage(stored, wdl);
	return { value, draw_flip };
}

template struct Table_File<DTM50_Traits>;
