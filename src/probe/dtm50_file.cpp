#include "probe/layered_file.h"
#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <cstdint>
#include <stdexcept>

void DTM50_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color&)
{
	if (reader.read<uint8_t>() != 0)
		throw std::runtime_error("DTM50 singular value must be DRAW");
}

void DTM50_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                                const Position_Index_Config& index_cfg,
                                const std::filesystem::path& path)
{
	const uint32_t perm = reader.read<uint32_t>();
	if (!index_permutation_config_is_valid(index_cfg, perm))
		throw std::runtime_error("Invalid DTM50 index permutation config " + path.string());
	pc.layout = make_index_storage_layout(index_cfg, perm);

	pc.entry_bytes = reader.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error("Bad DTM50 entry_bytes " + path.string());
	pc.block_positions = reader.read<uint32_t>();
	pc.block_cnt       = reader.read<uint64_t>();
	pc.tail_positions  = reader.read<uint32_t>();
	pc.data_size       = reader.read<uint64_t>();

	const size_t num_ranks = reader.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t r = 0; r < num_ranks; ++r)
		pc.rank_to_value[r] = reader.read<uint16_t>();
}

void DTM50_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
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
		const uint8_t usz_width    = reader.read<uint8_t>();
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		const uint8_t* usz_ptr = reader.caret();
		const size_t usz_bytes = Min0_Uint_Vec::on_disk_bytes(pc.block_cnt, usz_width);
		reader.advance(usz_bytes);
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1,
		                           log2_bu, sample_width, offset_width);
		pc.usizes = Min0_Uint_Vec(usz_ptr, pc.block_cnt, usz_width);
	}
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(per_color[i].data_size);
	}
}

// The value at `hmc`, and the layer where the cell turns DRAW; the terminal rank
// decides both. Only the draw-end hint says DRAW; a stored 0 is a mate.
DTM50_Cell DTM50_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos,
                              WDL_Entry wdl, unsigned hmc)
{
	ASSERT(wdl != WDL_Entry::DRAW && wdl != WDL_Entry::ILLEGAL);
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
			stored = layered_rank_value(pc, ref.entry);
			break;
		case 1: {
			// SINGLE: one transition at h.
			const uint16_t h = ref.entry[0] & 0x7Fu;
			if      (ref.draw_end) draw_flip = h;
			if      (layer < h)    stored = layered_rank_value(pc, ref.entry + 1);
			else if (ref.draw_end) return { DTM50_Cell::DRAWN, draw_flip };
			else                   stored = layered_rank_value(pc, ref.entry + 1 + eb);
			break;
		}
		case 2: {
			// DOUBLE: transitions at h1 < h2.
			const uint16_t h1 = ref.entry[0];
			const uint16_t h2 = ref.entry[1] & 0x7Fu;
			if      (ref.draw_end) draw_flip = h2;
			if      (layer < h1)   stored = layered_rank_value(pc, ref.entry + 2);
			else if (layer < h2)   stored = layered_rank_value(pc, ref.entry + 2 + eb);
			else if (ref.draw_end) return { DTM50_Cell::DRAWN, draw_flip };
			else                   stored = layered_rank_value(pc, ref.entry + 2 + 2 * eb);
			break;
		}
		default: {
			// MULTI: changepoint bitmap; rsel = popcount(bits ≤ layer) - 1.
			const size_t k = ref.entry[0] & 0x7Fu;
			constexpr size_t bm = DTM50_MULTI_BITMAP_BYTES;
			const size_t rsel = layered_multi_rank_slot<bm>(ref.entry, layer);
			if (ref.draw_end) draw_flip = layered_multi_last_changepoint<bm>(ref.entry);
			if (rsel == k - 1 && ref.draw_end) return { DTM50_Cell::DRAWN, draw_flip };
			stored = layered_rank_value(pc, ref.entry + 1 + bm + rsel * eb);
			break;
		}
	}

	const uint16_t value = flat
		? dtm_value_from_storage(stored, wdl)
		: dtm50_value_from_storage(stored, wdl);
	return { value, draw_flip };
}

template struct Table_File<DTM50_Traits>;
