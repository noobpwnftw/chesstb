#pragma once

#include "probe/entry.h"
#include "probe/position_index.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/cache.h"
#include "util/compress.h"
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
// An 8-man winner has at most six pawns, each with at most five non-converting
// pushes, so its budget curve has at most 30 relevant changepoints. For clean
// W/L the terminal one is exactly DTZ: solve the preceding 29 and embed that
// endpoint at row 0. Cursed/blessed cells use row 0 only as DTZ.
inline constexpr size_t DTC_BUDGET_LAYERS = 29;
inline constexpr size_t DTC_PACK_LAYERS = DTC_BUDGET_LAYERS + 1;

inline constexpr uint8_t SINGULAR_FLAG  = 0x80;
inline constexpr uint8_t DROPPED_FLAG   = 0x40;
inline constexpr uint8_t LOSS_ONLY_FLAG = 0x20;
inline constexpr uint8_t RELAXED_FLAG   = 0x10;
inline constexpr uint64_t TABLE_CHECKSUM_INIT = 0xf0f0f0f0f0f0;
inline constexpr unsigned IGNORE_50MR = ~0u;

// Plies a DTC answer may still spend before the 50-move claim: the whole band
// where the caller ignores the rule, and none at all once the clock has reached
// the claim, where only a mate already on the board, at distance 0, outruns it.
NODISCARD INLINE unsigned dtc_budget_plies(unsigned rule50)
{
	if (rule50 == IGNORE_50MR) return DTZ_MAX_NON_CURSED;
	return (rule50 >= DTZ_MAX_NON_CURSED) ? 0u : DTZ_MAX_NON_CURSED - rule50;
}

struct WDL_Per_Color : Block_Cache<LZ4_Decompress_Helper>
{
	using Block_Cache::Block_Cache;

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

struct LZMA_Rank_Per_Color : Block_Cache<>
{
	using Block_Cache::Block_Cache;

	Index_Storage_Layout layout;
	size_t entry_bytes = 0;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	Mono_Uint_Vec offsets;   // (block_cnt + 1) cumulative compressed offsets
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
	ASSERT(dsz != 0);  // read() short-circuits the skip sentinel before us

	auto buf = std::make_shared<std::vector<uint8_t>>(decode_sz, 0);
	lzma_decompress_into(
		Span<uint8_t>(buf->data(), decode_sz),
		Const_Span<uint8_t>(pc.compressed_data + doff, dsz));
	return buf;
}

NODISCARD INLINE uint16_t lzma_rank_value(const LZMA_Rank_Per_Color& pc,
                                          const uint8_t* block, size_t in_block_pos)
{
	if (pc.entry_bytes == 1) return pc.rank_to_value[block[in_block_pos]];
	uint16_t rank;
	std::memcpy(&rank, block + in_block_pos * sizeof(uint16_t), sizeof(rank));
	return pc.rank_to_value[rank];
}

struct Layered_Rank_Per_Color : Block_Cache<>
{
	using Block_Cache::Block_Cache;

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
	Memory_Mapped_File mapped;
	std::unique_ptr<Position_Index_Config> index_cfg;

	explicit Table_File(std::shared_ptr<Block_Pool> pool = default_block_pool()) :
		per_color{ Per_Color(pool), Per_Color(pool) }
	{
		static_assert(COLOR_NB == 2);
	}

	void load(const Piece_Config& ps, const std::filesystem::path& path,
	          bool verify_checksum = false);

	// Where a singular frame has no index to resolve, its one value stands in for
	// every position.
	NODISCARD Board_Index storage_index(Color c, const Position& pos) const
	{
		ASSERT(!is_dropped[c]);
		if (is_singular[c]) return BOARD_INDEX_ZERO;
		return board_index_of_position(*index_cfg, per_color[c].layout, pos);
	}

	// Resolve `pos` to its storage index and hand it to Traits::read; trailing
	// args pass through.
	template <typename... Args>
	auto read(Color c, const Position& pos, Args... args)
	{
		const Board_Index storage_pos = storage_index(c, pos);
		return Traits::read(per_color[c], is_singular[c], storage_pos, args...);
	}

	// Same, for the metric whose record holds more than the cell a read returns.
	template <typename... Args>
	auto read_curve(Color c, const Position& pos, Args... args)
	{
		const Board_Index storage_pos = storage_index(c, pos);
		return Traits::read_curve(per_color[c], is_singular[c], storage_pos, args...);
	}
};

template <typename Traits>
void Table_File<Traits>::load(const Piece_Config& ps, const std::filesystem::path& path,
                              bool verify_checksum)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error(std::string("Cannot open ") + Traits::NAME + " file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " file size " + path.string());

	Serial_Memory_Reader reader(input);
	if (verify_checksum && !reader.is_end_checksum_ok(TABLE_CHECKSUM_INIT))
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(Traits::MAGIC))
		throw std::runtime_error(std::string("Invalid ") + Traits::NAME + " magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const uint32_t key = key_and_table_num >> 2u;
	if (key != ps.min_material_key().value())
		throw std::runtime_error(std::string("Wrong material key in ") + Traits::NAME + " " + path.string());
	index_cfg = std::make_unique<Position_Index_Config>(ps);

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		is_loss_only[i] = (flag & LOSS_ONLY_FLAG) != 0;
		is_relaxed[i] = (flag & RELAXED_FLAG) != 0;
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
	if (table_num == 1)
	{
		// Symmetric material: BLACK is WHITE mirrored, flag byte and all.
		is_dropped[BLACK] = true;
		is_loss_only[BLACK] = is_loss_only[WHITE];
		is_relaxed[BLACK] = is_relaxed[WHITE];
	}

	Traits::finalize(reader, per_color, is_singular, is_dropped, table_colors);
}

struct WDL_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::WDL_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 128;
	static constexpr const char* NAME = "WDL";
	using Per_Color = WDL_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors);
	NODISCARD static WDL_Stored read(Per_Color& pc, bool is_singular, Board_Index pos);
};

struct DTZ_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTZ_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTZ";
	using Per_Color = LZMA_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors);
	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

// One DTC cell: pushes the winning side still owes before a conversion, and
// plies to the next zeroing move on the line that owes them. Both come from the
// budget layer that fits the clock the caller passed, so a fresh clock buys the
// fewest pushes any line manages, and a tighter one may force more of them or
// leave nothing that fits at all.
//
// DRAWN says no budget fits that clock, which is not the same as value 0 --
// zero is a mate, a terminal distance the pack stores like any other.
struct DTC_Cell
{
	// Same sentinel DTM50's cell uses, for the same thing: a layer settling nothing.
	static constexpr uint16_t DRAWN = 0xFFFFu;

	uint16_t order = 0;
	uint16_t value = DRAWN;
	// The record's unbounded row, which is the DTZ table the pack embeds, in that
	// table's own plies: the same decode that picks a budget passes it, and a
	// cursed class has only it.
	uint16_t dtz = DRAWN;

	NODISCARD bool priced() const { return value != DRAWN; }
};

// One position's whole record, budget-major. The first 29 points are solved
// finite budgets; the top entry is the embedded DTZ terminal endpoint. DRAWN
// marks a point that does not settle the cell. A read needs the point its clock
// picks; a derive needs the whole curve, since a loss is settled by its worst
// defence and only then is the value read off it.
struct DTC_Curve
{
	uint16_t value[DTC_PACK_LAYERS];
};

struct DTC_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTC_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTC";
	using Per_Color = Layered_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors);
	NODISCARD static DTC_Cell read(Per_Color& pc, bool is_singular, Board_Index pos,
	                               WDL_Entry wdl, unsigned rule50);
	// The whole record, for a caller minimaxing over budgets rather than reading
	// the one its clock picks, or wanting the unbounded row the pack embeds.
	static void read_curve(Per_Color& pc, bool is_singular, Board_Index pos,
	                       WDL_Entry wdl, Out_Param<DTC_Curve> curve);
};

struct DTM_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTM";
	using Per_Color = LZMA_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors);
	NODISCARD static uint16_t read(Per_Color& pc, bool is_singular, Board_Index pos, WDL_Entry wdl);
};

// One DTM50 cell. `draw_flip` is the layer where the position turns DRAW, 0 if
// it never does; for a plain WIN/LOSE cell it pins DTZ. `value` is DRAWN when
// the probed layer is DRAW.
struct DTM50_Cell
{
	static constexpr uint16_t DRAWN = 0xFFFFu;

	uint16_t value = DRAWN;
	uint16_t draw_flip = 0;
};

struct DTM50_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM50_MAGIC;
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr const char* NAME = "DTM50";
	using Per_Color = DTM50_Rank_Per_Color;

	static void on_singular(Serial_Memory_Reader& reader, Per_Color& pc);
	static void parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
	                         const Position_Index_Config& index_cfg,
	                         const std::filesystem::path& path);
	static void finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
	                     const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
	                     const Fixed_Vector<Color, 2>& table_colors);
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
