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

// Transcription sources: by-index readers over finished EGTB files, decoding a
// cell at a time out of an LRU of decompressed blocks.
//
// Separate from egtb_probe.h's *_For_Probe readers on purpose. Those feed the
// generator its sub-tables and may spill a flat copy, a sub-table being smaller
// than the material being built; a transcription reads the material itself,
// whose flat form is the footprint the pager exists to avoid. They also take
// different inputs: dropped and loss-only frames are conditionally accepted.
//
// Indices are logical, each source applying its own file's permutation, so a
// transcription is free to lay its output out under a different one.

// The per-color layouts a finished table was written under, header only. Unlike
// a source this takes dropped and loss-only frames: a layout is a layout.
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
	Mono_Uint_Vec offsets;                 // (block_cnt + 1) cumulative offsets
	const uint8_t* compressed_data = nullptr;
	LZ4_Dict dict;
	bool is_singular = false;
	WDL_Stored single_val = WDL_Stored::DRAW;
};

struct Source_WDL;

void load_source_wdl(Out_Param<Source_WDL> wdl, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only);

// Class companion for every distance source, and a transcription target itself,
// so reads answer WDL_Stored with boundary markers intact. Fold with
// wdl_from_storage where only the class matters.
struct Source_WDL
{
	static constexpr size_t TL_BLOCK_SLOTS = 128;

	Source_WDL() = default;
	Source_WDL(const Source_WDL&) = delete;
	Source_WDL& operator=(const Source_WDL&) = delete;

	void open(const Piece_Config& ps, const std::filesystem::path& path,
	          bool allow_loss_only = false)
	{
		load_source_wdl(out_param(*this), ps, path, allow_loss_only);
	}

	NODISCARD WDL_Stored read(Color color, Board_Index pos) const;

	// Colors this file stores; BLACK is absent on a symmetric material, being
	// WHITE mirrored rather than a frame of its own.
	Fixed_Vector<Color, 2> m_colors;
	Source_WDL_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	mutable Block_Cache<LZ4_Decompress_Helper> m_cache[COLOR_NB];
};

// DTZ and DTM share the rank-encoded layout; only the value decode differs.
struct Source_Rank_Per_Color
{
	bool is_dropped = false;
	Index_Permutation_Plan plan;
	size_t entry_bytes = 0;                // rank-index width: 1 or 2 bytes
	bool is_singular = false;
	uint16_t single_val = 0;
	size_t block_size = 0;                 // rank bytes per full block
	size_t tail_size = 0;
	size_t block_cnt = 0;
	size_t data_size = 0;
	Mono_Uint_Vec offsets;                 // (block_cnt + 1) cumulative offsets
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;
};

struct Source_DTZ;
struct Source_DTM;

// `allow_loss_only` admits a loss-only source, which only a loss-only
// transcription may take: the same cells are don't-cares on both sides. A
// dropped frame is always admitted -- it has no payload to re-encode, so it
// passes through as a drop -- except into loss-only, where the frame that
// survives would have nothing to derive its wins from.
void load_source_dtz(Out_Param<Source_DTZ> dtz, const Piece_Config& ps,
                     const std::filesystem::path& path, bool allow_loss_only);
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
	          const std::filesystem::path& wdl_path, bool allow_loss_only)
	{
		m_wdl.open(ps, wdl_path);
		load_source_dtz(out_param(*this), ps, path, allow_loss_only);
	}

	NODISCARD DTZ_Final_Entry read(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Rank_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache<> m_cache[COLOR_NB];
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
		m_wdl.open(ps, wdl_path);
		load_source_dtm(out_param(*this), ps, path, allow_loss_only);
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Rank_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache<> m_cache[COLOR_NB];
};

// Either changepoint pack: one record per position covers every layer of the
// stack -- DTM50's 101 (0 the unbounded DTM, 1..100 the hmc layers) or DTC's 30
// rows (0 the embedded DTZ terminal changepoint, then its 29 separately solved
// predecessors) -- so a read hands back the whole column.
struct Source_Layered_Per_Color
{
	bool is_dropped = false;
	Index_Permutation_Plan plan;
	size_t entry_bytes = 0;                // rank-index width: 1 or 2 bytes
	size_t block_positions = 0;
	size_t tail_positions = 0;
	size_t block_cnt = 0;
	size_t data_size = 0;
	Mono_Uint_Vec offsets;                 // (block_cnt + 1) cumulative offsets
	Min0_Uint_Vec usizes;                  // uncompressed rs-pack payload sizes
	const uint8_t* compressed_data = nullptr;
	std::vector<uint16_t> rank_to_value;
	bool is_singular = false;
};

// What a metric's pack is, in the shape Table_File's traits take: the file it
// opens and how tall its stack is. Both price every class their WDL companion
// calls decisive.
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
		m_wdl.open(ps, wdl_path);
		load_source_layered<Traits>(out_param(*this), ps, path, allow_loss_only);
	}

	// One cell's storage values as the packer emitted them, layer by layer. A
	// flip to DRAW ends the column: ILLEGAL_VAL lands at the flip layer and
	// nothing past it is. An ILLEGAL cell does not describe itself and the caller
	// overwrites: it decodes to the filler rank that extended the last run, which
	// legality and the WDL class settle.
	// `stride` steps between layers, so a caller packing columns into a
	// layer-major block writes them in place.
	void read_column(Color color, Board_Index pos, uint16_t* out, size_t stride) const;

	// Layer 0 alone. Every record shape opens with that rank, so this costs a
	// lookup rather than a column.
	NODISCARD uint16_t read_base(Color color, Board_Index pos) const;

	Fixed_Vector<Color, 2> m_colors;
	Source_Layered_Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_file;
	Source_WDL m_wdl;
	mutable Block_Cache<> m_cache[COLOR_NB];
};

extern template struct Source_Layered<DTM50_Source_Traits>;
extern template struct Source_Layered<DTC_Source_Traits>;

using Source_DTM50 = Source_Layered<DTM50_Source_Traits>;
using Source_DTC = Source_Layered<DTC_Source_Traits>;
