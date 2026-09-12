#pragma once

#include "chess/chess.h"
#include "util/defines.h"
#include "util/enum.h"
#include "util/fixed_vector.h"
#include "util/math.h"
#include "util/span.h"

enum struct WDL_Entry : uint8_t
{
	LOSE         = 0,
	BLESSED_LOSS = 1,
	DRAW         = 2,
	CURSED_WIN   = 3,
	WIN          = 4,
	ILLEGAL      = 7,
};

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

inline constexpr size_t WDL_ENTRY_PACK_RATIO = 2;
inline constexpr size_t WDL_ENTRY_BITS = 4;

NODISCARD constexpr Packed_WDL_Entries pack_wdl_entries(WDL_Stored v0, WDL_Stored v1)
{
	return static_cast<Packed_WDL_Entries>(
		  (static_cast<uint8_t>(v0) << 0)
		| (static_cast<uint8_t>(v1) << 4));
}

constexpr void pack_wdl_entries(Const_Span<WDL_Stored> in, Span<Packed_WDL_Entries> out)
{
	ASSERT(in.size() == out.size() * WDL_ENTRY_PACK_RATIO);
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = pack_wdl_entries(in[2 * i], in[2 * i + 1]);
}

constexpr void unpack_wdl_entries(Packed_WDL_Entries packed, WDL_Stored out[2])
{
	out[0] = static_cast<WDL_Stored>((packed >> 0) & 0xF);
	out[1] = static_cast<WDL_Stored>((packed >> 4) & 0xF);
}

constexpr void unpack_wdl_entries(Const_Span<Packed_WDL_Entries> in, Span<WDL_Stored> out)
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

inline constexpr uint8_t PACKED_WDL_ENTRY_INV_MASK[2] = {
	0b11110000, 0b00001111,
};

constexpr void set_wdl_entry(Packed_WDL_Entries& packed, size_t pos, WDL_Stored v)
{
	ASSERT(pos < WDL_ENTRY_PACK_RATIO);
	packed = static_cast<Packed_WDL_Entries>(
		(packed & PACKED_WDL_ENTRY_INV_MASK[pos]) | (static_cast<uint8_t>(v) << (pos * WDL_ENTRY_BITS)));
}

enum DTZ_Score : uint16_t {
	DTZ_SCORE_ZERO = 0,
	DTZ_SCORE_MAX = 0x07FEu,
};

ENUM_ENABLE_OPERATOR_INC(DTZ_Score);
ENUM_ENABLE_OPERATOR_ADD(DTZ_Score);

enum DTZ_Final_Entry_Flag : uint16_t {
	DTZ_FLAG_WIN  = 0x0800u,
	DTZ_FLAG_LOSS = 0x1000u,
};

enum DTZ_Rule_Flag : uint16_t {
	DTZ_FLAG_CAP_CWIN  = 0x2000u,
	DTZ_FLAG_CAP_CLOSS = 0x4000u,
};

enum DTZ_Intermediate_Entry_Flag : uint16_t {
	DTZ_FLAG_CHANGE = 0x8000u,
};

struct DTZ_Entry_Base
{
	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;
	static constexpr uint16_t RULE_MASK   = DTZ_FLAG_CAP_CWIN | DTZ_FLAG_CAP_CLOSS;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTZ_Rule_Flag>;

	static constexpr bool wants_zero_init_storage = false;

	constexpr DTZ_Entry_Base() : m_data(0) {}
	NODISCARD constexpr explicit DTZ_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr bool operator==(DTZ_Entry_Base o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTZ_Entry_Base o) const { return m_data != o.m_data; }

protected:
	uint16_t m_data;
};

struct DTZ_Final_Entry;

struct DTZ_Intermediate_Entry : DTZ_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTZ_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTZ_Intermediate_Entry_Flag>;

	constexpr DTZ_Intermediate_Entry() : DTZ_Entry_Base{} {}

	NODISCARD static constexpr DTZ_Intermediate_Entry make_cap_cwin()  { return DTZ_Intermediate_Entry{DTZ_FLAG_CAP_CWIN}; }
	NODISCARD static constexpr DTZ_Intermediate_Entry make_cap_draw()  { return DTZ_Intermediate_Entry{RULE_MASK}; }

	NODISCARD static constexpr DTZ_Intermediate_Entry make_cap_closs(uint16_t bound)
	{
		ASSERT(bound < ILLEGAL_VAL);
		return DTZ_Intermediate_Entry{static_cast<uint16_t>(bound | DTZ_FLAG_CAP_CLOSS)};
	}

	// No hint, only the clean bound. Zero reproduces a bare intermediate.
	NODISCARD static constexpr DTZ_Intermediate_Entry make_bound(uint16_t bound)
	{
		ASSERT(bound < ILLEGAL_VAL);
		return DTZ_Intermediate_Entry{bound};
	}

	NODISCARD constexpr bool has_cap_cwin()  const { return (m_data & RULE_MASK) == DTZ_FLAG_CAP_CWIN;  }
	NODISCARD constexpr bool has_cap_closs() const { return (m_data & RULE_MASK) == DTZ_FLAG_CAP_CLOSS; }
	NODISCARD constexpr bool has_cap_draw()  const { return (m_data & RULE_MASK) == RULE_MASK;          }

	// Any zeroing verdict cached (CWIN, CLOSS, or DRAW) — i.e. either rule bit set.
	NODISCARD constexpr bool has_any_hint()   const { return (m_data & RULE_MASK) != 0; }
	// A cursed cap routing hint: CWIN or CLOSS, i.e. exactly one rule bit set.
	NODISCARD constexpr bool has_cap_cursed() const
	{
		static_assert(DTZ_FLAG_CAP_CWIN << 1 == DTZ_FLAG_CAP_CLOSS);
		return ((m_data ^ (m_data << 1)) & DTZ_FLAG_CAP_CLOSS) != 0;
	}
	NODISCARD constexpr bool has_change()    const { return (m_data & DTZ_FLAG_CHANGE) != 0; }

	// The cached exit bound, or zero where no exit fixed one.
	NODISCARD constexpr uint16_t bound() const { return static_cast<uint16_t>(m_data & VALUE_MASK); }

	constexpr void set_flag(DTZ_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTZ_Intermediate_Entry_Flag f) { m_data &= ~f; }

private:
	NODISCARD constexpr explicit DTZ_Intermediate_Entry(uint16_t bits) : DTZ_Entry_Base{bits} {}
};
static_assert(sizeof(DTZ_Intermediate_Entry) == 2);

struct DTZ_Final_Entry : DTZ_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTZ_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTZ_Final_Entry_Flag>;

	static constexpr uint16_t MAX_NON_CURSED_DTZ = 100;

	constexpr DTZ_Final_Entry() : DTZ_Entry_Base{} {}

	NODISCARD static constexpr DTZ_Final_Entry make_illegal()           { return DTZ_Final_Entry{ILLEGAL_VAL}; }
	NODISCARD static constexpr DTZ_Final_Entry make_draw()              { return {}; }
	NODISCARD static constexpr DTZ_Final_Entry make_win(uint16_t v)   { return DTZ_Final_Entry{static_cast<uint16_t>(v | DTZ_FLAG_WIN)}; }
	NODISCARD static constexpr DTZ_Final_Entry make_loss(uint16_t v)  { return DTZ_Final_Entry{static_cast<uint16_t>(v | DTZ_FLAG_LOSS)}; }

	// Stamp the cursedness tag: CWIN on a win, CLOSS on a loss.
	constexpr void set_flag(DTZ_Rule_Flag f) { m_data |= f; }

	NODISCARD constexpr DTZ_Score value() const { return static_cast<DTZ_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_illegal() const { return (m_data & VALUE_MASK) == ILLEGAL_VAL; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTZ_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTZ_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTZ_FLAG_WIN | DTZ_FLAG_LOSS)) == 0;
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
	NODISCARD constexpr explicit DTZ_Final_Entry(uint16_t bits) : DTZ_Entry_Base{bits} {}
};
static_assert(sizeof(DTZ_Final_Entry) == 2);

NODISCARD constexpr WDL_Stored wdl_for_storage(DTZ_Final_Entry e)
{
	const WDL_Entry w = e.wdl();
	if (static_cast<uint16_t>(e.value()) == DTZ_Final_Entry::MAX_NON_CURSED_DTZ)
	{
		if (w == WDL_Entry::WIN)  return WDL_Stored::BOUNDARY_WIN;
		if (w == WDL_Entry::LOSE) return WDL_Stored::BOUNDARY_LOSS;
	}
	return static_cast<WDL_Stored>(w);
}

NODISCARD constexpr WDL_Entry wdl_from_storage(WDL_Stored s)
{
	if (s == WDL_Stored::BOUNDARY_WIN)  return WDL_Entry::WIN;
	if (s == WDL_Stored::BOUNDARY_LOSS) return WDL_Entry::LOSE;
	return static_cast<WDL_Entry>(s);
}

NODISCARD constexpr int wdl_class_rank(WDL_Entry w)
{
	return w == WDL_Entry::ILLEGAL ? -1 : static_cast<int>(w);
}

NODISCARD constexpr int wdl_class_rank(WDL_Stored s)
{
	return wdl_class_rank(wdl_from_storage(s));
}

// 1-byte tier halves cursed values;
// 2-byte tier writes raw values.
NODISCARD constexpr uint16_t dtz_value_for_storage(DTZ_Final_Entry e)
{
	const uint16_t v = static_cast<uint16_t>(e.value());
	const WDL_Entry w = e.wdl();
	return (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS)
		? static_cast<uint16_t>((v + 1) >> 1) : v;
}

// Pairs with the round-up encode: odd values recover exactly, even decode as v-1.
NODISCARD constexpr DTZ_Final_Entry dtz_entry_from_storage(uint16_t stored, WDL_Entry w, size_t entry_bytes)
{
	if (w == WDL_Entry::ILLEGAL) return DTZ_Final_Entry::make_illegal();
	if (w == WDL_Entry::DRAW)    return DTZ_Final_Entry::make_draw();

	const bool cursed = (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS);
	const bool win    = (w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN);

	uint16_t v = stored;
	if (entry_bytes == 1 && cursed)
		v = static_cast<uint16_t>((v << 1) - 1);

	DTZ_Final_Entry e = win ? DTZ_Final_Entry::make_win(v) : DTZ_Final_Entry::make_loss(v);
	if (cursed) e.set_flag(win ? DTZ_FLAG_CAP_CWIN : DTZ_FLAG_CAP_CLOSS);
	return e;
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

//   intermediate, written by init      final, written by retro taking one over
//   00    nothing                      00    nothing
//   MARK  no budget moves this cell    MARK  the cell is init's own
//   DRAW  a zeroing move draws         DRAW  init left it bare, retro took over
//   both  both of those                both  init left cap_draw, retro took over
enum DTC_Rule_Flag : uint16_t {
	DTC_FLAG_INIT_MARK = 0x2000u,
	DTC_FLAG_CAP_DRAW  = 0x4000u,
};

enum DTC_Intermediate_Entry_Flag : uint16_t {
	DTC_FLAG_CHANGE = 0x8000u,
};

struct DTC_Entry_Base
{
	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;
	static constexpr uint16_t RULE_MASK   = DTC_FLAG_INIT_MARK | DTC_FLAG_CAP_DRAW;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = std::is_same_v<FlagT, DTC_Rule_Flag>;

	static constexpr bool wants_zero_init_storage = false;

	constexpr DTC_Entry_Base() : m_data(0) {}
	NODISCARD constexpr explicit DTC_Entry_Base(uint16_t bits) : m_data(bits) {}

	NODISCARD constexpr bool operator==(DTC_Entry_Base o) const { return m_data == o.m_data; }
	NODISCARD constexpr bool operator!=(DTC_Entry_Base o) const { return m_data != o.m_data; }

protected:
	uint16_t m_data;
};

struct DTC_Intermediate_Entry : DTC_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTC_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTC_Intermediate_Entry_Flag>;

	constexpr DTC_Intermediate_Entry() : DTC_Entry_Base{} {}

	NODISCARD static constexpr DTC_Intermediate_Entry make_cap_draw()
	{
		return DTC_Intermediate_Entry{DTC_FLAG_CAP_DRAW};
	}
	// Init's seed, marked, for the layer above to inherit in turn.
	NODISCARD static constexpr DTC_Intermediate_Entry make_seed(bool cap_draw)
	{
		return DTC_Intermediate_Entry{static_cast<uint16_t>(
			DTC_FLAG_INIT_MARK | (cap_draw ? DTC_FLAG_CAP_DRAW : 0))};
	}

	// No hint, only the clean bound. Zero reproduces a bare intermediate.
	NODISCARD static constexpr DTC_Intermediate_Entry make_bound(uint16_t bound)
	{
		ASSERT(bound < ILLEGAL_VAL);
		return DTC_Intermediate_Entry{bound};
	}

	NODISCARD constexpr bool has_cap_draw()  const { return (m_data & DTC_FLAG_CAP_DRAW) != 0;  }
	NODISCARD constexpr bool has_init_mark() const { return (m_data & DTC_FLAG_INIT_MARK) != 0; }
	NODISCARD constexpr bool has_change()    const { return (m_data & DTC_FLAG_CHANGE) != 0;    }

	// The cached exit bound, or zero where no exit fixed one.
	NODISCARD constexpr uint16_t bound() const { return static_cast<uint16_t>(m_data & VALUE_MASK); }

	constexpr void set_flag(DTC_Rule_Flag f)                 { m_data |= f; }
	constexpr void set_flag(DTC_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTC_Intermediate_Entry_Flag f) { m_data &= ~f; }

private:
	NODISCARD constexpr explicit DTC_Intermediate_Entry(uint16_t bits) : DTC_Entry_Base{bits} {}
};
static_assert(sizeof(DTC_Intermediate_Entry) == 2);

struct DTC_Final_Entry : DTC_Entry_Base
{
	template <typename FlagT>
	static constexpr bool is_allowed_flag_type =
		DTC_Entry_Base::is_allowed_flag_type<FlagT>
		|| std::is_same_v<FlagT, DTC_Final_Entry_Flag>;

	static constexpr uint16_t MAX_DTC = DTZ_Final_Entry::MAX_NON_CURSED_DTZ;

	constexpr DTC_Final_Entry() : DTC_Entry_Base{} {}

	NODISCARD static constexpr DTC_Final_Entry make_illegal()        { return DTC_Final_Entry{ILLEGAL_VAL}; }
	NODISCARD static constexpr DTC_Final_Entry make_draw()           { return {}; }
	NODISCARD static constexpr DTC_Final_Entry make_win(uint16_t v)  { return DTC_Final_Entry{static_cast<uint16_t>(v | DTC_FLAG_WIN)}; }
	NODISCARD static constexpr DTC_Final_Entry make_loss(uint16_t v) { return DTC_Final_Entry{static_cast<uint16_t>(v | DTC_FLAG_LOSS)}; }

	constexpr void set_flag(DTC_Rule_Flag f) { m_data |= f; }
	NODISCARD constexpr bool has_init_mark() const
	{
		return (m_data & RULE_MASK) == DTC_FLAG_INIT_MARK;
	}
	NODISCARD constexpr bool has_retro_seed() const
	{
		return (m_data & DTC_FLAG_CAP_DRAW) != 0;
	}
	NODISCARD constexpr bool has_seed_cap_draw() const
	{
		return (m_data & DTC_FLAG_INIT_MARK) != 0;
	}

	NODISCARD constexpr DTC_Score value() const { return static_cast<DTC_Score>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool is_illegal() const { return (m_data & VALUE_MASK) == ILLEGAL_VAL; }

	NODISCARD constexpr bool is_win()  const { return (m_data & DTC_FLAG_WIN)  != 0; }
	NODISCARD constexpr bool is_loss() const { return (m_data & DTC_FLAG_LOSS) != 0; }
	NODISCARD constexpr bool is_draw() const
	{
		return (m_data & (DTC_FLAG_WIN | DTC_FLAG_LOSS)) == 0;
	}

	NODISCARD constexpr WDL_Entry wdl() const
	{
		if (is_illegal()) return WDL_Entry::ILLEGAL;
		if (is_win())     return WDL_Entry::WIN;
		if (is_loss())    return WDL_Entry::LOSE;
		return WDL_Entry::DRAW;
	}

private:
	NODISCARD constexpr explicit DTC_Final_Entry(uint16_t bits) : DTC_Entry_Base{bits} {}
};
static_assert(sizeof(DTC_Final_Entry) == 2);

NODISCARD constexpr DTC_Final_Entry dtc_entry_from_storage(uint16_t stored, WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::ILLEGAL: return DTC_Final_Entry::make_illegal();
		case WDL_Entry::WIN:     return DTC_Final_Entry::make_win(stored);
		case WDL_Entry::LOSE:    return DTC_Final_Entry::make_loss(stored);
		default:                 return DTC_Final_Entry::make_draw();
	}
}

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
	DTM_FLAG_CHANGE = 0x4000u,
};

struct DTM_Entry_Base
{
	static constexpr uint16_t VALUE_MASK  = 0x07FFu;
	static constexpr uint16_t ILLEGAL_VAL = VALUE_MASK;

	template <typename FlagT>
	static constexpr bool is_allowed_flag_type = false;

	static constexpr bool wants_zero_init_storage = false;

	constexpr DTM_Entry_Base() : m_data(0) {}
	NODISCARD constexpr explicit DTM_Entry_Base(uint16_t bits) : m_data(bits) {}

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

	NODISCARD static constexpr DTM_Intermediate_Entry make_bound(uint16_t bound)
	{
		ASSERT((bound & VALUE_MASK) == bound);
		ASSERT(bound != 0);
		return DTM_Intermediate_Entry{bound};
	}

	NODISCARD constexpr bool has_change() const { return (m_data & DTM_FLAG_CHANGE) != 0; }

	// The cached out-of-retro bound, or zero where no move fixed one.
	NODISCARD constexpr uint16_t bound() const { return static_cast<uint16_t>(m_data & VALUE_MASK); }

	NODISCARD constexpr bool has_flag(DTM_Intermediate_Entry_Flag f) const { return (m_data & f) != 0; }

	constexpr void set_flag(DTM_Intermediate_Entry_Flag f)   { m_data |= f; }
	constexpr void clear_flag(DTM_Intermediate_Entry_Flag f) { m_data &= ~f; }

private:
	NODISCARD constexpr explicit DTM_Intermediate_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
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
	NODISCARD constexpr explicit DTM_Final_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
};
static_assert(sizeof(DTM_Final_Entry) == 2);

// Both tiers store v/2 (parity invariant: WIN odd, LOSS even → lossless when
// class is known via the .lzw companion).
NODISCARD constexpr uint16_t dtm_value_for_storage(DTM_Final_Entry e)
{
	if (e.is_illegal()) return DTM_Final_Entry::ILLEGAL_VAL;
	return static_cast<uint16_t>(e.value()) >> 1;
}

// DTM has no cursed classes; CURSED_WIN→WIN, BLESSED_LOSS→LOSE.
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
	NODISCARD constexpr explicit DTM50_Intermediate_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
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
	NODISCARD constexpr explicit DTM50_Final_Entry(uint16_t bits) : DTM_Entry_Base{bits} {}
};
static_assert(sizeof(DTM50_Final_Entry) == 2);

// DRAW rides ILLEGAL_VAL: neither takes a rank. A real value is value() >> 1.
NODISCARD constexpr uint16_t dtm50_value_for_storage(DTM50_Final_Entry e)
{
	if (e.is_illegal() || e.is_draw()) return DTM50_Final_Entry::ILLEGAL_VAL;
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
