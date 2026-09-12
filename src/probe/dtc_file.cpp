#include "probe/layered_file.h"
#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

struct Budget_Segments
{
	static constexpr size_t MAX = DTC_PACK_LAYERS;

	uint16_t start[MAX];   // first pack index the segment covers
	uint16_t value[MAX];
	size_t count = 0;
	bool draw_end = false;
};

void decode_segments(const Layered_Rank_Per_Color& pc, const Layered_Entry_Ref& ref,
                     Budget_Segments& out)
{
	const size_t eb = pc.entry_bytes;
	out.draw_end = ref.draw_end;
	switch (ref.state)
	{
		case 0:
			out.start[0] = 0;
			out.value[0] = layered_rank_value(pc, ref.entry);
			out.count = 1;
			break;
		case 1:
			out.start[0] = 0;
			out.value[0] = layered_rank_value(pc, ref.entry + 1);
			out.start[1] = static_cast<uint16_t>(ref.entry[0] & 0x7Fu);
			out.count = 2;
			if (!ref.draw_end)
				out.value[1] = layered_rank_value(pc, ref.entry + 1 + eb);
			break;
		case 2:
			out.start[0] = 0;
			out.value[0] = layered_rank_value(pc, ref.entry + 2);
			out.start[1] = ref.entry[0];
			out.value[1] = layered_rank_value(pc, ref.entry + 2 + eb);
			out.start[2] = static_cast<uint16_t>(ref.entry[1] & 0x7Fu);
			out.count = 3;
			if (!ref.draw_end)
				out.value[2] = layered_rank_value(pc, ref.entry + 2 + 2 * eb);
			break;
		default:
		{
			const size_t k = ref.entry[0] & 0x7Fu;
			const auto words = layered_multi_bitmap<DTC_MULTI_BITMAP_BYTES>(ref.entry);
			const uint8_t* ranks = ref.entry + 1 + DTC_MULTI_BITMAP_BYTES;
			size_t n = 0;
			for (size_t w = 0; w < words.size(); ++w)
			{
				uint64_t bits = words[w];
				while (bits != 0)
				{
					const size_t b = static_cast<size_t>(__builtin_ctzll(bits));
					bits &= bits - 1;
					ASSERT(n < k);
					out.start[n] = static_cast<uint16_t>(w * 64 + b);
					// The hint omits the last rank, DRAW standing in for it.
					if (!(ref.draw_end && n == k - 1))
						out.value[n] = layered_rank_value(pc, ranks + n * eb);
					++n;
				}
			}
			ASSERT(n == k);
			out.count = k;
			break;
		}
	}
}

// False where nothing is stored, which reads as drawn at every budget.
NODISCARD bool decode_position(Layered_Rank_Per_Color& pc, bool is_singular, Board_Index pos,
                               Budget_Segments& out)
{
	if (is_singular) return false;

	Layered_Entry_Ref ref;
	if (!layered_locate_entry<DTC_Traits>(pc, pos, ref)) return false;
	decode_segments(pc, ref, out);
	return true;
}

NODISCARD size_t budget_of_pack_index(size_t j)
{
	return DTC_BUDGET_LAYERS - j;
}

}  // namespace

void DTC_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color&)
{
	if (reader.read<uint8_t>() != 0)
		throw std::runtime_error("DTC singular value must be DRAW");
}

void DTC_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                              const Position_Index_Config& index_cfg,
                              const std::filesystem::path& path)
{
	const uint32_t perm = reader.read<uint32_t>();
	if (!index_permutation_config_is_valid(index_cfg, perm))
		throw std::runtime_error("Invalid DTC index permutation config " + path.string());
	pc.layout = make_index_storage_layout(index_cfg, perm);

	pc.entry_bytes = reader.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error("Bad DTC entry_bytes " + path.string());
	pc.block_positions = reader.read<uint32_t>();
	pc.block_cnt       = reader.read<uint64_t>();
	pc.tail_positions  = reader.read<uint32_t>();
	pc.data_size       = reader.read<uint64_t>();

	const size_t num_ranks = reader.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t r = 0; r < num_ranks; ++r)
		pc.rank_to_value[r] = reader.read<uint16_t>();
}

void DTC_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
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

DTC_Cell DTC_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos,
                          WDL_Entry wdl, unsigned rule50)
{
	ASSERT(wdl != WDL_Entry::DRAW && wdl != WDL_Entry::ILLEGAL);
	Budget_Segments seg;
	if (!decode_position(pc, is_singular, pos, seg)) return {};

	DTC_Cell out;
	out.dtz = seg.value[0];
	if (wdl == WDL_Entry::CURSED_WIN || wdl == WDL_Entry::BLESSED_LOSS) return out;

	const unsigned budget_plies = dtc_budget_plies(rule50);

	const size_t decisive = seg.draw_end ? seg.count - 1 : seg.count;
	for (size_t s = decisive; s-- > 0; )
	{
		if (seg.value[s] > budget_plies) continue;
		const size_t last_index = (s + 1 < seg.count)
			? static_cast<size_t>(seg.start[s + 1]) - 1
			: DTC_PACK_LAYERS - 1;
		out.order = static_cast<uint16_t>(budget_of_pack_index(last_index));
		out.value = seg.value[s];
		break;
	}
	return out;
}

void DTC_Traits::read_curve(Per_Color& pc, bool is_singular, Board_Index pos,
                            WDL_Entry wdl, Out_Param<DTC_Curve> curve)
{
	for (uint16_t& v : curve->value) v = DTC_Cell::DRAWN;

	ASSERT(wdl != WDL_Entry::DRAW && wdl != WDL_Entry::ILLEGAL);
	if (wdl == WDL_Entry::CURSED_WIN || wdl == WDL_Entry::BLESSED_LOSS) return;

	Budget_Segments seg;
	if (!decode_position(pc, is_singular, pos, seg)) return;

	const size_t decisive = seg.draw_end ? seg.count - 1 : seg.count;
	for (size_t s = 0; s < decisive; ++s)
	{
		const size_t end = (s + 1 < seg.count) ? seg.start[s + 1] : DTC_PACK_LAYERS;
		for (size_t j = seg.start[s]; j < end; ++j)
			curve->value[budget_of_pack_index(j)] = seg.value[s];
	}
}

template struct Table_File<DTC_Traits>;
