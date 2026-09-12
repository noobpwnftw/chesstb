#pragma once

#include "chess/chess.h"

#include "util/defines.h"
#include "util/fixed_vector.h"

#include <cstdint>

// Magic values used by probe-visible table files.
enum struct EGTB_Magic : uint64_t
{
	WDL_MAGIC   = 0x9bd1e3a6,
	DTZ_MAGIC   = 0x2ec8b161,
	DTC_MAGIC   = 0x2ec8b17e,
	DTM_MAGIC   = 0xab57c134,
	DTM50_MAGIC = 0xab57c151,
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
	return static_cast<WDL_Entry>(s);
}

enum Packed_WDL_Entries : uint8_t {};

inline constexpr size_t WDL_ENTRY_PACK_RATIO = 2;
inline constexpr size_t WDL_ENTRY_BITS = 4;

inline constexpr uint16_t DTZ_MAX_NON_CURSED = 100;
// No 50-move clock: the probe ignores the 50-move rule.
inline constexpr unsigned IGNORE_50MR = ~0u;

NODISCARD INLINE Fixed_Vector<Color, 2> egtb_table_colors(size_t table_num)
{
	ASSERT(table_num <= COLOR_NB);
	Fixed_Vector<Color, 2> r;
	r.emplace_back(WHITE);
	if (table_num == 2) r.emplace_back(BLACK);
	return r;
}

