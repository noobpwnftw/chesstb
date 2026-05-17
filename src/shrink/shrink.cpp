// Shrink EGTB files for shipping.
//
// Usage:
//   ./shrink path/to/file.lzdtc path/to/file.lzdtm path/to/file.lzw ...
//   ./shrink path/to/dir/*       # shell glob
//
// Each file is detected by magic. The larger side-to-move table is dropped
// when it can be rederived by the probe library from the kept side and sub-TBs.
// Already-shrunk and non-derivable files are skipped.
//
// Writes "<path>.shrink_tmp" and renames it over the original.

#include "egtb/egtb_entry.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/memory.h"
#include "util/span.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr uint8_t SINGULAR_FLAG = 0x80;
constexpr uint8_t DROPPED_FLAG  = 0x40;
constexpr uint64_t CHECKSUM_INIT = 0xf0f0f0f0f0f0;

INLINE bool flag_is_normal(uint8_t flag)
{
	return (flag & (SINGULAR_FLAG | DROPPED_FLAG)) == 0;
}

// Select the color table to drop. Returns COLOR_NB when nothing is droppable:
//   - symmetric materials (only one color in table_colors)
//   - both colors already non-normal (singular/dropped)
//   - the only normal color's sibling is already dropped
// Normal pairs drop the larger table. A normal table opposite a singular one is
// still derivable, so the normal table can be dropped.
template <typename ColorInfo>
NODISCARD Color pick_drop_color(const ColorInfo info[COLOR_NB],
                                const Fixed_Vector<Color, 2>& table_colors)
{
	std::vector<Color> normals;
	for (Color c : table_colors)
		if (info[c].present && flag_is_normal(info[c].flag))
			normals.push_back(c);

	if (normals.size() == 2)
	{
		return (info[normals[0]].data_size >= info[normals[1]].data_size)
			? normals[0] : normals[1];
	}
	if (normals.size() == 1 && table_colors.size() == 2)
	{
		const Color other = (normals[0] == WHITE) ? BLACK : WHITE;
		if (info[other].present && (info[other].flag & SINGULAR_FLAG))
			return normals[0];
	}
	return COLOR_NB;
}

// Rank-encoded .lzdtc/.lzdtm files share this layout.
struct Rank_Color_Info
{
	bool present = false;
	uint8_t flag = 0;
	uint8_t single_val = 0;
	// Present when flag_is_normal(flag).
	uint8_t entry_bytes = 0;
	uint32_t tail_size = 0;
	uint32_t block_size = 0;
	uint32_t block_cnt = 0;
	uint64_t data_size = 0;
	uint16_t num_ranks = 0;
	const uint8_t* rank_table_ptr = nullptr;
	const uint8_t* offset_tb_ptr  = nullptr;
	const uint8_t* data_ptr       = nullptr;
};

NODISCARD bool parse_rank_encoded(const Const_Span<uint8_t>& bytes,
                                  EGTB_Magic expected_magic,
                                  Rank_Color_Info info[COLOR_NB],
                                  uint32_t* key_and_table_num,
                                  Fixed_Vector<Color, 2>* out_table_colors)
{
	Serial_Memory_Reader r(bytes);
	if (!r.is_end_checksum_ok(CHECKSUM_INIT)) return false;
	const uint32_t magic = r.read<uint32_t>();
	if (magic != static_cast<uint32_t>(expected_magic)) return false;
	*key_and_table_num = r.read<uint32_t>();
	const size_t table_num = *key_and_table_num & 3;
	*out_table_colors = egtb_table_colors(table_num);

	for (Color c : *out_table_colors)
	{
		info[c].present = true;
		const uint8_t flag = r.read<uint8_t>();
		info[c].flag = flag;
		if (flag & SINGULAR_FLAG)
		{
			info[c].single_val = r.read<uint8_t>();
		}
		else if (flag & DROPPED_FLAG)
		{
			// Dropped colors have no payload.
		}
		else
		{
			info[c].entry_bytes = r.read<uint8_t>();
			info[c].tail_size   = r.read<uint32_t>();
			info[c].block_size  = r.read<uint32_t>();
			info[c].block_cnt   = r.read<uint32_t>();
			info[c].data_size   = r.read<uint64_t>();
			info[c].num_ranks   = r.read<uint16_t>();
			info[c].rank_table_ptr = r.caret();
			r.advance(info[c].num_ranks * 2);
		}
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		info[c].offset_tb_ptr = r.caret();
		r.advance(info[c].block_cnt * 8);
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		r.align(64);
		info[c].data_ptr = r.caret();
		r.advance(info[c].data_size);
	}

	return true;
}

NODISCARD size_t rank_color_header_bytes(const Rank_Color_Info& ci)
{
	if (ci.flag & SINGULAR_FLAG) return 2;
	if (ci.flag & DROPPED_FLAG)  return 1;
	return 22 + 2 + ci.num_ranks * 2;
}

bool shrink_rank_encoded(const std::filesystem::path& path,
                         EGTB_Magic magic, const char* label)
{
	Memory_Mapped_File in;
	if (!in.open_readonly(path.c_str()))
	{
		std::fprintf(stderr, "%s: open failed\n", path.c_str());
		return false;
	}
	const Const_Span<uint8_t> bytes = in.data_span();
	if ((bytes.size() & 63) != 8)
	{
		std::fprintf(stderr, "%s: bad file size\n", path.c_str());
		return false;
	}

	Rank_Color_Info info[COLOR_NB]{};
	uint32_t key_and_table_num = 0;
	Fixed_Vector<Color, 2> table_colors;
	if (!parse_rank_encoded(bytes, magic, info, &key_and_table_num, &table_colors))
	{
		std::fprintf(stderr, "%s: bad %s header/checksum\n", path.c_str(), label);
		return false;
	}

	const Color drop = pick_drop_color(info, table_colors);
	if (drop == COLOR_NB)
	{
		std::printf("%s: skip (nothing droppable)\n", path.c_str());
		return true;
	}

	Rank_Color_Info shrunk[COLOR_NB] = { info[WHITE], info[BLACK] };
	shrunk[drop].flag = DROPPED_FLAG;
	shrunk[drop].entry_bytes = 0;
	shrunk[drop].num_ranks = 0;
	shrunk[drop].rank_table_ptr = nullptr;
	shrunk[drop].offset_tb_ptr = nullptr;
	shrunk[drop].data_ptr = nullptr;
	shrunk[drop].data_size = 0;
	shrunk[drop].block_cnt = 0;

	// Match save_egtb_table layout.
	size_t out_size = 8;
	for (Color c : table_colors)
		out_size += rank_color_header_bytes(shrunk[c]);
	for (Color c : table_colors)
		if (shrunk[c].present && flag_is_normal(shrunk[c].flag))
			out_size += shrunk[c].block_cnt * 8;
	out_size = ceil_to_multiple(out_size, (size_t)64);
	for (Color c : table_colors)
	{
		if (!shrunk[c].present || !flag_is_normal(shrunk[c].flag)) continue;
		out_size += shrunk[c].data_size;
		out_size = ceil_to_multiple(out_size, (size_t)64);
	}

	const std::filesystem::path tmp = path.string() + ".shrink_tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
	{
		std::fprintf(stderr, "%s: create temp failed\n", tmp.c_str());
		return false;
	}
	Serial_Memory_Writer w(out.data_span());

	w.write<uint32_t>(static_cast<uint32_t>(magic));
	w.write<uint32_t>(key_and_table_num);

	for (Color c : table_colors)
	{
		const Rank_Color_Info& ci = shrunk[c];
		if (ci.flag & SINGULAR_FLAG)
		{
			w.write<uint8_t>(ci.flag);
			w.write<uint8_t>(ci.single_val);
		}
		else if (ci.flag & DROPPED_FLAG)
		{
			w.write<uint8_t>(ci.flag);
		}
		else
		{
			w.write<uint8_t>(0);
			w.write<uint8_t>(ci.entry_bytes);
			w.write<uint32_t>(ci.tail_size);
			w.write<uint32_t>(ci.block_size);
			w.write<uint32_t>(ci.block_cnt);
			w.write<uint64_t>(ci.data_size);
			w.write<uint16_t>(ci.num_ranks);
			if (ci.num_ranks)
				w.write(Const_Span<uint8_t>(ci.rank_table_ptr, ci.num_ranks * 2));
		}
	}

	for (Color c : table_colors)
	{
		const Rank_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.offset_tb_ptr, ci.block_cnt * 8));
	}

	w.zero_align(64);

	for (Color c : table_colors)
	{
		const Rank_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.data_ptr, ci.data_size));
		w.zero_align(64);
	}

	if (w.num_bytes_written() != out_size)
	{
		std::fprintf(stderr, "%s: size mismatch %zu != %zu\n",
			path.c_str(), w.num_bytes_written(), out_size);
		out.close();
		std::filesystem::remove(tmp);
		return false;
	}
	w.write_end_checksum(CHECKSUM_INIT);
	out.close();
	in.close();

	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::fprintf(stderr, "%s: rename failed: %s\n", path.c_str(), ec.message().c_str());
		std::filesystem::remove(tmp);
		return false;
	}

	const size_t orig = bytes.size();
	std::printf("%s: %s shrunk %zu -> %zu (-%.1f%%), dropped %s\n",
		path.c_str(), label, orig, out_size + 8,
		100.0 * (1.0 - double(out_size + 8) / double(orig)),
		drop == WHITE ? "WHITE" : "BLACK");
	return true;
}

// WDL shrinker.
struct Wdl_Color_Info
{
	bool present = false;
	uint8_t flag = 0;
	uint8_t single_val = 0;
	// Present when flag_is_normal(flag).
	uint8_t offset_bits = 0;
	uint16_t tail_size = 0;
	uint32_t block_size = 0;
	uint32_t block_cnt = 0;
	uint64_t data_size = 0;
	uint16_t dict_size = 0;
	const uint8_t* dict_ptr = nullptr;
	const uint8_t* offset_tb_ptr = nullptr;
	const uint8_t* data_ptr = nullptr;
	// Non-empty dictionaries are 2-byte aligned.
	bool dict_pad_byte = false;
};

NODISCARD bool parse_wdl(const Const_Span<uint8_t>& bytes,
                         Wdl_Color_Info info[COLOR_NB],
                         uint32_t* key_and_table_num,
                         Fixed_Vector<Color, 2>* out_table_colors)
{
	Serial_Memory_Reader r(bytes);
	if (!r.is_end_checksum_ok(CHECKSUM_INIT)) return false;
	const uint32_t magic = r.read<uint32_t>();
	if (magic != static_cast<uint32_t>(EGTB_Magic::WDL_MAGIC)) return false;
	*key_and_table_num = r.read<uint32_t>();
	const size_t table_num = *key_and_table_num & 3;
	*out_table_colors = egtb_table_colors(table_num);

	for (Color c : *out_table_colors)
	{
		info[c].present = true;
		const uint8_t flag = r.read<uint8_t>();
		info[c].flag = flag;
		if (flag & SINGULAR_FLAG)
		{
			info[c].single_val = r.read<uint8_t>();
		}
		else if (flag & DROPPED_FLAG)
		{
			// Dropped colors have no payload.
		}
		else
		{
			info[c].offset_bits = r.read<uint8_t>();
			info[c].tail_size   = r.read<uint16_t>();
			info[c].block_size  = r.read<uint32_t>();
			info[c].block_cnt   = r.read<uint32_t>();
			info[c].data_size   = r.read<uint64_t>();
		}
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;

		info[c].dict_size = r.read<uint16_t>();
		if (info[c].dict_size != 0)
		{
			info[c].dict_ptr = r.caret();
			r.advance(info[c].dict_size);
			info[c].dict_pad_byte = ((r.num_bytes_read() & 1) != 0);
			if (info[c].dict_pad_byte) r.advance(1);
		}
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		info[c].offset_tb_ptr = r.caret();
		r.advance((2 + info[c].offset_bits) * info[c].block_cnt);
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		r.align(64);
		info[c].data_ptr = r.caret();
		r.advance(info[c].data_size);
	}

	return true;
}

NODISCARD size_t wdl_color_header_bytes(const Wdl_Color_Info& ci)
{
	if (ci.flag & SINGULAR_FLAG) return 2;
	if (ci.flag & DROPPED_FLAG)  return 1;
	return 20;
}

bool shrink_wdl(const std::filesystem::path& path)
{
	Memory_Mapped_File in;
	if (!in.open_readonly(path.c_str()))
	{
		std::fprintf(stderr, "%s: open failed\n", path.c_str());
		return false;
	}
	const Const_Span<uint8_t> bytes = in.data_span();
	if ((bytes.size() & 63) != 8)
	{
		std::fprintf(stderr, "%s: bad file size\n", path.c_str());
		return false;
	}

	Wdl_Color_Info info[COLOR_NB]{};
	uint32_t key_and_table_num = 0;
	Fixed_Vector<Color, 2> table_colors;
	if (!parse_wdl(bytes, info, &key_and_table_num, &table_colors))
	{
		std::fprintf(stderr, "%s: bad WDL header/checksum\n", path.c_str());
		return false;
	}

	const Color drop = pick_drop_color(info, table_colors);
	if (drop == COLOR_NB)
	{
		std::printf("%s: skip (nothing droppable)\n", path.c_str());
		return true;
	}

	Wdl_Color_Info shrunk[COLOR_NB] = { info[WHITE], info[BLACK] };
	shrunk[drop].flag = DROPPED_FLAG;
	shrunk[drop].dict_size = 0;
	shrunk[drop].dict_ptr = nullptr;
	shrunk[drop].dict_pad_byte = false;
	shrunk[drop].offset_tb_ptr = nullptr;
	shrunk[drop].data_ptr = nullptr;
	shrunk[drop].data_size = 0;
	shrunk[drop].block_cnt = 0;

	// Match save_wdl_table layout:
	//   8B file header
	//   per-color: header bytes (20/2/1)
	//   per-color normal: 2B dict_size + dict bytes (+ 1B align if odd)
	//   per-color normal: (2 + offset_bits)*block_cnt offset table bytes
	//   ceil64
	//   per-color normal: data_size bytes, each ceil64 after
	size_t out_size = 8;
	for (Color c : table_colors)
		out_size += wdl_color_header_bytes(shrunk[c]);
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += 2 + ci.dict_size;
		if (ci.dict_size && (out_size & 1)) out_size += 1;
	}
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += (2 + ci.offset_bits) * ci.block_cnt;
	}
	out_size = ceil_to_multiple(out_size, (size_t)64);
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += ci.data_size;
		out_size = ceil_to_multiple(out_size, (size_t)64);
	}

	const std::filesystem::path tmp = path.string() + ".shrink_tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
	{
		std::fprintf(stderr, "%s: create temp failed\n", tmp.c_str());
		return false;
	}
	Serial_Memory_Writer w(out.data_span());

	w.write<uint32_t>(static_cast<uint32_t>(EGTB_Magic::WDL_MAGIC));
	w.write<uint32_t>(key_and_table_num);

	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (ci.flag & SINGULAR_FLAG)
		{
			w.write<uint8_t>(ci.flag);
			w.write<uint8_t>(ci.single_val);
		}
		else if (ci.flag & DROPPED_FLAG)
		{
			w.write<uint8_t>(ci.flag);
		}
		else
		{
			w.write<uint8_t>(0);
			w.write<uint8_t>(ci.offset_bits);
			w.write<uint16_t>(ci.tail_size);
			w.write<uint32_t>(ci.block_size);
			w.write<uint32_t>(ci.block_cnt);
			w.write<uint64_t>(ci.data_size);
		}
	}

	// Empty dictionaries are uint16(0) with no padding. The probe parser only
	// consumes alignment padding after non-empty dictionaries.
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write<uint16_t>(ci.dict_size);
		if (ci.dict_size)
		{
			w.write(Const_Span<uint8_t>(ci.dict_ptr, ci.dict_size));
			w.zero_align(2);
		}
	}

	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.offset_tb_ptr, (2 + ci.offset_bits) * ci.block_cnt));
	}

	w.zero_align(64);

	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.data_ptr, ci.data_size));
		w.zero_align(64);
	}

	if (w.num_bytes_written() != out_size)
	{
		std::fprintf(stderr, "%s: size mismatch %zu != %zu\n",
			path.c_str(), w.num_bytes_written(), out_size);
		out.close();
		std::filesystem::remove(tmp);
		return false;
	}
	w.write_end_checksum(CHECKSUM_INIT);
	out.close();
	in.close();

	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::fprintf(stderr, "%s: rename failed: %s\n", path.c_str(), ec.message().c_str());
		std::filesystem::remove(tmp);
		return false;
	}

	const size_t orig = bytes.size();
	std::printf("%s: WDL shrunk %zu -> %zu (-%.1f%%), dropped %s\n",
		path.c_str(), orig, out_size + 8,
		100.0 * (1.0 - double(out_size + 8) / double(orig)),
		drop == WHITE ? "WHITE" : "BLACK");
	return true;
}

// Dispatch.
NODISCARD uint32_t peek_magic(const std::filesystem::path& path)
{
	Memory_Mapped_File m;
	if (!m.open_readonly(path.c_str())) return 0;
	const Const_Span<uint8_t> b = m.data_span();
	if (b.size() < 4) return 0;
	uint32_t v;
	std::memcpy(&v, b.data(), 4);
	return v;
}

bool shrink_one(const std::filesystem::path& path)
{
	const uint32_t magic = peek_magic(path);
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTC_MAGIC))
		return shrink_rank_encoded(path, EGTB_Magic::DTC_MAGIC, "DTC");
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTM_MAGIC))
		return shrink_rank_encoded(path, EGTB_Magic::DTM_MAGIC, "DTM");
	if (magic == static_cast<uint32_t>(EGTB_Magic::WDL_MAGIC))
		return shrink_wdl(path);
	std::fprintf(stderr, "%s: unknown magic 0x%08x (not DTC/DTM/WDL)\n", path.c_str(), magic);
	return false;
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s FILE [FILE...]\n", argv[0]);
		return 2;
	}
	int failures = 0;
	for (int i = 1; i < argc; ++i)
		if (!shrink_one(argv[i])) ++failures;
	return failures == 0 ? 0 : 1;
}
