#pragma once

#include "chess/chess.h"
#include "util/defines.h"
#include "util/enum.h"
#include "util/fixed_vector.h"
#include "util/math.h"
#include "util/span.h"

#include <variant>

// Semantic 5-class outcome. Numbered worst-to-best: the generator picks
// outcomes by value (e.g. effective_opp_wdl_after_dp), so the order is
// load-bearing. CURSED_WIN is a normal-rules win unreachable within 50mr (FIDE
// draw); BLESSED_LOSS a loss not forceable within 50mr. ILLEGAL is off-scale.
enum struct WDL_Entry : uint8_t
{
	LOSE         = 0,
	BLESSED_LOSS = 1,
	DRAW         = 2,
	CURSED_WIN   = 3,
	WIN          = 4,
	ILLEGAL      = 7,
};

// On-disk 4-bit code. The five classes share WDL_Entry's values, plus two
// markers for a WIN/LOSE whose conversion sits exactly at the 50mr edge
// (dtz == MAX_NON_CURSED_DTZ). Only the dropped-frame derive inspects the
// markers; wdl_from_storage() turns any stored code into a WDL_Entry. Keeping
// it a distinct type stops a marker from masquerading as a semantic class.
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

enum Packed_WDL_Entries : uint8_t {};

static constexpr size_t WDL_ENTRY_PACK_RATIO = 2;
static constexpr size_t WDL_ENTRY_BITS = 4;

NODISCARD constexpr Packed_WDL_Entries pack_wdl_entries(WDL_Stored v0, WDL_Stored v1)
{
	return static_cast<Packed_WDL_Entries>(
		  (static_cast<uint8_t>(v0) << 0)
		| (static_cast<uint8_t>(v1) << 4));
}

NODISCARD constexpr Packed_WDL_Entries pack_wdl_entries(const WDL_Stored v[2])
{
	return pack_wdl_entries(v[0], v[1]);
}

inline constexpr void pack_wdl_entries(Const_Span<WDL_Stored> in, Span<Packed_WDL_Entries> out)
{
	ASSERT(in.size() == out.size() * WDL_ENTRY_PACK_RATIO);
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = pack_wdl_entries(in.data() + i * WDL_ENTRY_PACK_RATIO);
}

constexpr void unpack_wdl_entries(Packed_WDL_Entries packed, WDL_Stored out[2])
{
	out[0] = static_cast<WDL_Stored>((packed >> 0) & 0xF);
	out[1] = static_cast<WDL_Stored>((packed >> 4) & 0xF);
}

inline constexpr void unpack_wdl_entries(Const_Span<Packed_WDL_Entries> in, Span<WDL_Stored> out)
{
	ASSERT(in.size() * WDL_ENTRY_PACK_RATIO == out.size());
	for (size_t i = 0; i < in.size(); ++i)
		unpack_wdl_entries(in[i], out.data() + i * WDL_ENTRY_PACK_RATIO);
}

NODISCARD constexpr WDL_Stored get_wdl_value(Packed_WDL_Entries packed, size_t pos)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	return static_cast<WDL_Stored>((packed >> (pos * WDL_ENTRY_BITS)) & 0xF);
}

static constexpr uint8_t PACKED_WDL_ENTRY_INV_MASK[2] = {
	0b11110000, 0b00001111,
};

constexpr void set_wdl_entry(Packed_WDL_Entries& packed, size_t pos, WDL_Stored v)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	packed = static_cast<Packed_WDL_Entries>(
		(packed & PACKED_WDL_ENTRY_INV_MASK[pos]) | (static_cast<uint8_t>(v) << (pos * WDL_ENTRY_BITS)));
}

enum DTC_Score : uint16_t {
	DTC_SCORE_ZERO = 0,
	DTC_SCORE_MAX = 0x07FEu,
};

ENUM_ENABLE_OPERATOR_INC(DTC_Score);
ENUM_ENABLE_OPERATOR_ADD(DTC_Score);

enum DTC_Final_Entry_Flag : uint16_t {
	DTC_FLAG_WIN  = 0x0800u,
	DTC_FLAG_LOSS = 0x1000u,
};

enum DTC_Rule_Flag : uint16_t {
	DTC_FLAG_CAP_CWIN  = 0x2000u,
	DTC_FLAG_CAP_CLOSS = 0x4000u,
};

enum DTC_Intermediate_Entry_Flag : uint16_t {
	DTC_FLAG_CHANGE = 0x8000u,
};

struct DTC_Entry_Base
{
	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;
	static constexpr uint16_t RULE_MASK   = DTC_FLAG_CAP_CWIN | DTC_FLAG_CAP_CLOSS;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTC_Rule_Flag>;

	static constexpr bool wants_zero_init_storage = false;

	// Huge_Array's For_Overwrite_Tag path uses `new T`; this ctor must zero m_data.
	constexpr DTC_Entry_Base() : m_data(0) {}
	constexpr explicit DTC_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr bool operator==(DTC_Entry_Base o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTC_Entry_Base o) const { return m_data != o.m_data; }

protected:
	uint16_t m_data;
};

struct DTC_Final_Entry;

struct DTC_Intermediate_Entry : DTC_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTC_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTC_Intermediate_Entry_Flag>;

	constexpr DTC_Intermediate_Entry() : DTC_Entry_Base{} {}

	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_cwin()  { return DTC_Intermediate_Entry{DTC_FLAG_CAP_CWIN}; }
	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_closs() { return DTC_Intermediate_Entry{DTC_FLAG_CAP_CLOSS}; }
	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_draw()  { return DTC_Intermediate_Entry{RULE_MASK}; }

	// Init-time zeroing verdict carried by the two rule bits (a 2-bit field, not
	// independent flags): CWIN/CLOSS cursed routing, or DRAW (both bits) meaning
	// "a zeroing move draws". Exact-match so DRAW is not misread as CWIN or CLOSS.
	NODISCARD constexpr bool has_cap_cwin()  const { return (m_data & RULE_MASK) == DTC_FLAG_CAP_CWIN;  }
	NODISCARD constexpr bool has_cap_closs() const { return (m_data & RULE_MASK) == DTC_FLAG_CAP_CLOSS; }
	NODISCARD constexpr bool has_cap_draw()  const { return (m_data & RULE_MASK) == RULE_MASK;          }

	// Any zeroing verdict cached (CWIN, CLOSS, or DRAW) — i.e. either rule bit set.
	NODISCARD constexpr bool has_any_hint()   const { return (m_data & RULE_MASK) != 0; }
	// A cursed cap routing hint: CWIN or CLOSS, i.e. exactly one rule bit set.
	NODISCARD constexpr bool has_cap_cursed() const
	{
		static_assert(DTC_FLAG_CAP_CWIN << 1 == DTC_FLAG_CAP_CLOSS);
		return ((m_data ^ (m_data << 1)) & DTC_FLAG_CAP_CLOSS) != 0;
	}
	NODISCARD constexpr bool has_change()    const { return (m_data & DTC_FLAG_CHANGE) != 0; }

	constexpr void set_flag(DTC_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTC_Intermediate_Entry_Flag f) { m_data &= ~f; }

private:
	constexpr explicit DTC_Intermediate_Entry(uint16_t bits) : DTC_Entry_Base{bits} {}
};
static_assert(sizeof(DTC_Intermediate_Entry) == 2);

struct DTC_Final_Entry : DTC_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTC_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTC_Final_Entry_Flag>;

	static constexpr uint16_t MAX_NON_CURSED_DTZ = 100;

	constexpr DTC_Final_Entry() : DTC_Entry_Base{} {}

	NODISCARD static constexpr DTC_Final_Entry make_illegal()           { return DTC_Final_Entry{ILLEGAL_VAL}; }
	NODISCARD static constexpr DTC_Final_Entry make_draw()              { return {}; }
	NODISCARD static constexpr DTC_Final_Entry make_win(uint16_t v)   { return DTC_Final_Entry{static_cast<uint16_t>(v | DTC_FLAG_WIN)}; }
	NODISCARD static constexpr DTC_Final_Entry make_loss(uint16_t v)  { return DTC_Final_Entry{static_cast<uint16_t>(v | DTC_FLAG_LOSS)}; }

	// Stamp the cursedness tag: CWIN on a win, CLOSS on a loss.
	constexpr void set_flag(DTC_Rule_Flag f) { m_data |= f; }

	NODISCARD constexpr DTC_Score value() const { return static_cast<DTC_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_illegal() const { return (m_data & VALUE_MASK) == ILLEGAL_VAL; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTC_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTC_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTC_FLAG_WIN | DTC_FLAG_LOSS)) == 0;
	}

	NODISCARD constexpr bool is_cursed() const { return (m_data & RULE_MASK) != 0; }

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())
		{
			const bool cursed = is_cursed() || value() > MAX_NON_CURSED_DTZ;
			return cursed ? WDL_Entry::CURSED_WIN : WDL_Entry::WIN;
		}
		if (is_loss())
		{
			const bool cursed = is_cursed() || value() > MAX_NON_CURSED_DTZ;
			return cursed ? WDL_Entry::BLESSED_LOSS : WDL_Entry::LOSE;
		}
		return WDL_Entry::DRAW;
	}

private:
	constexpr explicit DTC_Final_Entry(uint16_t bits) : DTC_Entry_Base{bits} {}
};
static_assert(sizeof(DTC_Final_Entry) == 2);

// Encode the class to its on-disk code, tagging a WIN/LOSE at the 50mr edge so
// a dropped frame's 1-ply derive can tip its quiet predecessor into CURSED_WIN /
// BLESSED_LOSS. wdl_from_storage() folds the tag back everywhere else.
NODISCARD constexpr WDL_Stored wdl_for_storage(DTC_Final_Entry e)
{
	const WDL_Entry w = e.wdl();
	if (static_cast<uint16_t>(e.value()) == DTC_Final_Entry::MAX_NON_CURSED_DTZ)
	{
		if (w == WDL_Entry::WIN)  return WDL_Stored::BOUNDARY_WIN;
		if (w == WDL_Entry::LOSE) return WDL_Stored::BOUNDARY_LOSS;
	}
	return static_cast<WDL_Stored>(w);  // five classes share WDL_Entry's values
}

// Inverse of wdl_for_storage: decode a stored code to its semantic class
// (markers fold to WIN/LOSE).
NODISCARD constexpr WDL_Entry wdl_from_storage(WDL_Stored s)
{
	if (s == WDL_Stored::BOUNDARY_WIN)  return WDL_Entry::WIN;
	if (s == WDL_Stored::BOUNDARY_LOSS) return WDL_Entry::LOSE;
	return static_cast<WDL_Entry>(s);  // five classes share WDL_Entry's values
}

// 1-byte tier halves cursed values;
// 2-byte tier writes raw values.
NODISCARD constexpr uint16_t dtc_value_for_storage(DTC_Final_Entry e)
{
	const uint16_t v = static_cast<uint16_t>(e.value());
	const WDL_Entry w = e.wdl();
	return (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS)
		? static_cast<uint16_t>((v + 1) >> 1) : v;
}

// Pairs with the round-up encode: odd values recover exactly, even decode as v-1.
//
// DRAW is WDL-companion authoritative across DTC/DTM/DTM50: the encoder
// run-stitches DRAW cells with W/L neighbors for compression, so their stored
// bits carry neighbor payloads. Synthesize DRAW from `w`, never from `stored`.
NODISCARD constexpr DTC_Final_Entry dtc_entry_from_storage(DTC_Final_Entry stored, WDL_Entry w, size_t entry_bytes)
{
	if (w == WDL_Entry::ILLEGAL) return DTC_Final_Entry::make_illegal();
	if (w == WDL_Entry::DRAW)    return DTC_Final_Entry::make_draw();

	const bool cursed = (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS);
	const bool win    = (w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN);

	uint16_t v = static_cast<uint16_t>(stored.value());
	if (entry_bytes == 1 && cursed)
		v = static_cast<uint16_t>((v << 1) - 1);

	DTC_Final_Entry e = win ? DTC_Final_Entry::make_win(v) : DTC_Final_Entry::make_loss(v);
	if (cursed) e.set_flag(win ? DTC_FLAG_CAP_CWIN : DTC_FLAG_CAP_CLOSS);
	return e;
}

using DTC_Any_Entry = std::variant<DTC_Intermediate_Entry, DTC_Final_Entry>;

enum DTM_Score : uint16_t {
	DTM_SCORE_ZERO = 0,
	DTM_SCORE_MAX = 0x07FEu,  // 2046 plies; ILLEGAL_VAL sits at 0x07FF.
};

ENUM_ENABLE_OPERATOR_INC(DTM_Score);
ENUM_ENABLE_OPERATOR_ADD(DTM_Score);

enum DTM_Final_Entry_Flag : uint16_t {
	DTM_FLAG_WIN  = 0x0800u,
	DTM_FLAG_LOSS = 0x1000u,
};

enum DTM_Intermediate_Entry_Flag : uint16_t {
	DTM_FLAG_PAWN_EVAL = 0x2000u,
	DTM_FLAG_CHANGE    = 0x4000u,
};

struct DTM_Entry_Base
{
	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = false;

	static constexpr bool wants_zero_init_storage = false;

	// Huge_Array's For_Overwrite_Tag path uses `new T`; this ctor must zero m_data.
	constexpr DTM_Entry_Base() : m_data(0) {}
	constexpr explicit DTM_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr DTM_Score value() const { return static_cast<DTM_Score>(m_data & VALUE_MASK); }


	NODISCARD constexpr bool operator==(DTM_Entry_Base o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTM_Entry_Base o) const { return m_data != o.m_data; }

protected:
	uint16_t m_data;
};

struct DTM_Intermediate_Entry : DTM_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTM_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTM_Intermediate_Entry_Flag>;

	constexpr DTM_Intermediate_Entry() : DTM_Entry_Base{} {}

	NODISCARD static constexpr DTM_Intermediate_Entry make_pawn_eval(uint16_t min_ply)
	{
		ASSERT((min_ply & VALUE_MASK) == min_ply);
		return DTM_Intermediate_Entry{static_cast<uint16_t>(min_ply | DTM_FLAG_PAWN_EVAL)};
	}

	NODISCARD constexpr bool has_change()    const { return (m_data & DTM_FLAG_CHANGE)    != 0; }
	NODISCARD constexpr bool has_pawn_eval() const { return (m_data & DTM_FLAG_PAWN_EVAL) != 0; }
	NODISCARD constexpr bool has_any_flags() const { return (m_data & ~VALUE_MASK)        != 0; }

	NODISCARD constexpr bool has_flag(DTM_Intermediate_Entry_Flag f) const { return (m_data & f) != 0; }

	constexpr void set_flag(DTM_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTM_Intermediate_Entry_Flag f) { m_data &= ~f; }

private:
	constexpr explicit DTM_Intermediate_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
};
static_assert(sizeof(DTM_Intermediate_Entry) == 2);

struct DTM_Final_Entry : DTM_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTM_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTM_Final_Entry_Flag>;

	constexpr DTM_Final_Entry() : DTM_Entry_Base{} {}

	NODISCARD static constexpr DTM_Final_Entry make_illegal()         { return DTM_Final_Entry{ILLEGAL_VAL}; }
	NODISCARD static constexpr DTM_Final_Entry make_draw()            { return {}; }
	NODISCARD static constexpr DTM_Final_Entry make_win(uint16_t v)   { return DTM_Final_Entry{static_cast<uint16_t>(v | DTM_FLAG_WIN)}; }
	NODISCARD static constexpr DTM_Final_Entry make_loss(uint16_t v)  { return DTM_Final_Entry{static_cast<uint16_t>(v | DTM_FLAG_LOSS)}; }

	NODISCARD constexpr bool is_illegal() const { return (m_data & VALUE_MASK) == ILLEGAL_VAL; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTM_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTM_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTM_FLAG_WIN | DTM_FLAG_LOSS)) == 0;
	}

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())     return WDL_Entry::WIN;
		if (is_loss())    return WDL_Entry::LOSE;
		return WDL_Entry::DRAW;
	}

private:
	constexpr explicit DTM_Final_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
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
		case WDL_Entry::CURSED_WIN:   return DTM_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE:
		case WDL_Entry::BLESSED_LOSS: return DTM_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::DRAW:
		default:                      return DTM_Final_Entry::make_draw();
	}
}

using DTM_Any_Entry = std::variant<DTM_Intermediate_Entry, DTM_Final_Entry>;

struct DTM50_Intermediate_Entry : DTM_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTM_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTM_Final_Entry_Flag>;

	static constexpr bool wants_zero_init_storage = false;

	constexpr DTM50_Intermediate_Entry() : DTM_Entry_Base{} {}

	NODISCARD static constexpr DTM50_Intermediate_Entry make_draw()           { return {}; }
	NODISCARD static constexpr DTM50_Intermediate_Entry make_win(uint16_t v)  { return DTM50_Intermediate_Entry{static_cast<uint16_t>(v | DTM_FLAG_WIN)}; }
	NODISCARD static constexpr DTM50_Intermediate_Entry make_loss(uint16_t v) { return DTM50_Intermediate_Entry{static_cast<uint16_t>(v | DTM_FLAG_LOSS)}; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTM_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTM_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTM_FLAG_WIN | DTM_FLAG_LOSS)) == 0;
	}

private:
	constexpr explicit DTM50_Intermediate_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
};
static_assert(sizeof(DTM50_Intermediate_Entry) == 2);

struct DTM50_Final_Entry : DTM_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTM_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTM_Final_Entry_Flag>;

	static constexpr bool wants_zero_init_storage = true;

	constexpr DTM50_Final_Entry() : DTM_Entry_Base{} {}

	NODISCARD static constexpr DTM50_Final_Entry make_illegal()         { return DTM50_Final_Entry{ILLEGAL_VAL}; }
	NODISCARD static constexpr DTM50_Final_Entry make_draw()            { return {}; }
	NODISCARD static constexpr DTM50_Final_Entry make_win(uint16_t v)   { return DTM50_Final_Entry{static_cast<uint16_t>(v | DTM_FLAG_WIN)}; }
	NODISCARD static constexpr DTM50_Final_Entry make_loss(uint16_t v)  { return DTM50_Final_Entry{static_cast<uint16_t>(v | DTM_FLAG_LOSS)}; }

	NODISCARD constexpr bool is_illegal() const { return (m_data & VALUE_MASK) == ILLEGAL_VAL; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTM_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTM_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTM_FLAG_WIN | DTM_FLAG_LOSS)) == 0;
	}

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())     return WDL_Entry::WIN;
		if (is_loss())    return WDL_Entry::LOSE;
		return WDL_Entry::DRAW;
	}

private:
	constexpr explicit DTM50_Final_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
};
static_assert(sizeof(DTM50_Final_Entry) == 2);

NODISCARD constexpr uint16_t dtm50_value_for_storage(DTM50_Final_Entry e)
{
	if (e.is_illegal()) return DTM50_Final_Entry::ILLEGAL_VAL;
	return static_cast<uint16_t>(e.value()) >> 1;
}

// DTM50 layer-0: cursed/blessed → DRAW. Unambiguous at hmc=0 (no cell is both
// strict-WIN in flat-DTM and DRAW in DTM50 at the reset window).
NODISCARD constexpr DTM50_Final_Entry dtm50_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::ILLEGAL: return DTM50_Final_Entry::make_illegal();
		case WDL_Entry::WIN:     return DTM50_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE:    return DTM50_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::CURSED_WIN:
		case WDL_Entry::BLESSED_LOSS:
		case WDL_Entry::DRAW:
		default:                 return DTM50_Final_Entry::make_draw();
	}
}

// DTM50 layer-k>0: WDL=WIN at storage=0 is ambiguous (WIN(1) vs cursed-route DRAW);
// resolve to DRAW, prober recovers WIN(1)/LOSS(0) locally via move-gen. storage>0
// disambiguates by class.
NODISCARD constexpr DTM50_Final_Entry dtm50_layered_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	if (w == WDL_Entry::ILLEGAL) return DTM50_Final_Entry::make_illegal();
	if (stored == 0)             return DTM50_Final_Entry::make_draw();
	switch (w)
	{
		case WDL_Entry::WIN:  return DTM50_Final_Entry::make_win(static_cast<uint16_t>((stored << 1) | 1u));
		case WDL_Entry::LOSE: return DTM50_Final_Entry::make_loss(static_cast<uint16_t>(stored << 1));
		case WDL_Entry::CURSED_WIN:
		case WDL_Entry::BLESSED_LOSS:
		case WDL_Entry::DRAW:
		default:              return DTM50_Final_Entry::make_draw();
	}
}
