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
	WDL_MAGIC         = 0x9bd1e3a4,
	DTC_MAGIC         = 0x2ec8b15f,
	DTC_SLICE_MAGIC   = 0xd1cef11e51ce2001ULL,
	DTM_MAGIC         = 0xab57c132,
	DTM_SLICE_MAGIC   = 0xd1cef11e51ce2002ULL,
	DTM50_MAGIC       = 0xab57c150,
	DTM50_SLICE_MAGIC = 0xd1cef11e51ce2050ULL,
};

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

// Decode a stored code to its semantic class (markers fold to WIN/LOSE).
NODISCARD constexpr WDL_Entry wdl_from_storage(WDL_Stored s)
{
	if (s == WDL_Stored::BOUNDARY_WIN)  return WDL_Entry::WIN;
	if (s == WDL_Stored::BOUNDARY_LOSS) return WDL_Entry::LOSE;
	return static_cast<WDL_Entry>(s);  // five classes share WDL_Entry's values
}

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

static constexpr uint8_t PACKED_WDL_ENTRY_INV_MASK[2] = {
	0b11110000, 0b00001111,
};

constexpr void set_wdl_entry(Packed_WDL_Entries& packed, size_t pos, WDL_Stored v)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	packed = static_cast<Packed_WDL_Entries>(
		(packed & PACKED_WDL_ENTRY_INV_MASK[pos]) | (static_cast<uint8_t>(v) << (pos * WDL_ENTRY_BITS)));
}

// =============================================================================
// DTC (50MR-aware). value = plies to next zeroing; class lives in flag bits.
// Final and Intermediate share 16-bit storage via DTC_Entry_Base. CAP_* bits
// sit on the base: on Final WIN/LOSS they mark cursed-class, on Intermediate
// draws they're cursed-route routing hints. CHANGE (Intermediate only) marks
// a cell as reverify-pending; set via atomic OR (lock_add_flags) so a racing
// retro_mark_wins Final write keeps its classification bits and the CHANGE
// bit lands on top harmlessly (Final dispatch ignores it). Doing this
// otherwise likely costs more than the atomics.
// =============================================================================

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

	// Huge_Array's For_Overwrite_Tag path uses `new T`; this ctor must zero m_data.
	constexpr DTC_Entry_Base() : m_data(0) {}
	constexpr explicit DTC_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr bool is_legal()   const { return (m_data & VALUE_MASK) != ILLEGAL_VAL; }
	NODISCARD constexpr bool is_illegal() const { return !is_legal(); }

	NODISCARD constexpr bool has_cap_cwin()  const { return (m_data & DTC_FLAG_CAP_CWIN)  != 0; }
	NODISCARD constexpr bool has_cap_closs() const { return (m_data & DTC_FLAG_CAP_CLOSS) != 0; }
	NODISCARD constexpr bool has_flag(DTC_Rule_Flag f) const { return (m_data & f) != 0; }

	constexpr void set_flag(DTC_Rule_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTC_Rule_Flag f) { m_data &= ~f; }

	NODISCARD constexpr bool operator==(DTC_Entry_Base o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTC_Entry_Base o) const { return m_data != o.m_data; }

protected:
	uint16_t m_data;
};

struct DTC_Final_Entry;

struct DTC_Intermediate_Entry : DTC_Entry_Base
{
	friend struct DTC_Final_Entry;  // for DTC_Final_Entry::copy_rule

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTC_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTC_Intermediate_Entry_Flag>;

	constexpr DTC_Intermediate_Entry() : DTC_Entry_Base{} {}

	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_cwin()  { return DTC_Intermediate_Entry{DTC_FLAG_CAP_CWIN}; }
	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_closs() { return DTC_Intermediate_Entry{DTC_FLAG_CAP_CLOSS}; }

	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (VALUE_MASK | DTC_FLAG_WIN | DTC_FLAG_LOSS)) == 0;
	}
	NODISCARD constexpr bool has_change()    const { return (m_data & DTC_FLAG_CHANGE) != 0; }
	NODISCARD constexpr bool has_any_flags() const { return (m_data & ~VALUE_MASK)     != 0; }

	NODISCARD constexpr bool has_flag(DTC_Intermediate_Entry_Flag f) const { return (m_data & f) != 0; }
	using DTC_Entry_Base::has_flag;

	constexpr void set_flag(DTC_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTC_Intermediate_Entry_Flag f) { m_data &= ~f; }
	using DTC_Entry_Base::set_flag;
	using DTC_Entry_Base::clear_flag;

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
	NODISCARD static constexpr DTC_Final_Entry make_score(DTC_Score v)
	{
		ASSERT(static_cast<uint16_t>(v) <= DTC_SCORE_MAX);
		return DTC_Final_Entry{static_cast<uint16_t>(v)};
	}
	NODISCARD static constexpr DTC_Final_Entry make_win(uint16_t dtz)   { return DTC_Final_Entry{static_cast<uint16_t>(dtz | DTC_FLAG_WIN)}; }
	NODISCARD static constexpr DTC_Final_Entry make_loss(uint16_t dtz)  { return DTC_Final_Entry{static_cast<uint16_t>(dtz | DTC_FLAG_LOSS)}; }

	// Construct from raw storage bits (for direct slice scans).
	NODISCARD static constexpr DTC_Final_Entry from_storage(uint16_t bits) { return DTC_Final_Entry{bits}; }

	// Project rule bits (CAP_*) from an Intermediate into a fresh Final, so
	// subsequent set_score_* preserves them when classifying.
	NODISCARD static constexpr DTC_Final_Entry copy_rule(DTC_Intermediate_Entry intermediate)
	{
		return DTC_Final_Entry{static_cast<uint16_t>(intermediate.m_data & RULE_MASK)};
	}

	// Mutators set classification + DTZ value, preserving CAP_* rule bits.
	constexpr void set_score_win(uint16_t dtz)
	{
		ASSERT((dtz & VALUE_MASK) == dtz);
		m_data = (m_data & RULE_MASK) | dtz | DTC_FLAG_WIN;
	}
	constexpr void set_score_loss(uint16_t dtz)
	{
		ASSERT((dtz & VALUE_MASK) == dtz);
		m_data = (m_data & RULE_MASK) | dtz | DTC_FLAG_LOSS;
	}
	constexpr void set_score(DTC_Score v)
	{
		ASSERT((static_cast<uint16_t>(v) & VALUE_MASK) == static_cast<uint16_t>(v));
		m_data = (m_data & ~VALUE_MASK) | static_cast<uint16_t>(v);
	}

	NODISCARD constexpr DTC_Score value() const { return static_cast<DTC_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTC_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTC_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (VALUE_MASK | DTC_FLAG_WIN | DTC_FLAG_LOSS)) == 0;
	}

	NODISCARD constexpr DTC_Final_Entry score_only() const
	{
		return DTC_Final_Entry{static_cast<uint16_t>(m_data & VALUE_MASK)};
	}

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())
		{
			const bool cursed = has_cap_cwin() || value() > MAX_NON_CURSED_DTZ;
			return cursed ? WDL_Entry::CURSED_WIN : WDL_Entry::WIN;
		}
		if (is_loss())
		{
			const bool cursed = has_cap_closs() || value() > MAX_NON_CURSED_DTZ;
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

using DTC_Any_Entry = std::variant<DTC_Intermediate_Entry, DTC_Final_Entry>;

// =============================================================================
// DTM (no 50MR). Flat ply count to mate; no zeroing, no cursed/blessed.
// On disk: class stripped, recovered from companion .lzw (same split as DTC).
// Parity invariant (WIN odd, LOSS even) lets both rank tiers halve storage.
// Final holds WIN/LOSS+value; Intermediate holds PAWN_EVAL (forward-read marker)
// + CHANGE (reverify-pending). No shared rule bits — base only carries legality.
// =============================================================================

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

	// Huge_Array's For_Overwrite_Tag path uses `new T`; this ctor must zero m_data.
	constexpr DTM_Entry_Base() : m_data(0) {}
	constexpr explicit DTM_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr bool is_legal()   const { return (m_data & VALUE_MASK) != ILLEGAL_VAL; }
	NODISCARD constexpr bool is_illegal() const { return !is_legal(); }

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

	NODISCARD static constexpr DTM_Intermediate_Entry make_pawn_eval() { return DTM_Intermediate_Entry{DTM_FLAG_PAWN_EVAL}; }

	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (VALUE_MASK | DTM_FLAG_WIN | DTM_FLAG_LOSS)) == 0;
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

	// Construct from raw storage bits (for direct slice scans).
	NODISCARD static constexpr DTM_Final_Entry from_storage(uint16_t bits) { return DTM_Final_Entry{bits}; }

	// Mutators set classification + ply value. DTM has no rule bits.
	constexpr void set_score_win(uint16_t v)
	{
		ASSERT((v & VALUE_MASK) == v);
		m_data = v | DTM_FLAG_WIN;
	}
	constexpr void set_score_loss(uint16_t v)
	{
		ASSERT((v & VALUE_MASK) == v);
		m_data = v | DTM_FLAG_LOSS;
	}
	constexpr void set_score(DTM_Score v)
	{
		ASSERT((static_cast<uint16_t>(v) & VALUE_MASK) == static_cast<uint16_t>(v));
		m_data = (m_data & ~VALUE_MASK) | static_cast<uint16_t>(v);
	}

	NODISCARD constexpr DTM_Score value() const { return static_cast<DTM_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTM_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTM_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (VALUE_MASK | DTM_FLAG_WIN | DTM_FLAG_LOSS)) == 0;
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

using DTM_Any_Entry = std::variant<DTM_Intermediate_Entry, DTM_Final_Entry>;
