#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/memory.h"

#include <cstdint>
#include <stdexcept>

DTZ_Value DTZ_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl)
{
	if (is_singular) return dtz_value_from_storage(pc.single_val, wdl, 1);

	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos / positions_per_block);
	const size_t in_block_pos = static_cast<size_t>(pos % positions_per_block);

	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1])
	{
		// Skip-block: uniform DRAW/ILLEGAL. The WDL class has already
		// filtered those out; W/L would have forced a non-zero cell.
		return DTZ_Value{ 0, false };
	}

	const uint8_t* data = fetch_block_cached<DTZ_Traits>(pc, block_id, lzma_rank_get_block);
	return dtz_value_from_storage(rank_value(pc, data + in_block_pos * pc.entry_bytes), wdl, pc.entry_bytes);
}

template struct Table_File<DTZ_Traits>;
