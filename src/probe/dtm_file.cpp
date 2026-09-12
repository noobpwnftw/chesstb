#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <cstdint>
#include <stdexcept>

uint16_t DTM_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl)
{
	if (is_singular) return dtm_value_from_storage(pc.single_val, wdl);

	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1])
	{
		// Skip-block: see DTZ_Traits::read.
		return 0;
	}

	const uint8_t* data = fetch_block_cached<DTM_Traits>(pc, block_id, lzma_rank_get_block);
	return dtm_value_from_storage(rank_value(pc, data + in_block_pos * pc.entry_bytes), wdl);
}

template struct Table_File<DTM_Traits>;
