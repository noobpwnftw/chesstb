#pragma once

#include "chess/chess.h"

#include "util/defines.h"
#include "util/fixed_vector.h"

#include <cstdint>

// Magic values used by probe-visible table files.
enum struct EGTB_Magic : uint64_t
{
	WDL_MAGIC   = 0x9bd1e3a6,
	DTC_MAGIC   = 0x2ec8b161,
	DTM_MAGIC   = 0xab57c134,
	DTM50_MAGIC = 0xab57c150,
};

// Semantic 5-class outcome. (For the cursed/blessed meaning see egtb_entry.h.)
enum struct WDL_Entry : uint8_t
{
	LOSE         = 0,
	BLESSED_LOSS = 1,
	DRAW         = 2,
	CURSED_WIN   = 3,
	WIN          = 4,
	ILLEGAL      = 7,
};

// On-disk 4-bit code: the five classes share WDL_Entry's values, plus two
// markers for a WIN/LOSE at the 50mr edge. Only the dropped-frame derive reads
// the markers; everything else turns a stored code into a class via
// wdl_from_storage(). The distinct type keeps a marker out of semantic code.
enum struct WDL_Stored : uint8_t
{
	LOSE          = 0,
	BLESSED_LOSS  = 1,
	DRAW          = 2,
	CURSED_WIN    = 3,
	WIN           = 4,
	BOUNDARY_LOSS = 5,
	BOUNDARY_WIN  = 6,
	ILLEGAL       = 7,
};

NODISCARD constexpr WDL_Entry wdl_from_storage(WDL_Stored s)
{
	if (s == WDL_Stored::BOUNDARY_WIN)  return WDL_Entry::WIN;
	if (s == WDL_Stored::BOUNDARY_LOSS) return WDL_Entry::LOSE;
	return static_cast<WDL_Entry>(s);  // five classes share WDL_Entry's values
}

enum Packed_WDL_Entries : uint8_t {};

static constexpr size_t WDL_ENTRY_PACK_RATIO = 2;
static constexpr size_t WDL_ENTRY_BITS = 4;

static constexpr uint16_t DTC_MAX_NON_CURSED_DTZ = 100;

NODISCARD inline Fixed_Vector<Color, 2> egtb_table_colors(size_t table_num)
{
	ASSERT(table_num <= COLOR_NB);
	Fixed_Vector<Color, 2> r;
	r.emplace_back(WHITE);
	if (table_num == 2) r.emplace_back(BLACK);
	return r;
}

NODISCARD constexpr WDL_Stored get_wdl_value(Packed_WDL_Entries packed, size_t pos)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	return static_cast<WDL_Stored>((packed >> (pos * WDL_ENTRY_BITS)) & 0xF);
}

NODISCARD constexpr uint16_t dtc_value_from_storage(
	uint16_t stored, WDL_Entry w, size_t entry_bytes)
{
	if (w == WDL_Entry::DRAW) return 0;
	return (entry_bytes == 1 && (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS))
		? static_cast<uint16_t>((stored << 1) - 1)
		: stored;
}

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

NODISCARD constexpr uint16_t dtm50_layered_value_from_storage(uint16_t stored, WDL_Entry w)
{
	if (stored == 0) return 0;
	return dtm50_value_from_storage(stored, w);
}
