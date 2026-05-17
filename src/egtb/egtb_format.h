#pragma once

#include "chess/chess.h"

#include "util/defines.h"
#include "util/fixed_vector.h"

// On-disk EGTB format: file magics and wire constants shared by the write side
// (egtb_compress) and the read side (egtb_probe). Kept free of compression and
// generation machinery so the probe path can depend on the wire format without
// pulling in either. Entry value types live in egtb_entry.h, which includes
// this header.

// Magic values used for marking the EGTB files.
enum struct EGTB_Magic : uint64_t
{
	WDL_MAGIC         = 0x9bd1e3a6,
	DTC_MAGIC         = 0x2ec8b161,
	DTC_SLICE_MAGIC   = 0xd1cef11e51ce2001ULL,
	DTM_MAGIC         = 0xab57c134,
	DTM_SLICE_MAGIC   = 0xd1cef11e51ce2002ULL,
	DTM50_MAGIC       = 0xab57c150,
	DTM50_SLICE_MAGIC = 0xd1cef11e51ce2050ULL,
};

constexpr uint8_t EGTB_SINGULAR_FLAG = 0x80;
constexpr uint64_t EGTB_CHECKSUM_INIT_VALUE = 0xf0f0f0f0f0f0;

// Hmc layers per DTM50 sub-table: layer 0 is the unbounded DTM, layers 1..100
// are hmc 0..99. Both the flat sub-loader and the probe block decoder read
// these, so they live in the format header rather than the generator header.
inline constexpr size_t DTM50_HMC_COUNT = 100;
inline constexpr size_t DTM50_PACK_LAYERS = DTM50_HMC_COUNT + 1;

NODISCARD inline Fixed_Vector<Color, 2> egtb_table_colors(size_t table_num)
{
	ASSERT(table_num <= COLOR_NB);
	Fixed_Vector<Color, 2> r;
	r.emplace_back(WHITE);
	if (table_num == 2) r.emplace_back(BLACK);
	return r;
}
