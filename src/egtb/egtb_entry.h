#pragma once

#include "chess/chess.h"
#include "util/defines.h"
#include "util/enum.h"
#include "util/fixed_vector.h"
#include "util/math.h"
#include "util/span.h"

#include <cstdint>
#include <type_traits>
#include <variant>

// Magic values used for marking the EGTB files.
enum struct EGTB_Magic : uint64_t
{
	WDL_MAGIC       = 0x7550918f,
	DTC_MAGIC       = 0xb19122de,
	DTC_SLICE_MAGIC = 0xd1cef11e51ce0001ULL,
	DTM_MAGIC       = 0xabc98e32,
	DTM_SLICE_MAGIC = 0xd1cef11e51ce0002ULL,
	DTM50_MAGIC             = 0xabc98e50,
	DTM50_SLICE_MAGIC       = 0xd1cef11e51ce0050ULL,
	DTM50_PHASE_SLICE_MAGIC = 0xd1cef11e51ce0051ULL,
};

// 4 bits per entry. Syzygy-style ordering: higher = better for mover (`w > best` maxes).
// ILLEGAL = 7 sits off-scale so it can't masquerade as a real outcome.
//   CURSED_WIN:   normal-rules win, not reachable within 50mr (FIDE: draw).
//   BLESSED_LOSS: loss not forceable within 50mr.
enum struct WDL_Entry : uint8_t
{
	LOSE         = 0,
	BLESSED_LOSS = 1,
	DRAW         = 2,
	CURSED_WIN   = 3,
	WIN          = 4,
	ILLEGAL      = 7,
};

enum Packed_WDL_Entries : uint8_t {};

static constexpr size_t WDL_ENTRY_PACK_RATIO = 2;
static constexpr size_t WDL_ENTRY_BITS = 4;

NODISCARD constexpr Packed_WDL_Entries pack_wdl_entries(WDL_Entry v0, WDL_Entry v1)
{
	return static_cast<Packed_WDL_Entries>(
		  (static_cast<uint8_t>(v0) << 0)
		| (static_cast<uint8_t>(v1) << 4));
}

NODISCARD constexpr Packed_WDL_Entries pack_wdl_entries(const WDL_Entry v[2])
{
	return pack_wdl_entries(v[0], v[1]);
}

inline constexpr void pack_wdl_entries(Const_Span<WDL_Entry> in, Span<Packed_WDL_Entries> out)
{
	ASSERT(in.size() == out.size() * WDL_ENTRY_PACK_RATIO);
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = pack_wdl_entries(in.data() + i * WDL_ENTRY_PACK_RATIO);
}

constexpr void unpack_wdl_entries(Packed_WDL_Entries packed, WDL_Entry out[2])
{
	out[0] = static_cast<WDL_Entry>((packed >> 0) & 0xF);
	out[1] = static_cast<WDL_Entry>((packed >> 4) & 0xF);
}

inline constexpr void unpack_wdl_entries(Const_Span<Packed_WDL_Entries> in, Span<WDL_Entry> out)
{
	ASSERT(in.size() * WDL_ENTRY_PACK_RATIO == out.size());
	for (size_t i = 0; i < in.size(); ++i)
		unpack_wdl_entries(in[i], out.data() + i * WDL_ENTRY_PACK_RATIO);
}

NODISCARD inline Fixed_Vector<Color, 2> egtb_table_colors(size_t table_num)
{
	ASSERT(table_num <= COLOR_NB);
	Fixed_Vector<Color, 2> r;
	r.emplace_back(WHITE);
	if (table_num == 2) r.emplace_back(BLACK);
	return r;
}

NODISCARD constexpr WDL_Entry get_wdl_value(Packed_WDL_Entries packed, size_t pos)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	return static_cast<WDL_Entry>((packed >> (pos * WDL_ENTRY_BITS)) & 0xF);
}

static constexpr uint8_t PACKED_WDL_ENTRY_INV_MASK[2] = {
	0b11110000, 0b00001111,
};

constexpr void set_wdl_entry(Packed_WDL_Entries& packed, size_t pos, WDL_Entry v)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	packed = static_cast<Packed_WDL_Entries>(
		(packed & PACKED_WDL_ENTRY_INV_MASK[pos]) | (static_cast<uint8_t>(v) << (pos * WDL_ENTRY_BITS)));
}

// DTC value = plies to next zeroing; class lives in flag bits.

enum DTC_Score : uint16_t {
	DTC_SCORE_ZERO = 0,
	DTC_SCORE_MAX = 0x07FEu,
};

ENUM_ENABLE_OPERATOR_INC(DTC_Score);
ENUM_ENABLE_OPERATOR_ADD(DTC_Score);

// Final and Intermediate share 16-bit storage.
//   WIN/LOSS:        classified; value = DTZ.
//   CAP_CWIN/CLOSS:  cursed-winning / cursed-losing zeroing class hint.
//   CHANGE:          reverify-pending on Intermediates. Atomic OR via lock_add_flags;
//                    cleared when write_dtc overwrites with a Final.
enum DTC_Intermediate_Entry_Flag : uint16_t {
	DTC_FLAG_WIN       = 0x0800u,
	DTC_FLAG_LOSS      = 0x1000u,
	DTC_FLAG_CAP_CWIN  = 0x2000u,
	DTC_FLAG_CAP_CLOSS = 0x4000u,
	DTC_FLAG_CHANGE    = 0x8000u,
};

struct DTC_Intermediate_Entry;

struct DTC_Final_Entry
{
	friend struct DTC_Intermediate_Entry;

	static constexpr uint16_t VALUE_MASK = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;  // = 0x07FF in the score field

	// lock_add_flags accepts Intermediate flags on Final-typed storage (CHANGE OR during iterate).
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTC_Intermediate_Entry_Flag>;

	// Huge_Array's For_Overwrite_Tag path uses `new T`; this ctor must zero m_data.
	constexpr DTC_Final_Entry() : m_data(0) {}

	NODISCARD static constexpr DTC_Final_Entry make_illegal()
	{
		DTC_Final_Entry e; e.m_data = ILLEGAL_VAL; return e;
	}

	NODISCARD static constexpr DTC_Final_Entry make_draw()
	{
		return {};
	}

	NODISCARD static constexpr DTC_Final_Entry make_score(DTC_Score v)
	{
		ASSERT(static_cast<uint16_t>(v) <= DTC_SCORE_MAX);
		DTC_Final_Entry e;
		e.m_data = static_cast<uint16_t>(v);
		return e;
	}

	NODISCARD static constexpr DTC_Final_Entry make_win(uint16_t dtz)
	{
		DTC_Final_Entry e;
		e.m_data = static_cast<uint16_t>(dtz) | DTC_FLAG_WIN;
		return e;
	}

	NODISCARD static constexpr DTC_Final_Entry make_loss(uint16_t dtz)
	{
		DTC_Final_Entry e;
		e.m_data = static_cast<uint16_t>(dtz) | DTC_FLAG_LOSS;
		return e;
	}

	static constexpr uint16_t MAX_NON_CURSED_DTZ = 100;

	NODISCARD constexpr DTC_Final_Entry with_win() const
	{
		DTC_Final_Entry r = *this; r.m_data |= DTC_FLAG_WIN; return r;
	}
	NODISCARD constexpr DTC_Final_Entry with_loss() const
	{
		DTC_Final_Entry r = *this; r.m_data |= DTC_FLAG_LOSS; return r;
	}
	NODISCARD constexpr DTC_Final_Entry with_cap_cwin() const
	{
		DTC_Final_Entry r = *this; r.m_data |= DTC_FLAG_CAP_CWIN; return r;
	}
	NODISCARD constexpr DTC_Final_Entry with_cap_closs() const
	{
		DTC_Final_Entry r = *this; r.m_data |= DTC_FLAG_CAP_CLOSS; return r;
	}
	NODISCARD constexpr DTC_Final_Entry without_change() const
	{
		DTC_Final_Entry r = *this; r.m_data &= ~DTC_FLAG_CHANGE; return r;
	}

	NODISCARD constexpr DTC_Score value() const { return static_cast<DTC_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_illegal() const { return value() == static_cast<DTC_Score>(ILLEGAL_VAL); }
	NODISCARD constexpr bool is_legal()   const { return !is_illegal(); }
	NODISCARD constexpr bool is_win()     const { return (m_data & DTC_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss()    const { return (m_data & DTC_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw()    const { return is_legal() && !is_win() && !is_loss() && value() == DTC_SCORE_ZERO; }

	NODISCARD constexpr bool has_cap_cwin()  const { return (m_data & DTC_FLAG_CAP_CWIN)  != 0; }
	NODISCARD constexpr bool has_cap_closs() const { return (m_data & DTC_FLAG_CAP_CLOSS) != 0; }
	NODISCARD constexpr bool has_change()    const { return (m_data & DTC_FLAG_CHANGE)    != 0; }

	NODISCARD constexpr bool has_any_flags() const { return (m_data & ~VALUE_MASK) != 0; }

	NODISCARD constexpr DTC_Final_Entry score_only() const
	{
		DTC_Final_Entry r;
		r.m_data = m_data & VALUE_MASK;
		return r;
	}

	NODISCARD constexpr WDL_Entry wdl() const
	{
		// CAP_* only projects cursed on Final WIN/LOSS; Intermediates with CAP_* are DRAW.
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())
		{
			const bool cursed = has_cap_cwin()
			                  || value() > MAX_NON_CURSED_DTZ;
			return cursed ? WDL_Entry::CURSED_WIN : WDL_Entry::WIN;
		}
		if (is_loss())
		{
			const bool cursed = has_cap_closs()
			                  || value() > MAX_NON_CURSED_DTZ;
			return cursed ? WDL_Entry::BLESSED_LOSS : WDL_Entry::LOSE;
		}
		return WDL_Entry::DRAW;  // Final DRAW or Intermediate
	}

	NODISCARD constexpr bool operator==(DTC_Final_Entry o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTC_Final_Entry o) const { return m_data != o.m_data; }

private:
	uint16_t m_data;
};
static_assert(sizeof(DTC_Final_Entry) == 2);

// 1-byte tier halves cursed values (round up so decode stays > MAX_NON_CURSED_DTZ);
// 2-byte tier writes raw values.
NODISCARD constexpr uint16_t dtc_value_for_storage(DTC_Final_Entry e)
{
	const uint16_t v = static_cast<uint16_t>(e.value());
	const WDL_Entry w = e.wdl();
	return (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS)
		? static_cast<uint16_t>((v + 1) >> 1) : v;
}

// Pairs with the round-up encode: v=101,103,... recover exactly; v=102,104,... as v-1.
//
// DRAW is WDL-companion authoritative across DTC/DTM/DTM50: the encoder
// run-stitches DRAW cells with W/L neighbors for compression, so their stored
// bits carry neighbor payloads. Synthesize DRAW from `w`, never from `stored`.
NODISCARD constexpr DTC_Final_Entry dtc_entry_from_storage(DTC_Final_Entry stored, WDL_Entry w, size_t entry_bytes)
{
	if (w == WDL_Entry::DRAW) return DTC_Final_Entry::make_draw();
	return (entry_bytes == 1 && (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS))
		? DTC_Final_Entry::make_score(static_cast<DTC_Score>((static_cast<uint16_t>(stored.value()) << 1) - 1))
		: stored;
}

// Distinct type, same 16-bit layout; conversions bit-identical.
struct DTC_Intermediate_Entry
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTC_Intermediate_Entry_Flag>;

	constexpr DTC_Intermediate_Entry() : m_data(0) {}

	constexpr explicit DTC_Intermediate_Entry(DTC_Final_Entry f) : m_data(f.m_data) {}

	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_cwin()
	{
		DTC_Intermediate_Entry e;
		e.m_data = DTC_FLAG_CAP_CWIN;
		return e;
	}

	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_closs()
	{
		DTC_Intermediate_Entry e;
		e.m_data = DTC_FLAG_CAP_CLOSS;
		return e;
	}

	NODISCARD constexpr bool has_flag(DTC_Intermediate_Entry_Flag f) const
	{
		return (m_data & f) != 0;
	}


	NODISCARD constexpr operator DTC_Final_Entry() const
	{
		DTC_Final_Entry f;
		f.m_data = m_data;
		return f;
	}

private:
	uint16_t m_data;
};
static_assert(sizeof(DTC_Intermediate_Entry) == 2);

using DTC_Any_Entry = std::variant<DTC_Intermediate_Entry, DTC_Final_Entry>;

// =============================================================================
// DTM (no 50MR). Flat ply count to mate; no zeroing, no cursed/blessed.
// On disk: class stripped, recovered from companion .lzw (same split as DTC).
// Parity invariant (WIN odd, LOSS even) lets both rank tiers halve storage.
// =============================================================================

enum DTM_Score : uint16_t {
	DTM_SCORE_ZERO = 0,
	DTM_SCORE_MAX = 0x07FEu,  // 2046 plies; ILLEGAL_VAL sits at 0x07FF.
};

ENUM_ENABLE_OPERATOR_INC(DTM_Score);
ENUM_ENABLE_OPERATOR_ADD(DTM_Score);

enum DTM_Entry_Flag : uint16_t {
	DTM_FLAG_WIN       = 0x0800u,
	DTM_FLAG_LOSS      = 0x1000u,
	DTM_FLAG_PAWN_EVAL = 0x2000u,
	DTM_FLAG_CHANGE    = 0x4000u,
};

struct DTM_Intermediate_Entry;

struct DTM_Final_Entry
{
	friend struct DTM_Intermediate_Entry;

	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTM_Entry_Flag>;

	// Huge_Array For_Overwrite_Tag uses `new T`; this ctor must zero (0 == DRAW).
	constexpr DTM_Final_Entry() : m_data(0) {}

	NODISCARD static constexpr DTM_Final_Entry make_draw() { return {}; }

	NODISCARD static constexpr DTM_Final_Entry make_illegal()
	{
		DTM_Final_Entry e; e.m_data = ILLEGAL_VAL; return e;
	}

	NODISCARD static constexpr DTM_Final_Entry make_win(uint16_t v)
	{
		DTM_Final_Entry e;
		e.m_data = static_cast<uint16_t>(v) | DTM_FLAG_WIN;
		return e;
	}

	NODISCARD static constexpr DTM_Final_Entry make_loss(uint16_t v)
	{
		DTM_Final_Entry e;
		e.m_data = static_cast<uint16_t>(v) | DTM_FLAG_LOSS;
		return e;
	}

	NODISCARD constexpr DTM_Final_Entry without_change() const
	{
		DTM_Final_Entry r = *this; r.m_data &= ~DTM_FLAG_CHANGE; return r;
	}

	NODISCARD constexpr DTM_Score value() const { return static_cast<DTM_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_illegal() const { return value() == static_cast<DTM_Score>(ILLEGAL_VAL); }
	NODISCARD constexpr bool is_legal()   const { return !is_illegal(); }
	NODISCARD constexpr bool is_win()     const { return (m_data & DTM_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss()    const { return (m_data & DTM_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw()    const { return is_legal() && !is_win() && !is_loss() && value() == DTM_SCORE_ZERO; }
	NODISCARD constexpr bool has_change()       const { return (m_data & DTM_FLAG_CHANGE)       != 0; }
	NODISCARD constexpr bool has_pawn_eval()    const { return (m_data & DTM_FLAG_PAWN_EVAL)    != 0; }

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())     return WDL_Entry::WIN;
		if (is_loss())    return WDL_Entry::LOSE;
		return WDL_Entry::DRAW;
	}

	NODISCARD constexpr bool operator==(DTM_Final_Entry o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTM_Final_Entry o) const { return m_data != o.m_data; }

private:
	uint16_t m_data;
};
static_assert(sizeof(DTM_Final_Entry) == 2);

// Both tiers store v/2 (parity invariant: WIN odd, LOSS even → lossless when
// class is known via the .lzw companion).
//
// ILLEGAL stays un-halved at ILLEGAL_VAL: halving would collide with
// LOSS(2046)/2 = 1023, breaking the run-stitch sentinel in LZMA_Rank_Compress_Helper.
NODISCARD constexpr uint16_t dtm_value_for_storage(DTM_Final_Entry e)
{
	if (e.is_illegal()) return DTM_Final_Entry::ILLEGAL_VAL;
	return static_cast<uint16_t>(e.value()) >> 1;
}

// DTM has no cursed classes; CURSED_WIN→WIN, BLESSED_LOSS→LOSE.
// See dtc_entry_from_storage above for the DRAW-is-companion-authoritative rule.
NODISCARD constexpr DTM_Final_Entry dtm_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::ILLEGAL:      return DTM_Final_Entry::make_illegal();
		case WDL_Entry::WIN:
		case WDL_Entry::CURSED_WIN:
			return DTM_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE:
		case WDL_Entry::BLESSED_LOSS:
			return DTM_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::DRAW:
		default:                      return DTM_Final_Entry::make_draw();
	}
}

// DTM50 layer-0: cursed/blessed → DRAW. Unambiguous at hmc=0 (no cell is both
// strict-WIN in flat-DTM and DRAW in DTM50 at the reset window).
NODISCARD constexpr DTM_Final_Entry dtm50_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::ILLEGAL: return DTM_Final_Entry::make_illegal();
		case WDL_Entry::WIN:     return DTM_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE:    return DTM_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::CURSED_WIN:
		case WDL_Entry::BLESSED_LOSS:
		case WDL_Entry::DRAW:
		default:                 return DTM_Final_Entry::make_draw();
	}
}

// DTM50 layer-k>0: WDL=WIN at storage=0 is ambiguous (WIN(1) vs cursed-route DRAW);
// resolve to DRAW, prober recovers WIN(1)/LOSS(0) locally via move-gen. storage>0
// disambiguates by class.
NODISCARD constexpr DTM_Final_Entry dtm50_layered_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	if (w == WDL_Entry::ILLEGAL) return DTM_Final_Entry::make_illegal();
	if (stored == 0)             return DTM_Final_Entry::make_draw();
	switch (w)
	{
		case WDL_Entry::WIN:  return DTM_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE: return DTM_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::CURSED_WIN:
		case WDL_Entry::BLESSED_LOSS:
		case WDL_Entry::DRAW:
		default:              return DTM_Final_Entry::make_draw();
	}
}

// Distinct type, same layout — lets init_entries std::visit Final vs Intermediate.
struct DTM_Intermediate_Entry
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTM_Entry_Flag>;

	constexpr DTM_Intermediate_Entry() : m_data(0) {}

	constexpr explicit DTM_Intermediate_Entry(DTM_Final_Entry f) : m_data(f.m_data) {}

	NODISCARD static constexpr DTM_Intermediate_Entry make_pawn_eval()
	{
		DTM_Intermediate_Entry e;
		e.m_data = DTM_FLAG_PAWN_EVAL;
		return e;
	}

	NODISCARD constexpr operator DTM_Final_Entry() const
	{
		DTM_Final_Entry f;
		f.m_data = m_data;
		return f;
	}

private:
	uint16_t m_data;
};
static_assert(sizeof(DTM_Intermediate_Entry) == 2);

using DTM_Any_Entry = std::variant<DTM_Intermediate_Entry, DTM_Final_Entry>;
