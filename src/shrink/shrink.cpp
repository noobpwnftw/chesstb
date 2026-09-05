// Shrink EGTB files for shipping.
//
// Usage:
//   ./shrink [-n|--dry-run] [--out DIR] path/to/file.lzdtz ...
//   ./shrink wdl dtz dtc dtm dtm50       # generated table directories
//   ./shrink path/to/dir/*               # shell glob; non-table files skipped
//   ./shrink --dry-run path/to/dir/*     # report sizes without writing
//   ./shrink --out output wdl/* dtz/*    # shrink into output/, inputs untouched

#include "chess/chess.h"

#include "egtb/egtb_format.h"

#include "util/filesystem.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"
#include "util/span.h"

#include <iomanip>
#include <iostream>
#include <sstream>


namespace {

constexpr uint8_t SINGULAR_FLAG  = 0x80;
constexpr uint8_t DROPPED_FLAG   = 0x40;
constexpr uint8_t LOSS_ONLY_FLAG = 0x20;
constexpr uint8_t RELAXED_FLAG = 0x10;

constexpr uint8_t KNOWN_FLAGS =
	SINGULAR_FLAG | DROPPED_FLAG | LOSS_ONLY_FLAG | RELAXED_FLAG;
constexpr uint64_t CHECKSUM_INIT = 0xf0f0f0f0f0f0;

bool g_dry_run = false;

// Destination directory for --out. Empty means rewrite in place.
std::filesystem::path g_output_dir;

// Running totals across all processed files (skipped files count unchanged).
uint64_t g_total_orig = 0;
uint64_t g_total_new  = 0;

void account(size_t orig, size_t new_size)
{
	g_total_orig += orig;
	g_total_new  += new_size;
}

NODISCARD std::filesystem::path output_path(const std::filesystem::path& path)
{
	return g_output_dir.empty() ? path : g_output_dir / path.filename();
}

NODISCARD bool keep_unshrinkable(const std::filesystem::path& path, const char* reason)
{
	const std::filesystem::path dst = output_path(path);
	std::error_code ec;
	if (g_dry_run || dst == path || std::filesystem::equivalent(path, dst, ec))
	{
		std::cout << path.string() << ": skip (" << reason << ")\n";
		return true;
	}

	ec.clear();
	std::filesystem::copy_file(
		path, dst, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec)
	{
		std::cerr << path.string() << ": copy to " << dst.string()
			<< " failed: " << ec.message() << "\n";
		return false;
	}
	std::cout << path.string() << ": copied unchanged (" << reason << ")\n";
	return true;
}

NODISCARD std::string pct_saved(uint64_t orig, uint64_t new_size)
{
	std::ostringstream os;
	os << std::fixed << std::setprecision(1)
		<< (orig ? 100.0 * (1.0 - double(new_size) / double(orig)) : 0.0);
	return os.str();
}

bool flag_is_normal(uint8_t flag)
{
	return (flag & (SINGULAR_FLAG | DROPPED_FLAG)) == 0;
}

// Select the color table to drop. Returns COLOR_NB when nothing is droppable:
//   - symmetric materials (only one color in table_colors)
//   - both colors already non-normal (singular/dropped)
//   - the only normal color's sibling is already dropped
//   - loss-only frames
template <typename ColorInfo>
NODISCARD Color pick_drop_color(const ColorInfo info[COLOR_NB],
                                const Fixed_Vector<Color, 2>& table_colors)
{
	for (Color c : table_colors)
		if (info[c].present && (info[c].flag & LOSS_ONLY_FLAG))
			return COLOR_NB;

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

// Rank-encoded .lzdtz/.lzdtm files share this layout.
struct Rank_Color_Info
{
	bool present = false;
	uint8_t flag = 0;
	uint8_t single_val = 0;
	// Present when flag_is_normal(flag).
	uint32_t index_perm = 0;
	uint8_t entry_bytes = 0;
	uint32_t tail_size = 0;
	uint32_t block_size = 0;
	uint64_t block_cnt = 0;
	uint64_t data_size = 0;
	uint16_t num_ranks = 0;
	const uint8_t* rank_table_ptr = nullptr;
	const uint8_t* offset_tb_ptr  = nullptr;
	size_t off_section_bytes = 0;
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
		if (flag & ~KNOWN_FLAGS) return false;
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
			info[c].index_perm = r.read<uint32_t>();
			info[c].entry_bytes = r.read<uint8_t>();
			info[c].tail_size   = r.read<uint32_t>();
			info[c].block_size  = r.read<uint32_t>();
			info[c].block_cnt   = r.read<uint64_t>();
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
		const uint8_t* sec_start = r.caret();
		const uint8_t log2_bu      = r.read<uint8_t>();
		const uint8_t sample_width = r.read<uint8_t>();
		const uint8_t offset_width = r.read<uint8_t>();
		r.advance(1);  // usz_width
		r.advance(Mono_Uint_Vec::on_disk_bytes(
			info[c].block_cnt + 1, log2_bu, sample_width, offset_width));
		info[c].offset_tb_ptr = sec_start;
		info[c].off_section_bytes = static_cast<size_t>(r.caret() - sec_start);
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
	// flag(1)+index_perm(4)+entry_bytes(1)+tail(4)+block_size(4)
	// + block_cnt(8)+data_size(8)+num_ranks(2) = 32
	return 32 + ci.num_ranks * 2;
}

bool shrink_rank_encoded(const std::filesystem::path& path,
                         EGTB_Magic magic, const char* label)
{
	Memory_Mapped_File in;
	if (!in.open_readonly(path.c_str()))
	{
		std::cerr << path.string() << ": open failed\n";
		return false;
	}
	const Const_Span<uint8_t> bytes = in.data_span();
	if ((bytes.size() & 63) != 8)
	{
		std::cerr << path.string() << ": bad file size\n";
		return false;
	}

	Rank_Color_Info info[COLOR_NB]{};
	uint32_t key_and_table_num = 0;
	Fixed_Vector<Color, 2> table_colors;
	if (!parse_rank_encoded(bytes, magic, info, &key_and_table_num, &table_colors))
	{
		std::cerr << path.string() << ": bad " << label << " header/checksum\n";
		return false;
	}

	const Color drop = pick_drop_color(info, table_colors);
	if (drop == COLOR_NB)
	{
		account(bytes.size(), bytes.size());
		return keep_unshrinkable(path, "nothing droppable");
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
			out_size += shrunk[c].off_section_bytes;
	out_size = ceil_to_multiple(out_size, (size_t)64);
	for (Color c : table_colors)
	{
		if (!shrunk[c].present || !flag_is_normal(shrunk[c].flag)) continue;
		out_size += shrunk[c].data_size;
		out_size = ceil_to_multiple(out_size, (size_t)64);
	}

	if (g_dry_run)
	{
		const size_t orig = bytes.size();
		std::cout << path.string() << ": " << label << " would shrink "
			<< orig << " -> " << out_size + 8
			<< " (-" << pct_saved(orig, out_size + 8) << "%), drop "
			<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
		account(orig, out_size + 8);
		return true;
	}

	const std::filesystem::path dst = output_path(path);
	const std::filesystem::path tmp = dst.string() + ".shrink_tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
	{
		std::cerr << tmp.string() << ": create temp failed\n";
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
			w.write<uint8_t>(ci.flag);
			w.write<uint32_t>(ci.index_perm);
			w.write<uint8_t>(ci.entry_bytes);
			w.write<uint32_t>(ci.tail_size);
			w.write<uint32_t>(ci.block_size);
			w.write<uint64_t>(ci.block_cnt);
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
		w.write(Const_Span<uint8_t>(ci.offset_tb_ptr, ci.off_section_bytes));
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
		std::cerr << path.string() << ": size mismatch "
			<< w.num_bytes_written() << " != " << out_size << "\n";
		out.close_file();
		std::filesystem::remove(tmp);
		return false;
	}
	w.write_end_checksum(CHECKSUM_INIT);
	out.close_file();
	in.close_file();

	std::error_code ec;
	std::filesystem::rename(tmp, dst, ec);
	if (ec)
	{
		std::cerr << path.string() << ": rename failed: " << ec.message() << "\n";
		std::filesystem::remove(tmp);
		return false;
	}

	const size_t orig = bytes.size();
	std::cout << path.string() << ": " << label << " shrunk "
		<< orig << " -> " << out_size + 8
		<< " (-" << pct_saved(orig, out_size + 8) << "%), dropped "
		<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
	account(orig, out_size + 8);
	return true;
}

struct Layered_Color_Info
{
	bool present = false;
	uint8_t flag = 0;
	uint8_t single_val = 0;
	// Fields below are populated only when flag_is_normal(flag).
	uint32_t index_perm = 0;
	uint8_t entry_bytes = 0;
	uint32_t block_positions = 0;
	uint64_t block_cnt = 0;
	uint32_t tail_positions = 0;
	uint64_t data_size = 0;
	uint16_t num_ranks = 0;
	const uint8_t* rank_table_ptr = nullptr;
	const uint8_t* offset_tb_ptr  = nullptr;
	size_t off_section_bytes = 0;
	const uint8_t* data_ptr       = nullptr;
};

NODISCARD bool parse_layered(const Const_Span<uint8_t>& bytes, EGTB_Magic expected_magic,
                             Layered_Color_Info info[COLOR_NB],
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
		if (flag & ~KNOWN_FLAGS) return false;
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
			info[c].index_perm = r.read<uint32_t>();
			info[c].entry_bytes     = r.read<uint8_t>();
			info[c].block_positions = r.read<uint32_t>();
			info[c].block_cnt       = r.read<uint64_t>();
			info[c].tail_positions  = r.read<uint32_t>();
			info[c].data_size       = r.read<uint64_t>();
			info[c].num_ranks       = r.read<uint16_t>();
			info[c].rank_table_ptr  = r.caret();
			r.advance(info[c].num_ranks * 2);
		}
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		const uint8_t* sec_start = r.caret();
		const uint8_t log2_bu      = r.read<uint8_t>();
		const uint8_t sample_width = r.read<uint8_t>();
		const uint8_t offset_width = r.read<uint8_t>();
		const uint8_t usz_width    = r.read<uint8_t>();
		r.advance(Mono_Uint_Vec::on_disk_bytes(
			info[c].block_cnt + 1, log2_bu, sample_width, offset_width));
		r.advance(Min0_Uint_Vec::on_disk_bytes(info[c].block_cnt, usz_width));
		info[c].offset_tb_ptr = sec_start;
		info[c].off_section_bytes = static_cast<size_t>(r.caret() - sec_start);
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

NODISCARD size_t layered_color_header_bytes(const Layered_Color_Info& ci)
{
	if (ci.flag & SINGULAR_FLAG) return 2;
	if (ci.flag & DROPPED_FLAG)  return 1;
	// 1 flag + 4 index_perm + 1 eb + 4 block_positions + 8 block_cnt
	// + 4 tail_positions + 8 data_size + 2 num_ranks + ranks.
	return 32 + ci.num_ranks * 2;
}

bool shrink_layered(const std::filesystem::path& path, EGTB_Magic magic, const char* label)
{
	Memory_Mapped_File in;
	if (!in.open_readonly(path.c_str()))
	{
		std::cerr << path.string() << ": open failed\n";
		return false;
	}
	const Const_Span<uint8_t> bytes = in.data_span();
	if ((bytes.size() & 63) != 8)
	{
		std::cerr << path.string() << ": bad file size\n";
		return false;
	}

	Layered_Color_Info info[COLOR_NB]{};
	uint32_t key_and_table_num = 0;
	Fixed_Vector<Color, 2> table_colors;
	if (!parse_layered(bytes, magic, info, &key_and_table_num, &table_colors))
	{
		std::cerr << path.string() << ": bad " << label << " header/checksum\n";
		return false;
	}

	const Color drop = pick_drop_color(info, table_colors);
	if (drop == COLOR_NB)
	{
		account(bytes.size(), bytes.size());
		return keep_unshrinkable(path, "nothing droppable");
	}

	Layered_Color_Info shrunk[COLOR_NB] = { info[WHITE], info[BLACK] };
	shrunk[drop].flag = DROPPED_FLAG;
	shrunk[drop].entry_bytes = 0;
	shrunk[drop].num_ranks = 0;
	shrunk[drop].rank_table_ptr = nullptr;
	shrunk[drop].offset_tb_ptr = nullptr;
	shrunk[drop].data_ptr = nullptr;
	shrunk[drop].data_size = 0;
	shrunk[drop].block_cnt = 0;

	// Match save_layered_table layout.
	size_t out_size = 8;
	for (Color c : table_colors)
		out_size += layered_color_header_bytes(shrunk[c]);
	for (Color c : table_colors)
		if (shrunk[c].present && flag_is_normal(shrunk[c].flag))
			out_size += shrunk[c].off_section_bytes;
	out_size = ceil_to_multiple(out_size, (size_t)64);
	for (Color c : table_colors)
	{
		if (!shrunk[c].present || !flag_is_normal(shrunk[c].flag)) continue;
		out_size += shrunk[c].data_size;
		out_size = ceil_to_multiple(out_size, (size_t)64);
	}

	if (g_dry_run)
	{
		const size_t orig = bytes.size();
		std::cout << path.string() << ": " << label << " would shrink "
			<< orig << " -> " << out_size + 8
			<< " (-" << pct_saved(orig, out_size + 8) << "%), drop "
			<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
		account(orig, out_size + 8);
		return true;
	}

	const std::filesystem::path dst = output_path(path);
	const std::filesystem::path tmp = dst.string() + ".shrink_tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
	{
		std::cerr << tmp.string() << ": create temp failed\n";
		return false;
	}
	Serial_Memory_Writer w(out.data_span());

	w.write<uint32_t>(static_cast<uint32_t>(magic));
	w.write<uint32_t>(key_and_table_num);

	for (Color c : table_colors)
	{
		const Layered_Color_Info& ci = shrunk[c];
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
			w.write<uint8_t>(ci.flag);
			w.write<uint32_t>(ci.index_perm);
			w.write<uint8_t>(ci.entry_bytes);
			w.write<uint32_t>(ci.block_positions);
			w.write<uint64_t>(ci.block_cnt);
			w.write<uint32_t>(ci.tail_positions);
			w.write<uint64_t>(ci.data_size);
			w.write<uint16_t>(ci.num_ranks);
			if (ci.num_ranks)
				w.write(Const_Span<uint8_t>(ci.rank_table_ptr, ci.num_ranks * 2));
		}
	}

	for (Color c : table_colors)
	{
		const Layered_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.offset_tb_ptr, ci.off_section_bytes));
	}

	w.zero_align(64);

	for (Color c : table_colors)
	{
		const Layered_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.data_ptr, ci.data_size));
		w.zero_align(64);
	}

	if (w.num_bytes_written() != out_size)
	{
		std::cerr << path.string() << ": size mismatch "
			<< w.num_bytes_written() << " != " << out_size << "\n";
		out.close_file();
		std::filesystem::remove(tmp);
		return false;
	}
	w.write_end_checksum(CHECKSUM_INIT);
	out.close_file();
	in.close_file();

	std::error_code ec;
	std::filesystem::rename(tmp, dst, ec);
	if (ec)
	{
		std::cerr << path.string() << ": rename failed: " << ec.message() << "\n";
		std::filesystem::remove(tmp);
		return false;
	}

	const size_t orig = bytes.size();
	std::cout << path.string() << ": " << label << " shrunk "
		<< orig << " -> " << out_size + 8
		<< " (-" << pct_saved(orig, out_size + 8) << "%), dropped "
		<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
	account(orig, out_size + 8);
	return true;
}

struct Wdl_Color_Info
{
	bool present = false;
	uint8_t flag = 0;
	uint8_t single_val = 0;
	// Present when flag_is_normal(flag).
	uint32_t index_perm = 0;
	uint16_t tail_size = 0;
	uint32_t block_size = 0;
	uint64_t block_cnt = 0;
	uint64_t data_size = 0;
	uint16_t dict_size = 0;
	const uint8_t* dict_ptr = nullptr;
	const uint8_t* offset_tb_ptr = nullptr;
	size_t off_section_bytes = 0;
	const uint8_t* data_ptr = nullptr;
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
		if (flag & ~KNOWN_FLAGS) return false;
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
			info[c].index_perm = r.read<uint32_t>();
			info[c].tail_size   = r.read<uint16_t>();
			info[c].block_size  = r.read<uint32_t>();
			info[c].block_cnt   = r.read<uint64_t>();
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
		}
	}

	for (Color c : *out_table_colors)
	{
		if (!info[c].present) continue;
		if (!flag_is_normal(info[c].flag)) continue;
		const uint8_t* sec_start = r.caret();
		const uint8_t log2_bu      = r.read<uint8_t>();
		const uint8_t sample_width = r.read<uint8_t>();
		const uint8_t offset_width = r.read<uint8_t>();
		r.advance(1);  // usz_width
		r.advance(Mono_Uint_Vec::on_disk_bytes(
			info[c].block_cnt + 1, log2_bu, sample_width, offset_width));
		info[c].offset_tb_ptr = sec_start;
		info[c].off_section_bytes = static_cast<size_t>(r.caret() - sec_start);
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
	// flag(1)+index_perm(4)+tail(2)+block_size(4)+block_cnt(8)+data_size(8) = 27
	return 27;
}

bool shrink_wdl(const std::filesystem::path& path)
{
	Memory_Mapped_File in;
	if (!in.open_readonly(path.c_str()))
	{
		std::cerr << path.string() << ": open failed\n";
		return false;
	}
	const Const_Span<uint8_t> bytes = in.data_span();
	if ((bytes.size() & 63) != 8)
	{
		std::cerr << path.string() << ": bad file size\n";
		return false;
	}

	Wdl_Color_Info info[COLOR_NB]{};
	uint32_t key_and_table_num = 0;
	Fixed_Vector<Color, 2> table_colors;
	if (!parse_wdl(bytes, info, &key_and_table_num, &table_colors))
	{
		std::cerr << path.string() << ": bad WDL header/checksum\n";
		return false;
	}

	const Color drop = pick_drop_color(info, table_colors);
	if (drop == COLOR_NB)
	{
		account(bytes.size(), bytes.size());
		return keep_unshrinkable(path, "nothing droppable");
	}

	Wdl_Color_Info shrunk[COLOR_NB] = { info[WHITE], info[BLACK] };
	shrunk[drop].flag = DROPPED_FLAG;
	shrunk[drop].dict_size = 0;
	shrunk[drop].dict_ptr = nullptr;
	shrunk[drop].offset_tb_ptr = nullptr;
	shrunk[drop].data_ptr = nullptr;
	shrunk[drop].data_size = 0;
	shrunk[drop].block_cnt = 0;

	// Match save_wdl_table layout.
	size_t out_size = 8;
	for (Color c : table_colors)
		out_size += wdl_color_header_bytes(shrunk[c]);
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += 2 + ci.dict_size;
	}
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += ci.off_section_bytes;
	}
	out_size = ceil_to_multiple(out_size, (size_t)64);
	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		out_size += ci.data_size;
		out_size = ceil_to_multiple(out_size, (size_t)64);
	}

	if (g_dry_run)
	{
		const size_t orig = bytes.size();
		std::cout << path.string() << ": WDL would shrink "
			<< orig << " -> " << out_size + 8
			<< " (-" << pct_saved(orig, out_size + 8) << "%), drop "
			<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
		account(orig, out_size + 8);
		return true;
	}

	const std::filesystem::path dst = output_path(path);
	const std::filesystem::path tmp = dst.string() + ".shrink_tmp";
	Memory_Mapped_File out;
	if (!out.create(tmp.c_str(), out_size + 8))
	{
		std::cerr << tmp.string() << ": create temp failed\n";
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
			w.write<uint8_t>(ci.flag);
			w.write<uint32_t>(ci.index_perm);
			w.write<uint16_t>(ci.tail_size);
			w.write<uint32_t>(ci.block_size);
			w.write<uint64_t>(ci.block_cnt);
			w.write<uint64_t>(ci.data_size);
		}
	}

	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write<uint16_t>(ci.dict_size);
		if (ci.dict_size)
			w.write(Const_Span<uint8_t>(ci.dict_ptr, ci.dict_size));
	}

	for (Color c : table_colors)
	{
		const Wdl_Color_Info& ci = shrunk[c];
		if (!ci.present || !flag_is_normal(ci.flag)) continue;
		w.write(Const_Span<uint8_t>(ci.offset_tb_ptr, ci.off_section_bytes));
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
		std::cerr << path.string() << ": size mismatch "
			<< w.num_bytes_written() << " != " << out_size << "\n";
		out.close_file();
		std::filesystem::remove(tmp);
		return false;
	}
	w.write_end_checksum(CHECKSUM_INIT);
	out.close_file();
	in.close_file();

	std::error_code ec;
	std::filesystem::rename(tmp, dst, ec);
	if (ec)
	{
		std::cerr << path.string() << ": rename failed: " << ec.message() << "\n";
		std::filesystem::remove(tmp);
		return false;
	}

	const size_t orig = bytes.size();
	std::cout << path.string() << ": WDL shrunk "
		<< orig << " -> " << out_size + 8
		<< " (-" << pct_saved(orig, out_size + 8) << "%), dropped "
		<< (drop == WHITE ? "WHITE" : "BLACK") << "\n";
	account(orig, out_size + 8);
	return true;
}

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
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTZ_MAGIC))
		return shrink_rank_encoded(path, EGTB_Magic::DTZ_MAGIC, "DTZ");
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTC_MAGIC))
		return shrink_layered(path, EGTB_Magic::DTC_MAGIC, "DTC");
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTM_MAGIC))
		return shrink_rank_encoded(path, EGTB_Magic::DTM_MAGIC, "DTM");
	if (magic == static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		return shrink_layered(path, EGTB_Magic::DTM50_MAGIC, "DTM50");
	if (magic == static_cast<uint32_t>(EGTB_Magic::WDL_MAGIC))
		return shrink_wdl(path);
	std::cout << path.string() << ": skip (not WDL/DTZ/DTC/DTM/DTM50)\n";
	return true;
}

bool shrink_path(const std::filesystem::path& path)
{
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec))
	{
		std::vector<std::filesystem::path> files;
		for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->is_regular_file(ec))
				files.push_back(it->path());
		}
		if (ec)
		{
			std::cerr << path.string() << ": directory scan failed: " << ec.message() << "\n";
			return false;
		}
		std::sort(files.begin(), files.end());
		bool ok = true;
		for (const auto& file : files)
			ok = shrink_one(file) && ok;
		return ok;
	}

	if (!std::filesystem::is_regular_file(path, ec))
	{
		std::cerr << path.string() << ": not a regular file or directory\n";
		return false;
	}

	return shrink_one(path);
}

}  // namespace

int main(int argc, char** argv)
{
	try {
		std::vector<const char*> inputs;
		bool options_done = false;
		for (int i = 1; i < argc; ++i)
		{
			const char* a = argv[i];
			if (options_done)
				inputs.push_back(a);
			else if (std::strcmp(a, "-n") == 0 || std::strcmp(a, "--dry-run") == 0)
				g_dry_run = true;
			else if (std::strcmp(a, "--out") == 0)
			{
				if (i + 1 >= argc)
				{
					std::cerr << "--out: missing directory argument\n";
					return 2;
				}
				g_output_dir = argv[++i];
			}
			else if (std::strncmp(a, "--out=", 6) == 0)
				g_output_dir = a + 6;
			else if (std::strcmp(a, "--") == 0)
				options_done = true;
			else if (a[0] == '-' && a[1] != '\0')
			{
				std::cerr << a << ": unknown option\n";
				return 2;
			}
			else
				inputs.push_back(a);
		}

		if (inputs.empty())
		{
			std::cerr << "usage: " << argv[0]
				<< " [-n|--dry-run] [--out DIR] FILE_OR_DIR [FILE_OR_DIR...]\n";
			return 2;
		}

		if (!g_output_dir.empty() && !g_dry_run)
		{
			std::error_code ec;
			std::filesystem::create_directories(g_output_dir, ec);
			if (ec)
			{
				std::cerr << g_output_dir.string() << ": create failed: "
					<< ec.message() << "\n";
				return 2;
			}
			if (!std::filesystem::is_directory(g_output_dir))
			{
				std::cerr << g_output_dir.string() << ": not a directory\n";
				return 2;
			}
		}

		int failures = 0;
		for (const char* input : inputs)
			if (!shrink_path(input)) ++failures;

		std::cout << "total: " << (g_dry_run ? "would shrink" : "shrunk")
			<< " " << g_total_orig << " -> " << g_total_new
			<< " (-" << pct_saved(g_total_orig, g_total_new) << "%)\n";
		return failures == 0 ? 0 : 1;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
