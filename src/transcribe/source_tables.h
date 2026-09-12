#pragma once

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "egtb/egtb_compress.h"
#include "egtb/index_permutation_plan.h"

#include "util/cache.h"
#include "util/compress.h"
#include "util/defines.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"
#include "util/param.h"

#include <filesystem>
#include <vector>

NODISCARD std::array<uint32_t, COLOR_NB> read_table_permutations(
	const Piece_Config& ps, const std::filesystem::path& path, EGTB_Magic magic);

struct Source_WDL_Per_Color
{
	bool is_dropped = false;
	Index_Permutation_Plan plan;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	size_t data_size = 0;
	Mono_Uint_Vec offsets;
	const uint8_t* compressed_data = nullptr;
	LZ4_Dict dict;
	bool is_singular = false;
	WDL_Stored single_val = WDL_Stored::DRAW;
};

struct Source_WDL;

void load_source_wdl(Out_Param<Source_WDL> wdl, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only,
                     bool allow_relaxed);

struct Source_WDL
{
	static constexpr size_t TL_BLOCK_SLOTS = 128;

	Source_WDL() = default;
	Source_WDL(const Source_WDL&) = delete;
	Source_WDL& operator=(const Source_WDL&) = delete;

	void open(const Piece_Config& ps, const std::filesystem::path& path,
	          bool allow_loss_only, bool allow_relaxed)
	{
		load_source_wdl(out_param(*this), ps, path, allow_loss_only, allow_relaxed);
	}

	NODISCARD WDL_Stored read(Color color, Board_Index pos) const;

	// Colors this file stores; BLACK is absent on a symmetric material, being
	// WHITE mirrored rather than a frame of its own.
	Fixed_Vector<Color, 2> m_colors;
	Source_WDL_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	mutable Block_Cache m_cache[COLOR_NB];
};

// DTZ and DTM share the rank-encoded layout; only the value decode differs.
struct Source_Rank_Per_Color
{
	bool is_dropped = false;
	Index_Permutation_Plan plan;
	size_t entry_bytes = 0;
	bool is_singular = false;
	uint16_t single_val = 0;
	size_t block_size = 0;
	size_t tail_size = 0;
	size_t block_cnt = 0;
	size_t data_size = 0;
	Mono_Uint_Vec offsets;
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;
};

struct Source_DTZ;
struct Source_DTM;

void load_source_dtz(Out_Param<Source_DTZ> dtz, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only,
                     bool allow_relaxed);
void load_source_dtm(Out_Param<Source_DTM> dtm, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only);

struct Source_DTZ
{
	static constexpr size_t TL_BLOCK_SLOTS = 16;

	Source_DTZ() = default;
	Source_DTZ(const Source_DTZ&) = delete;
	Source_DTZ& operator=(const Source_DTZ&) = delete;

	// `wdl_path` is the material's own companion -- DTZ stores no class.
	void open(const Piece_Config& ps, const std::filesystem::path& path,
	          const std::filesystem::path& wdl_path, bool allow_loss_only,
	          bool allow_relaxed)
	{
		// The companion carries the class every cell decodes against, so it is
		// never a reduced one, whatever form the payload takes.
		m_wdl.open(ps, wdl_path, /*allow_loss_only=*/false, /*allow_relaxed=*/false);
		load_source_dtz(out_param(*this), ps, path, allow_loss_only, allow_relaxed);
	}

	NODISCARD DTZ_Final_Entry read(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Rank_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache m_cache[COLOR_NB];
};

struct Source_DTM
{
	static constexpr size_t TL_BLOCK_SLOTS = 16;

	Source_DTM() = default;
	Source_DTM(const Source_DTM&) = delete;
	Source_DTM& operator=(const Source_DTM&) = delete;

	void open(const Piece_Config& ps, const std::filesystem::path& path,
	          const std::filesystem::path& wdl_path, bool allow_loss_only)
	{
		m_wdl.open(ps, wdl_path, /*allow_loss_only=*/false, /*allow_relaxed=*/false);
		load_source_dtm(out_param(*this), ps, path, allow_loss_only);
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Rank_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache m_cache[COLOR_NB];
};

struct Source_Layered_Per_Color
{
	bool is_dropped = false;
	Index_Permutation_Plan plan;
	size_t entry_bytes = 0;
	size_t block_positions = 0;
	size_t tail_positions = 0;
	size_t block_cnt = 0;
	size_t data_size = 0;
	Mono_Uint_Vec offsets;
	Min0_Uint_Vec usizes;
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;
	bool is_singular = false;
};

struct DTM50_Source_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTM50_MAGIC;
	static constexpr const char* NAME = "DTM50";
	static constexpr size_t LAYERS = DTM50_PACK_LAYERS;
};

struct DTC_Source_Traits
{
	static constexpr EGTB_Magic MAGIC = EGTB_Magic::DTC_MAGIC;
	static constexpr const char* NAME = "DTC";
	static constexpr size_t LAYERS = DTC_PACK_LAYERS;
};

template <typename Traits>
struct Source_Layered;

template <typename Traits>
void load_source_layered(Out_Param<Source_Layered<Traits>> src, const Piece_Config& ps,
                         const std::filesystem::path& path, bool allow_loss_only);

template <typename Traits>
struct Source_Layered
{
	static constexpr size_t TL_BLOCK_SLOTS = 16;
	static constexpr size_t LAYERS = Traits::LAYERS;
	static constexpr const char* NAME = Traits::NAME;

	Source_Layered() = default;
	Source_Layered(const Source_Layered&) = delete;
	Source_Layered& operator=(const Source_Layered&) = delete;

	void open(const Piece_Config& ps, const std::filesystem::path& path,
	          const std::filesystem::path& wdl_path, bool allow_loss_only)
	{
		m_wdl.open(ps, wdl_path, /*allow_loss_only=*/false, /*allow_relaxed=*/false);
		load_source_layered<Traits>(out_param(*this), ps, path, allow_loss_only);
	}

	void read_column(Color color, Board_Index pos, uint16_t* out, size_t stride) const;

	NODISCARD uint16_t read_base(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Layered_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache m_cache[COLOR_NB];
};

extern template struct Source_Layered<DTM50_Source_Traits>;
extern template struct Source_Layered<DTC_Source_Traits>;

using Source_DTM50 = Source_Layered<DTM50_Source_Traits>;
using Source_DTC = Source_Layered<DTC_Source_Traits>;
