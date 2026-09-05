#pragma once

#include "chess/chess.h"

#include "util/defines.h"
#include "util/fixed_vector.h"

// Magic values used for marking the EGTB files.
enum struct EGTB_Magic : uint64_t
{
	WDL_MAGIC         = 0x9bd1e3a6,
	DTZ_MAGIC         = 0x2ec8b161,
	DTZ_SLICE_MAGIC   = 0xd1cef11e51ce2001ULL,
	DTC_MAGIC         = 0x2ec8b17e,
	DTC_SLICE_MAGIC   = 0xd1cef11e51ce2003ULL,
	DTM_MAGIC         = 0xab57c134,
	DTM_SLICE_MAGIC   = 0xd1cef11e51ce2002ULL,
	DTM50_MAGIC       = 0xab57c151,
	DTM50_SLICE_MAGIC = 0xd1cef11e51ce2050ULL,
};

inline constexpr uint8_t EGTB_SINGULAR_FLAG  = 0x80;
inline constexpr uint8_t EGTB_DROPPED_FLAG   = 0x40;
inline constexpr uint8_t EGTB_LOSS_ONLY_FLAG = 0x20;
inline constexpr uint8_t EGTB_RELAXED_FLAG   = 0x10;

inline constexpr uint64_t EGTB_CHECKSUM_INIT_VALUE = 0xf0f0f0f0f0f0;

// Hmc layers per DTM50 sub-table: layer 0 is the unbounded DTM, layers 1..100
// are hmc 0..99. Both the flat sub-loader and the probe block decoder read
// these, so they live in the format header rather than the generator header.
inline constexpr size_t DTM50_HMC_COUNT = 100;
inline constexpr size_t DTM50_PACK_LAYERS = DTM50_HMC_COUNT + 1;

// An 8-man winner has at most six pawns, each with at most five non-converting
// pushes, so its budget curve has at most 30 relevant changepoints. For clean
// W/L the terminal one is exactly DTZ: solve the preceding 29 and embed that
// endpoint at row 0. Cursed/blessed cells use row 0 only as DTZ.
inline constexpr size_t DTC_BUDGET_LAYERS = 29;
inline constexpr size_t DTC_PACK_LAYERS = DTC_BUDGET_LAYERS + 1;

// Width of a MULTI record's changepoint bitmap for a stack of `layers`: whole
// 32-bit words, so both sides mask it a word at a time with no per-byte tail.
// DTC's 30 layers take 4 bytes, DTM50's 101 take 16. Lives here beside the layer
// counts because both the encoder and the readers evaluate against it.
NODISCARD constexpr size_t layered_bitmap_bytes(size_t layers)
{
	return ((layers + 31) / 32) * 4;
}

inline constexpr size_t DTM50_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTM50_PACK_LAYERS);
inline constexpr size_t DTC_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTC_PACK_LAYERS);

NODISCARD INLINE Fixed_Vector<Color, 2> egtb_table_colors(size_t table_num)
{
	ASSERT(table_num <= COLOR_NB);
	Fixed_Vector<Color, 2> r;
	r.emplace_back(WHITE);
	if (table_num == 2) r.emplace_back(BLACK);
	return r;
}
