#pragma once

#include "chess/chess.h"
#include "chess/bitboard.h"

#include "util/defines.h"
#include "util/init.h"

#include <array>

// Per-square attack tables for non-sliding pieces. Computed at compile time,
// so there is nothing to initialize at startup.

namespace detail
{
	constexpr void attack_try_add(Bitboard& bb, int r, int f)
	{
		if (r >= 0 && r <= 7 && f >= 0 && f <= 7)
			bb |= sq_make(static_cast<Rank>(r), static_cast<File>(f));
	}

	using Attack_Table = std::array<Bitboard, SQUARE_NB>;
	using Attack_Table_By_Color = std::array<Attack_Table, COLOR_NB>;

	NODISCARD constexpr Attack_Table make_empty_attack_table()
	{
		return make_filled_array<SQUARE_NB, Bitboard>(Bitboard::make_empty());
	}

	NODISCARD constexpr Attack_Table_By_Color make_empty_attack_table_by_color()
	{
		return make_filled_array<COLOR_NB>(make_empty_attack_table());
	}
}

inline constexpr detail::Attack_Table KING_ATTACKS = []() {
	auto res = detail::make_empty_attack_table();
	for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
	{
		const int r = sq_rank(sq);
		const int f = sq_file(sq);
		for (int dr = -1; dr <= 1; ++dr)
			for (int df = -1; df <= 1; ++df)
				if (dr || df)
					detail::attack_try_add(res[sq], r + dr, f + df);
	}
	return res;
}();

inline constexpr detail::Attack_Table KNIGHT_ATTACKS = []() {
	constexpr int K[8][2] = {
		{ 1, 2}, { 2, 1}, { 2,-1}, { 1,-2},
		{-1,-2}, {-2,-1}, {-2, 1}, {-1, 2}
	};
	auto res = detail::make_empty_attack_table();
	for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
		for (auto& d : K)
			detail::attack_try_add(res[sq], sq_rank(sq) + d[0], sq_file(sq) + d[1]);
	return res;
}();

inline constexpr detail::Attack_Table_By_Color PAWN_ATTACKS = []() {
	auto res = detail::make_empty_attack_table_by_color();
	for (Color c : { WHITE, BLACK })
	{
		const int dr_fwd = (c == WHITE) ? 1 : -1;
		for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
		{
			detail::attack_try_add(res[c][sq], sq_rank(sq) + dr_fwd, sq_file(sq) - 1);
			detail::attack_try_add(res[c][sq], sq_rank(sq) + dr_fwd, sq_file(sq) + 1);
		}
	}
	return res;
}();

// Single push target (if any).
inline constexpr detail::Attack_Table_By_Color PAWN_PUSHES = []() {
	auto res = detail::make_empty_attack_table_by_color();
	for (Color c : { WHITE, BLACK })
	{
		const int dr_fwd = (c == WHITE) ? 1 : -1;
		for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
			detail::attack_try_add(res[c][sq], sq_rank(sq) + dr_fwd, sq_file(sq));
	}
	return res;
}();

// Double push target (only from the start rank).
inline constexpr detail::Attack_Table_By_Color PAWN_DOUBLE_PUSHES = []() {
	auto res = detail::make_empty_attack_table_by_color();
	for (Color c : { WHITE, BLACK })
	{
		const int dr_fwd = (c == WHITE) ? 1 : -1;
		const int start_rank = (c == WHITE) ? 1 : 6;
		for (Square sq = SQ_A1; sq < SQUARE_NB; ++sq)
			if (sq_rank(sq) == start_rank)
				detail::attack_try_add(res[c][sq], sq_rank(sq) + 2 * dr_fwd, sq_file(sq));
	}
	return res;
}();

NODISCARD constexpr Bitboard king_attacks(Square sq) { return KING_ATTACKS[sq]; }
NODISCARD constexpr Bitboard knight_attacks(Square sq) { return KNIGHT_ATTACKS[sq]; }
NODISCARD constexpr Bitboard pawn_attacks(Color c, Square sq) { return PAWN_ATTACKS[c][sq]; }
NODISCARD constexpr Bitboard pawn_pushes(Color c, Square sq) { return PAWN_PUSHES[c][sq]; }
NODISCARD constexpr Bitboard pawn_double_pushes(Color c, Square sq) { return PAWN_DOUBLE_PUSHES[c][sq]; }

// Slider attacks. On x86 these read PEXT tables built once before main() by a
// static initializer in attack.cpp; elsewhere they walk rays directly.
NODISCARD Bitboard bishop_attacks(Square sq, Bitboard occupied);
NODISCARD Bitboard rook_attacks(Square sq, Bitboard occupied);

NODISCARD INLINE Bitboard queen_attacks(Square sq, Bitboard occupied)
{
	return bishop_attacks(sq, occupied) | rook_attacks(sq, occupied);
}

// Attacks `to` from `from`, for the given piece. Includes blocker check.
NODISCARD Bitboard piece_attacks(Piece pc, Square from, Bitboard occupied);
