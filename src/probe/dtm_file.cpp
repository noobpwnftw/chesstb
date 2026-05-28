#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// DTM is the byte-for-byte twin of DTC; only the magic and the value decode
// differ.

namespace {

Block_Ptr dtm_get_block(Lzma_Rank_Per_Color& pc, size_t block_id)
{
	if (Block_Ptr cached = find_cached_block(pc.block_id, pc.data, pc.live, block_id))
		return cached;

	const size_t decode_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const size_t positions = decode_sz / pc.entry_bytes;

	const auto pair = pc.offsets.get2(block_id);
	const size_t doff = pair[0];
	const size_t dsz  = pair[1] - pair[0];

	const size_t slot = next_cache_slot(pc.live, pc.next_slot);
	pc.block_id[slot] = block_id;
	auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

	if (dsz != 0)
	{
		if (!pc.decomp)
			pc.decomp = std::make_unique<LZMA_Decompress_Helper>(pc.block_size);
		const Const_Span<uint8_t> raw = pc.decomp->decompress(
			Const_Span(pc.compressed_data + doff, dsz), decode_sz);

		const auto& r2v = pc.rank_to_value;
		uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
		if (pc.entry_bytes == 1)
		{
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[raw[k]];
		}
		else
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[in[k]];
		}
	}

	pc.data[slot] = buf;
	return pc.data[slot];
}

}  // namespace

void DTM_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color&)
{
	const WDL_Entry sv = static_cast<WDL_Entry>(reader.read<uint8_t>());
	if (sv != WDL_Entry::DRAW)
		throw std::runtime_error("DTM singular value must be DRAW");
}

void DTM_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                              const std::filesystem::path& path)
{
	pc.entry_bytes = reader.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error("Bad DTM entry_bytes " + path.string());
	pc.tail_size  = reader.read<uint32_t>();
	pc.block_size = reader.read<uint32_t>();
	pc.block_cnt  = reader.read<uint64_t>();
	pc.data_size  = reader.read<uint64_t>();

	const size_t num_ranks = reader.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t r = 0; r < num_ranks; ++r)
		pc.rank_to_value[r] = reader.read<uint16_t>();
}

void DTM_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
                          const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
                          const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
                          const std::filesystem::path& path)
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

	const size_t num_positions = Position_Index_Config(ps).num_positions();
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTM decompressed size mismatch " + path.string());
	}
}

uint16_t DTM_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl)
{
	if (wdl == WDL_Entry::DRAW || wdl == WDL_Entry::ILLEGAL) return 0;
	if (is_singular) return 0;

	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1])
	{
		// Skip-block: see DTC_Traits::read.
		return 0;
	}

	const uint8_t* data = fetch_block_cached(pc, block_id, dtm_get_block);
	uint16_t stored;
	std::memcpy(&stored, data + in_block_pos * sizeof(uint16_t), sizeof(stored));
	return dtm_value_from_storage(stored, wdl);
}

template struct Table_File<DTM_Traits>;
