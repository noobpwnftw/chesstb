#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/math.h"
#include "util/memory.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

Block_Ptr wdl_get_block(WDL_Per_Color& pc, size_t block_id)
{
	if (Block_Ptr cached = find_cached_block(pc.block_id, pc.data, pc.live, block_id))
		return cached;

	if (!pc.decomp)
		pc.decomp = std::make_unique<LZ4_Decompress_Helper>(pc.dict, pc.block_size);

	const auto pair = pc.offsets.get2(block_id);
	const size_t doff = pair[0];
	const size_t dsz  = pair[1] - pair[0];
	const size_t out_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const auto decompressed = pc.decomp->decompress(
		Const_Span<uint8_t>(pc.compressed_data + doff, dsz), out_sz);

	const size_t slot = next_cache_slot(pc.live, pc.next_slot);
	pc.block_id[slot] = block_id;
	pc.data[slot] = std::make_shared<const std::vector<uint8_t>>(
		decompressed.begin(), decompressed.end());
	return pc.data[slot];
}

}  // namespace

void WDL_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color& pc)
{
	pc.single_val = static_cast<WDL_Entry>(reader.read<uint8_t>());
}

void WDL_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                              const std::filesystem::path&)
{
	pc.tail_size   = reader.read<uint16_t>();
	pc.block_size  = reader.read<uint32_t>();
	pc.block_cnt   = reader.read<uint64_t>();
	pc.data_size   = reader.read<uint64_t>();
}

void WDL_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
                          const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
                          const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
                          const std::filesystem::path& path)
{
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		pc.dict_size = reader.read<uint16_t>();
		if (pc.dict_size != 0)
		{
			pc.lp_dict = reader.caret();
			reader.advance(pc.dict_size);
		}
	}

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
		Per_Color& pc = per_color[i];
		reader.align(64);
		pc.compressed_data = reader.caret();
		reader.advance(pc.data_size);
	}

	const size_t num_positions = Position_Index_Config(ps).num_positions();
	const size_t expected_uncompressed = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];

		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t file_sz  = pc.block_size * num_full + pc.tail_size;
		if (file_sz != expected_uncompressed)
			throw std::runtime_error("WDL decompressed size mismatch " + path.string());

		pc.dict = LZ4_Dict::load(Const_Span(pc.lp_dict, pc.lp_dict + pc.dict_size));
	}
}

WDL_Entry WDL_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos)
{
	if (is_singular) return pc.single_val;

	const size_t packed_byte = static_cast<size_t>(pos) / WDL_ENTRY_PACK_RATIO;
	const size_t block_id    = packed_byte / pc.block_size;
	const size_t in_block    = packed_byte % pc.block_size;

	const auto pair = pc.offsets.get2(block_id);
	if (pair[0] == pair[1])
		return WDL_Entry::ILLEGAL;

	const uint8_t* data = fetch_block_cached(pc, block_id, wdl_get_block);
	Packed_WDL_Entries entry;
	std::memcpy(&entry, data + in_block, sizeof(entry));
	return get_wdl_value(entry, static_cast<size_t>(pos) % WDL_ENTRY_PACK_RATIO);
}

template struct Table_File<WDL_Traits>;
