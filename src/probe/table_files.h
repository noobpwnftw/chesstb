#pragma once

#include "probe/cache.h"
#include "probe/entry.h"
#include "probe/position_index.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

constexpr const char* WDL_EXT = ".lzw";
constexpr const char* DTC_EXT = ".lzdtc";
constexpr const char* DTM_EXT = ".lzdtm";
constexpr const char* DTM50_EXT = ".lzdtm50";
constexpr size_t DTM50_HMC_COUNT = 100;
constexpr uint8_t SINGULAR_FLAG = 0x80;
constexpr uint8_t DROPPED_FLAG  = 0x40;
constexpr uint64_t TABLE_CHECKSUM_INIT = 0xf0f0f0f0f0f0;

struct WDL_Per_Color : Block_Cache<LZ4_Decompress_Helper>
{
	Index_Storage_Layout layout;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;   // (block_cnt + 1) cumulative compressed offsets
	const uint8_t* compressed_data = nullptr;
	LZ4_Dict dict;
	WDL_Stored single_val = WDL_Stored::DRAW;

	// load scratch
	size_t dict_size = 0;
	size_t data_size = 0;
	const uint8_t* lp_dict = nullptr;
};

struct Lzma_Rank_Per_Color : Block_Cache<LZMA_Decompress_Helper>
{
	Index_Storage_Layout layout;
	size_t entry_bytes = 0;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;   // (block_cnt + 1) cumulative compressed offsets
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;

	size_t data_size = 0;
};

struct DTM50_Per_Color : Block_Cache<LZMA_Decompress_Helper>
{
	Index_Storage_Layout layout;
	size_t entry_bytes = 0;
	size_t block_positions = 0;
	size_t tail_positions = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;   // (block_cnt + 1) cumulative compressed offsets
	Min0_Uint_Vec usizes;    // block_cnt uncompressed-payload sizes
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;

	size_t data_size = 0;
};

template <typename Traits>
struct Table_File
{
	using Per_Color = typename Traits::Per_Color;

	bool is_singular[COLOR_NB] = { false, false };
	bool is_dropped[COLOR_NB]  = { false, false };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped;
	std::unique_ptr<Position_Index_Config> index_cfg;

	void load(const Piece_Config& ps, const std::filesystem::path& path);

	// Arity varies by type (WDL has no wdl/hmc args); the trait read() takes
	// whatever the caller passes after the position.
	template <typename... Args>
	auto read(Color c, const Position& pos, Args... args)
	{
		ASSERT(!is_dropped[c]);
		const Board_Index storage_pos =
			board_index_of_position(*index_cfg, per_color[c].layout, pos);
		return Traits::read(per_color[c], is_singular[c], storage_pos, args...);
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
	if (!reader.is_end_checksum_ok(TABLE_CHECKSUM_INIT))
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(Traits::MAGIC))
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error(std::string("Wrong material key in ") + Traits::NAME + " " + path.string());
	index_cfg = std::make_unique<Position_Index_Config>(ps);

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			Traits::on_singular(reader, per_color[i]);
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

	Traits::finalize(reader, per_color, is_singular, is_dropped, table_colors, ps, path);
}

struct WDL_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::WDL_MAGIC;
	static constexpr const char* NAME = "WDL";
	using Per_Color = WDL_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
	                     const std::filesystem::path& path);
	NODISCARD static WDL_Stored read(Per_Color& pc, bool is_singular, Board_Index pos);
};

struct DTC_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTC_MAGIC;
	static constexpr const char* NAME = "DTC";
	using Per_Color = Lzma_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
	                     const std::filesystem::path& path);
	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

struct DTM_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM_MAGIC;
	static constexpr const char* NAME = "DTM";
	using Per_Color = Lzma_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
	                     const std::filesystem::path& path);
	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

struct DTM50_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM50_MAGIC;
	static constexpr const char* NAME = "DTM50";
	using Per_Color = DTM50_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors, const Piece_Config& ps,
	                     const std::filesystem::path& path);
	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos,
	                               WDL_Entry wdl, uint16_t hmc);
};

using WDL_File   = Table_File<WDL_Traits>;
using DTC_File   = Table_File<DTC_Traits>;
using DTM_File   = Table_File<DTM_Traits>;
using DTM50_File = Table_File<DTM50_Traits>;

// Each type is instantiated once, in its own translation unit.
extern template struct Table_File<WDL_Traits>;
extern template struct Table_File<DTC_Traits>;
extern template struct Table_File<DTM_Traits>;
extern template struct Table_File<DTM50_Traits>;
