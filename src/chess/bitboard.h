#pragma once

#include "chess.h"

#include "util/intrin.h"
#include "util/defines.h"
#include "util/enum.h"
#include "util/init.h"

#include <cstdint>
#include <array>

// Plain 64-bit bitboard. Square index = bit index (a1=bit 0, h8=bit 63).
struct Bitboard
{
	INLINE static constexpr Bitboard make_board_mask()
	{
		return Bitboard(~uint64_t(0));
	}

	INLINE static constexpr Bitboard make_empty()
	{
		return Bitboard(uint64_t(0));
	}

	// NOTE: not value-initialized by default for performance reasons.
	// Use Bitboard::make_empty() if you want an empty bitboard.
	INLINE Bitboard() = default;

	INLINE explicit constexpr Bitboard(uint64_t bits) :
		m_bits(bits)
	{
	}

	INLINE constexpr Bitboard(Const_Span<Square> squares) :
		m_bits(0)
	{
		for (const Square sq : squares)
			m_bits |= uint64_t(1) << sq;
	}

	INLINE constexpr Bitboard(const Bitboard&) = default;
	INLINE constexpr Bitboard(Bitboard&&) noexcept = default;
	INLINE constexpr Bitboard& operator=(const Bitboard&) = default;
	INLINE constexpr Bitboard& operator=(Bitboard&&) noexcept = default;

	INLINE constexpr Bitboard& operator&=(const Bitboard& b) { m_bits &= b.m_bits; return *this; }
	INLINE constexpr Bitboard& operator|=(const Bitboard& b) { m_bits |= b.m_bits; return *this; }
	INLINE constexpr Bitboard& operator^=(const Bitboard& b) { m_bits ^= b.m_bits; return *this; }

	NODISCARD INLINE friend constexpr Bitboard operator&(const Bitboard& a, const Bitboard& b) { return Bitboard(a.m_bits & b.m_bits); }
	NODISCARD INLINE friend constexpr Bitboard operator|(const Bitboard& a, const Bitboard& b) { return Bitboard(a.m_bits | b.m_bits); }
	NODISCARD INLINE friend constexpr Bitboard operator^(const Bitboard& a, const Bitboard& b) { return Bitboard(a.m_bits ^ b.m_bits); }
	NODISCARD INLINE friend constexpr Bitboard operator~(const Bitboard& a) { return Bitboard(~a.m_bits); }

	INLINE constexpr Bitboard& operator|=(Square sq);
	INLINE constexpr Bitboard& operator&=(Square sq);
	INLINE constexpr Bitboard& operator^=(Square sq);

	INLINE constexpr Bitboard& operator|=(Rank r);
	INLINE constexpr Bitboard& operator&=(Rank r);
	INLINE constexpr Bitboard& operator^=(Rank r);

	INLINE constexpr Bitboard& operator|=(File f);
	INLINE constexpr Bitboard& operator&=(File f);
	INLINE constexpr Bitboard& operator^=(File f);

	template <typename IntT>
	NODISCARD INLINE friend constexpr Bitboard operator>>(const Bitboard& bb, IntT bit)
	{
		return Bitboard(bb.m_bits >> bit);
	}

	template <typename IntT>
	NODISCARD INLINE friend constexpr Bitboard operator<<(const Bitboard& bb, IntT bit)
	{
		return Bitboard(bb.m_bits << bit);
	}

	NODISCARD INLINE friend constexpr bool operator==(const Bitboard& a, const Bitboard& b) noexcept { return a.m_bits == b.m_bits; }
	NODISCARD INLINE friend constexpr bool operator!=(const Bitboard& a, const Bitboard& b) noexcept { return a.m_bits != b.m_bits; }
	NODISCARD INLINE friend constexpr bool operator< (const Bitboard& a, const Bitboard& b) noexcept { return a.m_bits <  b.m_bits; }
	NODISCARD INLINE friend constexpr bool operator> (const Bitboard& a, const Bitboard& b) noexcept { return a.m_bits >  b.m_bits; }

	NODISCARD INLINE constexpr uint64_t bits() const { return m_bits; }

	NODISCARD INLINE constexpr operator bool() const { return m_bits != 0; }
	NODISCARD INLINE constexpr bool empty() const { return m_bits == 0; }

	NODISCARD INLINE size_t peek_1st_bit() const { return lsb(m_bits); }

	// Horizontal mirror (file a↔h, b↔g, ...). Equivalent to reversing the bits within each byte.
	NODISCARD INLINE constexpr Bitboard mirror_files() const
	{
		uint64_t x = m_bits;
		constexpr uint64_t k1 = 0x5555555555555555ULL;
		constexpr uint64_t k2 = 0x3333333333333333ULL;
		constexpr uint64_t k4 = 0x0f0f0f0f0f0f0f0fULL;
		x = ((x >> 1) & k1) | ((x & k1) << 1);
		x = ((x >> 2) & k2) | ((x & k2) << 2);
		x = ((x >> 4) & k4) | ((x & k4) << 4);
		return Bitboard(x);
	}

	// Vertical mirror (rank 1↔8, 2↔7, ...). One byte per rank → byteswap.
	NODISCARD INLINE constexpr Bitboard mirror_ranks() const
	{
		uint64_t x = m_bits;
		x = ((x >>  8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) <<  8);
		x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
		x = ( x >> 32                          ) | ( x                          << 32);
		return Bitboard(x);
	}

	// Transpose along the a1-h8 diagonal: square (file f, rank r) → (file r, rank f).
	NODISCARD INLINE constexpr Bitboard mirror_diag() const
	{
		uint64_t x = m_bits;
		constexpr uint64_t k1 = 0x5500550055005500ULL;
		constexpr uint64_t k2 = 0x3333000033330000ULL;
		constexpr uint64_t k4 = 0x0f0f0f0f00000000ULL;
		uint64_t t = k4 & (x ^ (x << 28));
		x ^= t ^ (t >> 28);
		t = k2 & (x ^ (x << 14));
		x ^= t ^ (t >> 14);
		t = k1 & (x ^ (x <<  7));
		x ^= t ^ (t >>  7);
		return Bitboard(x);
	}

	NODISCARD INLINE constexpr Bitboard maybe_mirror_files(bool mirr) const { return mirr ? mirror_files() : *this; }
	NODISCARD INLINE constexpr Bitboard maybe_mirror_ranks(bool mirr) const { return mirr ? mirror_ranks() : *this; }
	NODISCARD INLINE constexpr Bitboard maybe_mirror_diag (bool mirr) const { return mirr ? mirror_diag()  : *this; }

	INLINE size_t pop_1st_bit()
	{
		const size_t r = lsb(m_bits);
		m_bits &= m_bits - 1;
		return r;
	}

	NODISCARD INLINE size_t num_set_bits() const
	{
		return popcnt(m_bits);
	}

	NODISCARD INLINE bool has_only_one_set_bit() const
	{
		return m_bits != 0 && (m_bits & (m_bits - 1)) == 0;
	}

	INLINE Bitboard& set_bit(size_t idx)
	{
		m_bits |= uint64_t(1) << idx;
		return *this;
	}

	NODISCARD INLINE constexpr bool has_square(Square sq) const
	{
		ASSERT(sq >= SQ_START && sq < SQ_END);
		return (m_bits & (uint64_t(1) << sq)) != 0;
	}

	INLINE Square pop_first_square()
	{
		const size_t b = lsb(m_bits);
		m_bits &= m_bits - 1;
		return static_cast<Square>(b);
	}

	NODISCARD INLINE Square peek_first_square() const
	{
		return static_cast<Square>(lsb(m_bits));
	}

	INLINE Square pop_last_square()
	{
		const size_t b = msb(m_bits);
		m_bits ^= uint64_t(1) << b;
		return static_cast<Square>(b);
	}

	NODISCARD INLINE Square peek_last_square() const
	{
		return static_cast<Square>(msb(m_bits));
	}

	INLINE constexpr void clear()
	{
		m_bits = 0;
	}

	void display() const;

private:
	uint64_t m_bits;
};

static_assert(sizeof(Bitboard) == 8);

constexpr std::array<Bitboard, SQUARE_NB> SQ_BB_MASK = []() {
	auto res = make_filled_array<SQUARE_NB, Bitboard>(Bitboard::make_empty());
	for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
		res[sq] = Bitboard(uint64_t(1) << sq);
	return res;
}();

NODISCARD INLINE constexpr const Bitboard& square_bb(Square sq)
{
	return SQ_BB_MASK[sq];
}

NODISCARD INLINE constexpr Bitboard operator&(const Bitboard& a, Square sq) { return a & square_bb(sq); }
NODISCARD INLINE constexpr Bitboard operator|(const Bitboard& a, Square sq) { return a | square_bb(sq); }
NODISCARD INLINE constexpr Bitboard operator^(const Bitboard& a, Square sq) { return a ^ square_bb(sq); }

constexpr Bitboard& Bitboard::operator|=(Square sq) { return *this = *this | sq; }
constexpr Bitboard& Bitboard::operator&=(Square sq) { return *this = *this & sq; }
constexpr Bitboard& Bitboard::operator^=(Square sq) { return *this = *this ^ sq; }

constexpr std::array<Bitboard, RANK_NB> RANK_BB_MASK = []() {
	auto res = make_filled_array<RANK_NB, Bitboard>(Bitboard::make_empty());
	for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
		res[sq_rank(sq)] |= sq;
	return res;
}();

NODISCARD INLINE constexpr const Bitboard& rank_bb(Rank r) { return RANK_BB_MASK[r]; }
NODISCARD INLINE const Bitboard& square_rank_bb(Square sq) { return rank_bb(sq_rank(sq)); }

NODISCARD INLINE constexpr Bitboard operator&(const Bitboard& a, Rank r) { return a & rank_bb(r); }
NODISCARD INLINE constexpr Bitboard operator|(const Bitboard& a, Rank r) { return a | rank_bb(r); }
NODISCARD INLINE constexpr Bitboard operator^(const Bitboard& a, Rank r) { return a ^ rank_bb(r); }

constexpr std::array<Bitboard, FILE_NB> FILE_BB_MASK = []() {
	auto res = make_filled_array<FILE_NB, Bitboard>(Bitboard::make_empty());
	for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
		res[sq_file(sq)] |= sq;
	return res;
}();

NODISCARD INLINE constexpr const Bitboard& file_bb(File f) { return FILE_BB_MASK[f]; }
NODISCARD INLINE const Bitboard& square_file_bb(Square sq) { return file_bb(sq_file(sq)); }

NODISCARD INLINE constexpr Bitboard operator&(const Bitboard& a, File f) { return a & file_bb(f); }
NODISCARD INLINE constexpr Bitboard operator|(const Bitboard& a, File f) { return a | file_bb(f); }
NODISCARD INLINE constexpr Bitboard operator^(const Bitboard& a, File f) { return a ^ file_bb(f); }

constexpr Bitboard& Bitboard::operator|=(Rank r) { return *this = *this | r; }
constexpr Bitboard& Bitboard::operator&=(Rank r) { return *this = *this & r; }
constexpr Bitboard& Bitboard::operator^=(Rank r) { return *this = *this ^ r; }

constexpr Bitboard& Bitboard::operator|=(File f) { return *this = *this | f; }
constexpr Bitboard& Bitboard::operator&=(File f) { return *this = *this & f; }
constexpr Bitboard& Bitboard::operator^=(File f) { return *this = *this ^ f; }

INLINE constexpr Bitboard operator|(Square lhs, Square rhs) { return square_bb(lhs) | square_bb(rhs); }
INLINE constexpr Bitboard operator&(Square lhs, Square rhs) { return square_bb(lhs) & square_bb(rhs); }
INLINE constexpr Bitboard operator^(Square lhs, Square rhs) { return square_bb(lhs) ^ square_bb(rhs); }
INLINE constexpr Bitboard operator~(Square sq) { return ~square_bb(sq); }

INLINE constexpr Bitboard operator|(Rank lhs, Rank rhs) { return rank_bb(lhs) | rank_bb(rhs); }
INLINE constexpr Bitboard operator&(Rank lhs, Rank rhs) { return rank_bb(lhs) & rank_bb(rhs); }
INLINE constexpr Bitboard operator^(Rank lhs, Rank rhs) { return rank_bb(lhs) ^ rank_bb(rhs); }
INLINE constexpr Bitboard operator~(Rank r) { return ~rank_bb(r); }

INLINE constexpr Bitboard operator|(File lhs, File rhs) { return file_bb(lhs) | file_bb(rhs); }
INLINE constexpr Bitboard operator&(File lhs, File rhs) { return file_bb(lhs) & file_bb(rhs); }
INLINE constexpr Bitboard operator^(File lhs, File rhs) { return file_bb(lhs) ^ file_bb(rhs); }
INLINE constexpr Bitboard operator~(File f) { return ~file_bb(f); }

// Pawn legal-square mask: ranks 2..7.
constexpr Bitboard PAWN_AREA_BB = (RANK_2 | RANK_3 | RANK_4 | RANK_5 | RANK_6 | RANK_7);

NODISCARD INLINE constexpr const Bitboard& pawn_area_bb()
{
	return PAWN_AREA_BB;
}
