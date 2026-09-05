#include "transcribe/source_tables.h"

#include "util/math.h"

#include <stdexcept>
#include <string>

namespace {

// Everything before the per-color headers, identical across the three formats.
NODISCARD Fixed_Vector<Color, 2> read_prologue(
	Serial_Memory_Reader& r,
	const Piece_Config& ps,
	const std::filesystem::path& path,
	EGTB_Magic magic,
	const char* kind)
{
	if (!r.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error(std::string("Invalid ") + kind + " checksum " + path.string());

	if (r.read<uint32_t>() != narrowing_static_cast<uint32_t>(magic))
		throw std::runtime_error(std::string("Invalid ") + kind + " magic " + path.string());

	const uint32_t key_and_table_num = r.read<uint32_t>();
	if ((key_and_table_num >> 2u) != ps.min_material_key().value())
		throw std::runtime_error(std::string("Wrong material key in ") + kind + " " + path.string());

	return egtb_table_colors(key_and_table_num & 3);
}

void open_source_file(Memory_Mapped_File& file, const std::filesystem::path& path,
                      const char* kind)
{
	if (!file.open_readonly(path.c_str()))
		throw std::runtime_error(std::string("Could not open ") + kind + " " + path.string());
	if ((file.data_span().size() & 63) != 8)
		throw std::runtime_error(std::string("Invalid ") + kind + " file size " + path.string());
}

NODISCARD uint8_t read_frame_flag(Serial_Memory_Reader& r, const std::filesystem::path& path,
                                  const char* kind, bool allow_loss_only, bool allow_relaxed)
{
	const uint8_t flag = r.read<uint8_t>();
	if ((flag & EGTB_DROPPED_FLAG) && allow_loss_only)
		throw std::runtime_error(std::string(kind) + " has a dropped frame; a loss-only one needs "
			"both frames " + path.string());
	if ((flag & EGTB_LOSS_ONLY_FLAG) && !allow_loss_only)
		throw std::runtime_error(std::string(kind) + " is loss-only; only --loss-only takes it as a source "
			+ path.string());
	// A relaxed run omits the same cells again, so it never reads one that is
	// already gone. Any other run would take the fill for a value.
	if ((flag & EGTB_RELAXED_FLAG) && !allow_relaxed)
		throw std::runtime_error(std::string(kind) + " is relaxed; only a relaxed or loss-only"
			" DTZ run takes such a source " + path.string());
	return flag;
}

NODISCARD uint32_t read_perm(Serial_Memory_Reader& r, const Piece_Config_For_Gen& epsi,
                             const std::filesystem::path& path, const char* kind)
{
	const uint32_t perm = r.read<uint32_t>();
	if (!index_permutation_config_is_valid(epsi, perm))
		throw std::runtime_error(std::string("Invalid ") + kind + " index permutation " + path.string());
	return perm;
}

// The per-color offset section, shared by all three formats (usz_width is
// DTM50's alone; the other two write a zero there).
void read_offsets(Serial_Memory_Reader& r, Mono_Uint_Vec& out, size_t block_cnt)
{
	const uint8_t log2_bu      = r.read<uint8_t>();
	const uint8_t sample_width = r.read<uint8_t>();
	const uint8_t offset_width = r.read<uint8_t>();
	r.advance(1);  // usz_width
	const uint8_t* ptr = r.caret();
	r.advance(Mono_Uint_Vec::on_disk_bytes(block_cnt + 1, log2_bu, sample_width, offset_width));
	out = Mono_Uint_Vec(ptr, block_cnt + 1, log2_bu, sample_width, offset_width);
}

void read_rank_header(Serial_Memory_Reader& r, Source_Rank_Per_Color& pc,
                      const Piece_Config_For_Gen& epsi,
                      const std::filesystem::path& path, const char* kind)
{
	pc.plan = make_index_permutation_plan(epsi, read_perm(r, epsi, path, kind));

	pc.entry_bytes = r.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error(std::string("Bad ") + kind + " entry_bytes " + path.string());
	pc.tail_size  = r.read<uint32_t>();
	pc.block_size = r.read<uint32_t>();
	pc.block_cnt  = r.read<uint64_t>();
	pc.data_size  = r.read<uint64_t>();

	const size_t num_ranks = r.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t i = 0; i < num_ranks; ++i)
		pc.rank_to_value[i] = r.read<uint16_t>();
}

void load_rank_source(
	Source_Rank_Per_Color (&per_color)[COLOR_NB],
	Fixed_Vector<Color, 2>& colors,
	Memory_Mapped_File& file,
	const Piece_Config& ps,
	const std::filesystem::path& path,
	EGTB_Magic magic,
	const char* kind,
	bool allow_loss_only,
	bool allow_relaxed)
{
	open_source_file(file, path, kind);
	Serial_Memory_Reader r(file.data_span());
	colors = read_prologue(r, ps, path, magic, kind);

	const Piece_Config_For_Gen epsi(ps);

	for (const Color c : colors)
	{
		Source_Rank_Per_Color& pc = per_color[c];
		const uint8_t flag = read_frame_flag(r, path, kind, allow_loss_only, allow_relaxed);
		if (flag & EGTB_DROPPED_FLAG)
		{
			pc.is_dropped = true;
			continue;
		}
		if (flag & EGTB_SINGULAR_FLAG)
		{
			pc.is_singular = true;
			pc.entry_bytes = 1;
			pc.single_val = r.read<uint8_t>();
			continue;
		}
		read_rank_header(r, pc, epsi, path, kind);
	}
	for (const Color c : colors)
	{
		if (per_color[c].is_singular || per_color[c].is_dropped) continue;
		read_offsets(r, per_color[c].offsets, per_color[c].block_cnt);
	}
	for (const Color c : colors)
	{
		if (per_color[c].is_singular || per_color[c].is_dropped) continue;
		r.align(64);
		per_color[c].compressed_data = r.caret();
		r.advance(per_color[c].data_size);
	}

	const size_t num_positions = epsi.num_positions();
	for (const Color c : colors)
	{
		const Source_Rank_Per_Color& pc = per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		if (pc.block_size * num_full + pc.tail_size != num_positions * pc.entry_bytes)
			throw std::runtime_error(std::string(kind) + " decompressed size mismatch " + path.string());
	}
}

template <typename Tag>
NODISCARD uint16_t rank_cell(const Source_Rank_Per_Color& pc, Block_Cache& cache,
                             Board_Index pos)
{
	if (pc.is_singular) return pc.single_val;

	const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
	const size_t per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = storage_pos / per_block;
	const size_t in_block = storage_pos % per_block;

	// Equal offsets: an all-don't-care block, dropped rather than compressed.
	const auto pair = pc.offsets.get2(block_id);
	if (pair[0] == pair[1]) return 0;

	const uint8_t* decoded = fetch_block_cached<Tag>(cache, block_id, [&pc](
		Block_Cache&, size_t bid) -> Block_Ptr
	{
		const size_t decode_sz =
			(bid == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
		const auto pr = pc.offsets.get2(bid);
		auto buf = std::make_shared<std::vector<uint8_t>>(decode_sz, 0);
		lzma_decompress_into(
			Span<uint8_t>(buf->data(), decode_sz),
			Const_Span<uint8_t>(pc.compressed_data + pr[0], pr[1] - pr[0]));
		return buf;
	});

	if (pc.entry_bytes == 1) return pc.rank_to_value[decoded[in_block]];
	uint16_t rank;
	std::memcpy(&rank, decoded + in_block * sizeof(uint16_t), sizeof(rank));
	return pc.rank_to_value[rank];
}

}  // namespace

// One decoded rs-pack block: a directory of per-position records over the
// payload. Records are variable-width and walkable only in order, so the walk
// runs once per block and expansion is then random access. Blob layout: np,
// payload offset, state[np], entry_off[np], payload.
namespace {

constexpr size_t RS_DIR_HEADER = 8;

NODISCARD Block_Ptr decode_rs_block(const Source_Layered_Per_Color& pc, size_t block_id)
{
	const size_t positions =
		(block_id == pc.block_cnt - 1 && pc.tail_positions != 0)
		? pc.tail_positions : pc.block_positions;
	const size_t usz = pc.usizes.get(block_id);
	const auto pair = pc.offsets.get2(block_id);

	auto blob = std::make_shared<std::vector<uint8_t>>(
		RS_DIR_HEADER + positions * 5 + usz, 0);
	uint8_t* const base = blob->data();
	const uint32_t np32 = narrowing_static_cast<uint32_t>(positions);
	const uint32_t payload_off = narrowing_static_cast<uint32_t>(RS_DIR_HEADER + positions * 5);
	std::memcpy(base, &np32, 4);
	std::memcpy(base + 4, &payload_off, 4);
	uint8_t* const state = base + RS_DIR_HEADER;
	auto* const entry_off = reinterpret_cast<uint32_t*>(base + RS_DIR_HEADER + positions);
	uint8_t* const raw = base + payload_off;

	lzma_decompress_into(Span<uint8_t>(raw, usz),
		Const_Span<uint8_t>(pc.compressed_data + pair[0], pair[1] - pair[0]));

	uint32_t hdr[6];
	std::memcpy(hdr, raw, 24);
	const uint32_t num_single = hdr[1], num_double = hdr[2], num_multi = hdr[3];
	const uint32_t ss_bytes = hdr[4], ds_bytes = hdr[5];
	ASSERT(hdr[0] == np32);

	const size_t eb = pc.entry_bytes;
	const size_t num_const = positions - num_single - num_double - num_multi;
	const uint8_t* p = raw + 24;
	const uint8_t* const state_bits = p;      p += (positions * 2 + 7) / 8;
	const uint8_t* const const_stream = p;    p += num_const * eb;
	const uint8_t* const single_stream = p;   p += ss_bytes;
	const uint8_t* const double_stream = p;   p += ds_bytes;
	p += (4 - static_cast<size_t>(p - raw) % 4) % 4;
	const uint8_t* const multi_dir = p;       p += (num_multi + 1) * 4u;
	const uint8_t* const multi_data = p;

	// Widths follow the draw-end bit in the record's own h byte.
	size_t const_idx = 0, single_off = 0, double_off = 0, multi_idx = 0;
	for (size_t i = 0; i < positions; ++i)
	{
		const size_t bit = i * 2;
		const uint8_t st = (state_bits[bit / 8] >> (bit % 8)) & 3u;
		state[i] = st;
		switch (st)
		{
			case 0:
				entry_off[i] = narrowing_static_cast<uint32_t>(const_stream - raw + const_idx * eb);
				++const_idx;
				break;
			case 1:
			{
				const uint8_t* e = single_stream + single_off;
				entry_off[i] = narrowing_static_cast<uint32_t>(e - raw);
				single_off += (e[0] & 0x80u) ? (1 + eb) : (1 + 2 * eb);
				break;
			}
			case 2:
			{
				const uint8_t* e = double_stream + double_off;
				entry_off[i] = narrowing_static_cast<uint32_t>(e - raw);
				double_off += (e[1] & 0x80u) ? (2 + 2 * eb) : (2 + 3 * eb);
				break;
			}
			default:
			{
				uint32_t off;
				std::memcpy(&off, multi_dir + multi_idx * 4u, 4);
				entry_off[i] = narrowing_static_cast<uint32_t>(multi_data - raw + off);
				++multi_idx;
				break;
			}
		}
	}
	return blob;
}

NODISCARD INLINE uint16_t rs_rank(const uint8_t* p, size_t eb)
{
	if (eb == 1) return *p;
	uint16_t r;
	std::memcpy(&r, p, 2);
	return r;
}

}  // namespace

std::array<uint32_t, COLOR_NB> read_table_permutations(
	const Piece_Config& ps, const std::filesystem::path& path, EGTB_Magic magic)
{
	Memory_Mapped_File file;
	open_source_file(file, path, "table");
	Serial_Memory_Reader r(file.data_span());
	const Fixed_Vector<Color, 2> colors = read_prologue(r, ps, path, magic, "table");

	std::array<uint32_t, COLOR_NB> out{};
	for (const Color c : colors)
	{
		const uint8_t flag = r.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG) { r.advance(1); continue; }
		if (flag & EGTB_DROPPED_FLAG) continue;
		out[c] = r.read<uint32_t>();
		r.advance(1 + 4 + 4 + 8 + 8);  // entry_bytes, tail, block_size, block_cnt, data_size
		r.advance(r.read<uint16_t>() * 2u);  // rank table
	}
	return out;
}

void load_source_wdl(Out_Param<Source_WDL> wdl, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only,
                     bool allow_relaxed)
{
	constexpr const char* KIND = "WDL";

	open_source_file(wdl->m_file, path, KIND);
	Serial_Memory_Reader r(wdl->m_file.data_span());
	wdl->m_colors = read_prologue(r, ps, path, EGTB_Magic::WDL_MAGIC, KIND);

	const Piece_Config_For_Gen epsi(ps);

	for (const Color c : wdl->m_colors)
	{
		Source_WDL_Per_Color& pc = wdl->m_per_color[c];
		const uint8_t flag = read_frame_flag(r, path, KIND, allow_loss_only, allow_relaxed);
		if (flag & EGTB_DROPPED_FLAG)
		{
			pc.is_dropped = true;
			continue;
		}
		if (flag & EGTB_SINGULAR_FLAG)
		{
			pc.is_singular = true;
			pc.single_val = static_cast<WDL_Stored>(r.read<uint8_t>());
			continue;
		}
		pc.plan = make_index_permutation_plan(epsi, read_perm(r, epsi, path, KIND));
		pc.tail_size  = r.read<uint16_t>();
		pc.block_size = r.read<uint32_t>();
		pc.block_cnt  = r.read<uint64_t>();
		pc.data_size  = r.read<uint64_t>();
	}

	for (const Color c : wdl->m_colors)
	{
		Source_WDL_Per_Color& pc = wdl->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		const size_t dict_size = r.read<uint16_t>();
		const uint8_t* dict = r.caret();
		r.advance(dict_size);
		pc.dict = LZ4_Dict::load(Const_Span<uint8_t>(dict, dict + dict_size));
	}
	for (const Color c : wdl->m_colors)
	{
		Source_WDL_Per_Color& pc = wdl->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		read_offsets(r, pc.offsets, pc.block_cnt);
	}
	for (const Color c : wdl->m_colors)
	{
		Source_WDL_Per_Color& pc = wdl->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		r.align(64);
		pc.compressed_data = r.caret();
		r.advance(pc.data_size);
	}

	const size_t packed_bytes = ceil_div(epsi.num_positions(), WDL_ENTRY_PACK_RATIO);
	for (const Color c : wdl->m_colors)
	{
		const Source_WDL_Per_Color& pc = wdl->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		if (pc.block_size * num_full + pc.tail_size != packed_bytes)
			throw std::runtime_error("WDL decompressed size mismatch " + path.string());
	}
}

void load_source_dtz(Out_Param<Source_DTZ> dtz, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only,
                     bool allow_relaxed)
{
	load_rank_source(dtz->m_per_color, dtz->m_colors, dtz->m_file, ps, path,
	                 EGTB_Magic::DTZ_MAGIC, "DTZ", allow_loss_only, allow_relaxed);
}

void load_source_dtm(Out_Param<Source_DTM> dtm, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only)
{
	load_rank_source(dtm->m_per_color, dtm->m_colors, dtm->m_file, ps, path,
	                 EGTB_Magic::DTM_MAGIC, "DTM", allow_loss_only,
	                 /*allow_relaxed=*/false);
}

WDL_Stored Source_WDL::read(Color color, Board_Index pos) const
{
	const Source_WDL_Per_Color& pc = m_per_color[color];
	if (pc.is_singular) return pc.single_val;
	ASSERT(pc.compressed_data != nullptr);  // mirror-only frame; see m_colors

	const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
	const size_t packed_byte = storage_pos / WDL_ENTRY_PACK_RATIO;
	const size_t block_id = packed_byte / pc.block_size;
	const size_t in_block = packed_byte % pc.block_size;

	const auto pair = pc.offsets.get2(block_id);
	if (pair[0] == pair[1]) return WDL_Stored::ILLEGAL;

	const uint8_t* point_index = fetch_block_cached<Source_WDL, true>(m_cache[color], block_id, [&pc](
		Block_Cache&, size_t bid) -> Block_Ptr
	{
		const auto pr = pc.offsets.get2(bid);
		const size_t out_sz =
			(bid == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
		auto index = std::make_shared<std::vector<uint8_t>>(lz4_point_index_bytes(out_sz));
		lz4_build_point_index(
			Const_Span<uint8_t>(pc.compressed_data + pr[0], pr[1] - pr[0]),
			Span<uint8_t>(index->data(), index->size()));
		return index;
	});

	const Packed_WDL_Entries packed = static_cast<Packed_WDL_Entries>(lz4_point_read(
		Const_Span<uint8_t>(pc.compressed_data + pair[0], pair[1] - pair[0]),
		Const_Span<uint8_t>(pc.dict.data(), pc.dict.size()),
		point_index, in_block));
	return get_wdl_value(packed, storage_pos % WDL_ENTRY_PACK_RATIO);
}

DTZ_Final_Entry Source_DTZ::read(Color color, Board_Index pos) const
{
	const WDL_Entry w = wdl_from_storage(m_wdl.read(color, pos));
	ASSERT(w != WDL_Entry::ILLEGAL);
	if (w == WDL_Entry::DRAW) return DTZ_Final_Entry::make_draw();

	const Source_Rank_Per_Color& pc = m_per_color[color];
	return dtz_entry_from_storage(rank_cell<Source_DTZ>(pc, m_cache[color], pos), w,
	                              pc.entry_bytes);
}

DTM_Final_Entry Source_DTM::read(Color color, Board_Index pos) const
{
	const WDL_Entry w = wdl_from_storage(m_wdl.read(color, pos));
	ASSERT(w != WDL_Entry::ILLEGAL);
	if (w == WDL_Entry::DRAW) return DTM_Final_Entry::make_draw();

	return dtm_entry_from_storage(rank_cell<Source_DTM>(m_per_color[color], m_cache[color], pos), w);
}

template <typename Traits>
void load_source_layered(Out_Param<Source_Layered<Traits>> src, const Piece_Config& ps,
                         const std::filesystem::path& path, bool allow_loss_only)
{
	constexpr EGTB_Magic magic = Traits::MAGIC;
	constexpr const char* kind = Traits::NAME;

	open_source_file(src->m_file, path, kind);
	Serial_Memory_Reader r(src->m_file.data_span());
	src->m_colors = read_prologue(r, ps, path, magic, kind);

	const Piece_Config_For_Gen epsi(ps);

	for (const Color c : src->m_colors)
	{
		Source_Layered_Per_Color& pc = src->m_per_color[c];
		const uint8_t flag = read_frame_flag(r, path, kind, allow_loss_only,
		                                     /*allow_relaxed=*/false);
		if (flag & EGTB_DROPPED_FLAG)
		{
			pc.is_dropped = true;
			continue;
		}
		if (flag & EGTB_SINGULAR_FLAG)
		{
			pc.is_singular = true;
			if (r.read<uint8_t>() != 0)
				throw std::runtime_error(std::string(kind) + " singular value must be DRAW " + path.string());
			continue;
		}
		pc.plan = make_index_permutation_plan(epsi, read_perm(r, epsi, path, kind));
		pc.entry_bytes = r.read<uint8_t>();
		if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
			throw std::runtime_error(std::string("Bad ") + kind + " entry_bytes " + path.string());
		pc.block_positions = r.read<uint32_t>();
		pc.block_cnt       = r.read<uint64_t>();
		pc.tail_positions  = r.read<uint32_t>();
		pc.data_size       = r.read<uint64_t>();
		const size_t num_ranks = r.read<uint16_t>();
		pc.rank_to_value.resize(num_ranks);
		for (size_t i = 0; i < num_ranks; ++i)
			pc.rank_to_value[i] = r.read<uint16_t>();
	}

	for (const Color c : src->m_colors)
	{
		Source_Layered_Per_Color& pc = src->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		const uint8_t log2_bu      = r.read<uint8_t>();
		const uint8_t sample_width = r.read<uint8_t>();
		const uint8_t offset_width = r.read<uint8_t>();
		const uint8_t usz_width    = r.read<uint8_t>();
		const uint8_t* mono_ptr = r.caret();
		r.advance(Mono_Uint_Vec::on_disk_bytes(pc.block_cnt + 1, log2_bu, sample_width, offset_width));
		const uint8_t* usz_ptr = r.caret();
		r.advance(Min0_Uint_Vec::on_disk_bytes(pc.block_cnt, usz_width));
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		pc.usizes = Min0_Uint_Vec(usz_ptr, pc.block_cnt, usz_width);
	}
	for (const Color c : src->m_colors)
	{
		Source_Layered_Per_Color& pc = src->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		r.align(64);
		pc.compressed_data = r.caret();
		r.advance(pc.data_size);
	}

	const size_t num_positions = epsi.num_positions();
	for (const Color c : src->m_colors)
	{
		const Source_Layered_Per_Color& pc = src->m_per_color[c];
		if (pc.is_singular || pc.is_dropped) continue;
		const size_t full = pc.tail_positions != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t got = pc.block_positions * full + pc.tail_positions;
		if (got != num_positions)
			throw std::runtime_error(std::string(kind) + " position count mismatch: got " + std::to_string(got)
				+ ", want " + std::to_string(num_positions) + " " + path.string());
	}
}

template <typename Source>
NODISCARD static bool layered_locate(const Source_Layered_Per_Color& pc, Block_Cache& cache,
                                     Board_Index pos, const uint8_t** entry, uint8_t* state)
{
	if (pc.is_singular) return false;

	const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
	const size_t block_id = storage_pos / pc.block_positions;
	const size_t in_block = storage_pos % pc.block_positions;

	const auto pair = pc.offsets.get2(block_id);
	if (pair[0] == pair[1]) return false;

	const uint8_t* blob = fetch_block_cached<Source>(cache, block_id,
		[&pc](Block_Cache&, size_t bid) { return decode_rs_block(pc, bid); });

	uint32_t np, payload_off, off;
	std::memcpy(&np, blob, 4);
	std::memcpy(&payload_off, blob + 4, 4);
	std::memcpy(&off, blob + RS_DIR_HEADER + np + in_block * 4u, 4);
	*state = blob[RS_DIR_HEADER + in_block];
	*entry = blob + payload_off + off;
	return true;
}

template <typename Traits>
uint16_t Source_Layered<Traits>::read_base(Color color, Board_Index pos) const
{
	const uint8_t* e;
	uint8_t state;
	if (!layered_locate<Source_Layered<Traits>>(m_per_color[color], m_cache[color], pos, &e, &state))
		return 0;

	// CONST holds the rank outright; the others open with h bytes or a bitmap,
	// layer 0 being the first rank past them.
	static constexpr size_t FIRST_RANK[4] =
		{ 0, 1, 2, 1 + layered_bitmap_bytes(Traits::LAYERS) };
	return m_per_color[color].rank_to_value[
		rs_rank(e + FIRST_RANK[state], m_per_color[color].entry_bytes)];
}

template <typename Traits>
void Source_Layered<Traits>::read_column(Color color, Board_Index pos,
                                         uint16_t* out, size_t stride) const
{
	const Source_Layered_Per_Color& pc = m_per_color[color];
	constexpr size_t layers = Traits::LAYERS;

	const uint8_t* e;
	uint8_t state;
	if (!layered_locate<Source_Layered<Traits>>(pc, m_cache[color], pos, &e, &state))
	{
		out[0] = DTM_Final_Entry::ILLEGAL_VAL;
		return;
	}

	const size_t eb = pc.entry_bytes;
	const auto& r2v = pc.rank_to_value;
	switch (state)
	{
		case 0:
		{
			const uint16_t v = r2v[rs_rank(e, eb)];
			for (size_t i = 0; i < layers; ++i) out[i * stride] = v;
			break;
		}
		case 1:
		{
			const size_t h = e[0] & 0x7Fu;
			const bool draw_end = (e[0] & 0x80u) != 0;
			const uint16_t r0 = r2v[rs_rank(e + 1, eb)];
			const uint16_t tail = draw_end ? DTM_Final_Entry::ILLEGAL_VAL : r2v[rs_rank(e + 1 + eb, eb)];
			const size_t end = draw_end ? h + 1 : layers;
			for (size_t i = 0; i < end; ++i) out[i * stride] = (i < h) ? r0 : tail;
			break;
		}
		case 2:
		{
			const size_t h1 = e[0];
			const size_t h2 = e[1] & 0x7Fu;
			const bool draw_end = (e[1] & 0x80u) != 0;
			const uint16_t r0 = r2v[rs_rank(e + 2, eb)];
			const uint16_t r1 = r2v[rs_rank(e + 2 + eb, eb)];
			const uint16_t tail = draw_end ? DTM_Final_Entry::ILLEGAL_VAL : r2v[rs_rank(e + 2 + 2 * eb, eb)];
			const size_t end = draw_end ? h2 + 1 : layers;
			for (size_t i = 0; i < end; ++i)
				out[i * stride] = (i < h1) ? r0 : (i < h2) ? r1 : tail;
			break;
		}
		default:
		{
			const size_t k = e[0] & 0x7Fu;
			const bool draw_end = (e[0] & 0x80u) != 0;
			constexpr size_t bm_bytes = layered_bitmap_bytes(layers);
			constexpr size_t bm_words = (bm_bytes + 7) / 8;
			const uint8_t* const ranks = e + 1 + bm_bytes;
			uint64_t bm[bm_words] = {};
			std::memcpy(bm, e + 1, bm_bytes);
			const uint16_t tail = draw_end ? DTM_Final_Entry::ILLEGAL_VAL : r2v[rs_rank(ranks + (k - 1) * eb, eb)];
			size_t end = layers;
			if (draw_end)
			{
				end = 0;
				for (size_t w = bm_words; w-- > 0; )
					if (bm[w] != 0)
					{
						end = w * 64 + 64 - static_cast<size_t>(__builtin_clzll(bm[w]));
						break;
					}
			}
			for (size_t i = 0; i < end; ++i)
			{
				size_t rsel = 0;
				for (size_t w = 0; w < bm_words && i >= w * 64; ++w)
				{
					const size_t bits = i - w * 64 + 1;
					const uint64_t mask =
						(bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
					rsel += static_cast<size_t>(__builtin_popcountll(bm[w] & mask));
				}
				--rsel;
				out[i * stride] = (rsel == k - 1) ? tail : r2v[rs_rank(ranks + rsel * eb, eb)];
			}
			break;
		}
	}
}

template struct Source_Layered<DTM50_Source_Traits>;
template struct Source_Layered<DTC_Source_Traits>;
