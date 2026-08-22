#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <cstdint>
#include <stdexcept>

void DTZ_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color& pc)
{
	pc.single_val = reader.read<uint8_t>();
}

void DTZ_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                              const Position_Index_Config& index_cfg,
                              const std::filesystem::path& path)
{
	const uint32_t perm = reader.read<uint32_t>();
	if (!index_permutation_config_is_valid(index_cfg, perm))
		throw std::runtime_error("Invalid DTZ index permutation config " + path.string());
	pc.layout = make_index_storage_layout(index_cfg, perm);

	pc.entry_bytes = reader.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error("Bad DTZ entry_bytes " + path.string());
	pc.tail_size  = reader.read<uint32_t>();
	pc.block_size = reader.read<uint32_t>();
	pc.block_cnt  = reader.read<uint64_t>();
	pc.data_size  = reader.read<uint64_t>();

	const size_t num_ranks = reader.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t r = 0; r < num_ranks; ++r)
		pc.rank_to_value[r] = reader.read<uint16_t>();
}

void DTZ_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
                          const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
                          const Fixed_Vector<Color, 2>& table_colors)
{
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1,
		                           log2_bu, sample_width, offset_width);
	}
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(per_color[i].data_size);
	}
}

uint16_t DTZ_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl)
{
	ASSERT(wdl != WDL_Entry::DRAW && wdl != WDL_Entry::ILLEGAL);
	if (is_singular) return dtz_value_from_storage(pc.single_val, wdl, 1);

	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1])
	{
		// Skip-block: uniform DRAW/ILLEGAL. The WDL class has already
		// filtered those out; W/L would have forced a non-zero cell.
		return 0;
	}

	const uint8_t* data = fetch_block_cached<DTZ_Traits>(pc, block_id, lzma_rank_get_block);
	return dtz_value_from_storage(lzma_rank_value(pc, data, in_block_pos), wdl, pc.entry_bytes);
}

template struct Table_File<DTZ_Traits>;
