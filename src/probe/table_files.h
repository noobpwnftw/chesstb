#pragma once

#include "probe/entry.h"
#include "probe/position_index.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/cache.h"
#include "util/decompress.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"
#include "util/param.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

inline constexpr const char* WDL_EXT = ".lzw";
inline constexpr const char* DTZ_EXT = ".lzdtz";
inline constexpr const char* DTC_EXT = ".lzdtc";
inline constexpr const char* DTM_EXT = ".lzdtm";
inline constexpr const char* DTM50_EXT = ".lzdtm50";
inline constexpr size_t DTM50_HMC_COUNT = 100;
inline constexpr size_t DTM50_PACK_LAYERS = DTM50_HMC_COUNT + 1;
inline constexpr size_t DTC_BUDGET_LAYERS = 30;
inline constexpr size_t DTC_PACK_LAYERS = DTC_BUDGET_LAYERS + 1;
inline constexpr uint16_t DRAWN_AT_LAYER = 0xFFFFu;

inline constexpr uint8_t SINGULAR_FLAG  = 0x80;
inline constexpr uint8_t DROPPED_FLAG   = 0x40;
inline constexpr uint8_t LOSS_ONLY_FLAG = 0x20;
inline constexpr uint8_t RELAXED_FLAG   = 0x10;
inline constexpr uint64_t TABLE_CHECKSUM_INIT = 0xf0f0f0f0f0f0;

NODISCARD INLINE unsigned dtc_budget_plies(unsigned rule50)
{
	if (rule50 == IGNORE_50MR) return DTZ_MAX_NON_CURSED;
	return (rule50 >= DTZ_MAX_NON_CURSED) ? 0u : DTZ_MAX_NON_CURSED - rule50;
}

// Composition avoids GCC PGO misinferring the derived object size from
// a Block_Cache base (-Warray-bounds).
struct WDL_Per_Color
{
	explicit WDL_Per_Color(std::shared_ptr<Block_Pool> pool = default_block_pool()) :
		cache(std::move(pool))
	{}

	Block_Cache cache;

	Index_Storage_Layout layout;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;
	const uint8_t* compressed_data = nullptr;
	Const_Span<uint8_t> dict;  // in the mapped file
	WDL_Stored single_val = WDL_Stored::DRAW;

	size_t data_size = 0;
};

struct LZMA_Rank_Per_Color
{
	explicit LZMA_Rank_Per_Color(std::shared_ptr<Block_Pool> pool = default_block_pool()) :
		cache(std::move(pool))
	{}

	Block_Cache cache;

	Index_Storage_Layout layout;
	size_t entry_bytes = 0;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;
	uint16_t single_val = 0;

	size_t data_size = 0;
};

NODISCARD INLINE Block_Ptr lzma_rank_get_block(LZMA_Rank_Per_Color& pc, size_t block_id)
{
	const size_t decode_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;

	const auto pair = pc.offsets.get2(block_id);
	const size_t doff = pair[0];
	const size_t dsz  = pair[1] - pair[0];
	ASSERT(dsz != 0);

	auto buf = std::make_shared<std::vector<uint8_t>>(decode_sz, 0);
	if (!lzma_decode(buf->data(), decode_sz, pc.compressed_data + doff, dsz))
		throw std::runtime_error("LZMA error when trying to decompress a block.");
	return buf;
}

// The value of the rank stored at p, in entry_bytes.
template <typename Per_Color>
NODISCARD uint16_t rank_value(const Per_Color& pc, const uint8_t* p)
{
	if (pc.entry_bytes == 1) return pc.rank_to_value[*p];
	return pc.rank_to_value[read_le<uint16_t>(p)];
}

struct Layered_Rank_Per_Color
{
	explicit Layered_Rank_Per_Color(std::shared_ptr<Block_Pool> pool = default_block_pool()) :
		cache(std::move(pool))
	{}

	Block_Cache cache;

	Index_Storage_Layout layout;
	size_t entry_bytes = 0;
	size_t block_positions = 0;
	size_t tail_positions = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;
	Min0_Uint_Vec usizes;
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;

	size_t data_size = 0;
};

using DTM50_Rank_Per_Color = Layered_Rank_Per_Color;

template <typename Traits>
struct Table_File
{
	using Per_Color = typename Traits::Per_Color;

	bool is_singular[COLOR_NB]  = { false, false };
	bool is_dropped[COLOR_NB]   = { false, false };
	bool is_loss_only[COLOR_NB] = { false, false };
	bool is_relaxed[COLOR_NB]   = { false, false };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped{ Memory_Mapped_File::Access_Advice::RANDOM };
	std::unique_ptr<Position_Index_Config> index_cfg;

	explicit Table_File(std::shared_ptr<Block_Pool> pool = default_block_pool()) :
		per_color{ Per_Color(pool), Per_Color(pool) }
	{
		static_assert(COLOR_NB == 2);
	}

	void load(const Piece_Config& ps, const std::filesystem::path& path);

	NODISCARD Board_Index storage_index(Color c, const Position& pos) const
	{
		ASSERT(!is_dropped[c]);
		if (is_singular[c]) return BOARD_INDEX_ZERO;
		return board_index_of_position(*index_cfg, per_color[c].layout, pos);
	}

	template <typename... Args>
	auto read(Color c, const Position& pos, Args... args)
	{
		const Board_Index storage_pos = storage_index(c, pos);
		return Traits::read(per_color[c], is_singular[c], storage_pos, args...);
	}

	template <typename... Args>
	auto read_curve(Color c, const Position& pos, Args... args)
	{
		const Board_Index storage_pos = storage_index(c, pos);
		return Traits::read_curve(per_color[c], is_singular[c], storage_pos, args...);
	}
};

template <typename Traits>
void Table_File<Traits>::load(const Piece_Config& ps, const std::filesystem::path& path)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error(std::string("Cannot open ") + Traits::NAME + " file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " file size " + path.string());

	Serial_Memory_Reader reader(input);

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(Traits::MAGIC))
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const uint32_t key = key_and_table_num >> 2u;
	if (key != (ps.min_material_key().value() & 0x3FFFFFFFu))
		throw std::runtime_error(std::string("Wrong material key in ") + Traits::NAME + " " + path.string());
	index_cfg = std::make_unique<Position_Index_Config>(ps);

	// Frames stored: WHITE, and BLACK unless the material is symmetric (1).
	const size_t table_num = key_and_table_num & 3;
	if (table_num == 0 || table_num > COLOR_NB)
		throw std::runtime_error(std::string("Invalid table count in ") + Traits::NAME + " " + path.string());

	for (size_t c = 0; c < table_num; ++c)
	{
		const Color i = static_cast<Color>(c);
		const uint8_t flag = reader.read<uint8_t>();
		is_loss_only[i] = (flag & LOSS_ONLY_FLAG) != 0;
		is_relaxed[i] = (flag & RELAXED_FLAG) != 0;
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			if (!Traits::on_singular(reader, per_color[i]))
				throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " singular value " + path.string());
		}
		else if (flag & DROPPED_FLAG)
		{
			is_dropped[i] = true;
		}
		else
		{
			Traits::parse_header(reader, per_color[i], *index_cfg, path);
		}
	}
	if (table_num == 1)
	{
		// Symmetric material: BLACK is WHITE mirrored, flag byte and all.
		is_dropped[BLACK] = true;
		is_loss_only[BLACK] = is_loss_only[WHITE];
		is_relaxed[BLACK] = is_relaxed[WHITE];
	}

	Traits::finalize(reader, per_color, is_singular, is_dropped);
}

NODISCARD constexpr WDL_Stored get_wdl_value(uint8_t packed, size_t pos)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	return static_cast<WDL_Stored>((packed >> (pos * WDL_ENTRY_BITS)) & 0xF);
}

struct WDL_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::WDL_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 128;
	static constexpr const char* NAME = "WDL";
	using Per_Color = WDL_Per_Color;

	NODISCARD static bool on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB]);
	NODISCARD static WDL_Stored read(Per_Color& pc, bool is_singular, Board_Index pos);
};

struct DTZ_Value
{
	uint16_t dtz;
	bool rounded;
};

NODISCARD constexpr DTZ_Value dtz_value_from_storage(
	uint16_t stored, WDL_Entry w, size_t entry_bytes)
{
	if (w == WDL_Entry::DRAW) return DTZ_Value{ 0, false };
	if (entry_bytes == 1 && (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS))
		return DTZ_Value{ static_cast<uint16_t>((stored << 1) - 1), true };
	return DTZ_Value{ stored, false };
}

// The loaders the rank tables share: DTZ and DTM files differ only in what a
// value means, as do DTC and DTM50 files.
struct LZMA_Rank_Loader
{
	using Per_Color = LZMA_Rank_Per_Color;

	NODISCARD static bool on_singular(Serial_Memory_Reader& reader, Per_Color& pc)
	{
		pc.single_val = reader.read<uint8_t>();
		return true;
	}

	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path)
	{
		const uint32_t perm = reader.read<uint32_t>();
		if (!index_permutation_config_is_valid(index_cfg, perm))
			throw std::runtime_error("Invalid index permutation config " + path.string());
		pc.layout = make_index_storage_layout(index_cfg, perm);

		pc.entry_bytes = reader.read<uint8_t>();
		if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
			throw std::runtime_error("Bad entry_bytes " + path.string());
		pc.tail_size  = reader.read<uint32_t>();
		pc.block_size = reader.read<uint32_t>();
		pc.block_cnt  = reader.read<uint64_t>();
		pc.data_size  = reader.read<uint64_t>();

		const size_t num_ranks = reader.read<uint16_t>();
		pc.rank_to_value.resize(num_ranks);
		for (size_t r = 0; r < num_ranks; ++r)
			pc.rank_to_value[r] = reader.read<uint16_t>();
	}

	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB])
	{
		for (Color i : { WHITE, BLACK })
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
		for (Color i : { WHITE, BLACK })
		{
			if (is_singular[i] || is_dropped[i]) continue;
			reader.align(64);
			per_color[i].compressed_data = reader.caret();
			reader.advance(per_color[i].data_size);
		}
	}
};

struct Layered_Rank_Loader
{
	using Per_Color = Layered_Rank_Per_Color;

	NODISCARD static bool on_singular(Serial_Memory_Reader& reader, Per_Color&)
	{
		return reader.read<uint8_t>() == 0;  // the singular value must be DRAW
	}

	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path)
	{
		const uint32_t perm = reader.read<uint32_t>();
		if (!index_permutation_config_is_valid(index_cfg, perm))
			throw std::runtime_error("Invalid index permutation config " + path.string());
		pc.layout = make_index_storage_layout(index_cfg, perm);

		pc.entry_bytes = reader.read<uint8_t>();
		if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
			throw std::runtime_error("Bad entry_bytes " + path.string());
		pc.block_positions = reader.read<uint32_t>();
		pc.block_cnt       = reader.read<uint64_t>();
		pc.tail_positions  = reader.read<uint32_t>();
		pc.data_size       = reader.read<uint64_t>();

		const size_t num_ranks = reader.read<uint16_t>();
		pc.rank_to_value.resize(num_ranks);
		for (size_t r = 0; r < num_ranks; ++r)
			pc.rank_to_value[r] = reader.read<uint16_t>();
	}

	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB])
	{
		for (Color i : { WHITE, BLACK })
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
		for (Color i : { WHITE, BLACK })
		{
			if (is_singular[i] || is_dropped[i]) continue;
			reader.align(64);
			per_color[i].compressed_data = reader.caret();
			reader.advance(per_color[i].data_size);
		}
	}
};

struct DTZ_Traits : LZMA_Rank_Loader
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTZ_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTZ";

	NODISCARD static DTZ_Value read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

struct DTC_Cell
{
	uint16_t order = 0;
	uint16_t value = DRAWN_AT_LAYER;
	uint16_t dtz = DRAWN_AT_LAYER;
	bool dtz_rounded = false;

	NODISCARD bool priced() const { return value != DRAWN_AT_LAYER; }
};

struct DTC_Curve
{
	uint16_t value[DTC_PACK_LAYERS];
};

struct DTC_Traits : Layered_Rank_Loader
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTC_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTC";

	NODISCARD static DTC_Cell read(Per_Color& pc, bool is_singular, Board_Index pos,
	                               WDL_Entry wdl, unsigned rule50);
	static void read_curve(Per_Color& pc, bool is_singular, Board_Index pos,
	                       WDL_Entry wdl, Out_Param<DTC_Curve> curve);
};

NODISCARD constexpr uint16_t dtm_value_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::WIN:
		case WDL_Entry::CURSED_WIN:   return static_cast<uint16_t>((stored << 1) | 1u);
		case WDL_Entry::LOSE:
		case WDL_Entry::BLESSED_LOSS: return static_cast<uint16_t>(stored << 1);
		case WDL_Entry::DRAW:
		case WDL_Entry::ILLEGAL:
		default:                      return 0;
	}
}

struct DTM_Traits : LZMA_Rank_Loader
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTM";

	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

// One DTM50 cell. `draw_flip` is the layer where the position turns DRAW, 0 if
// it never does; for a plain WIN/LOSE cell it pins DTZ. `value` is DRAWN_AT_LAYER
// when the probed layer is DRAW.
struct DTM50_Cell
{
	uint16_t value = DRAWN_AT_LAYER;
	uint16_t draw_flip = 0;
};

NODISCARD constexpr uint16_t dtm50_value_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::WIN:  return static_cast<uint16_t>((stored << 1) | 1u);
		case WDL_Entry::LOSE: return static_cast<uint16_t>(stored << 1);
		case WDL_Entry::CURSED_WIN:
		case WDL_Entry::BLESSED_LOSS:
		case WDL_Entry::DRAW:
		case WDL_Entry::ILLEGAL:
		default:              return 0;
	}
}

struct DTM50_Traits : Layered_Rank_Loader
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM50_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTM50";

	NODISCARD static DTM50_Cell read(Per_Color& pc, bool is_singular, Board_Index pos,
	                                 WDL_Entry wdl, unsigned hmc);
};

using WDL_File   = Table_File<WDL_Traits>;
using DTZ_File   = Table_File<DTZ_Traits>;
using DTC_File   = Table_File<DTC_Traits>;
using DTM_File   = Table_File<DTM_Traits>;
using DTM50_File = Table_File<DTM50_Traits>;

// Each type is instantiated once, in its own translation unit.
extern template struct Table_File<WDL_Traits>;
extern template struct Table_File<DTZ_Traits>;
extern template struct Table_File<DTC_Traits>;
extern template struct Table_File<DTM_Traits>;
extern template struct Table_File<DTM50_Traits>;
