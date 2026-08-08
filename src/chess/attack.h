#pragma once

#include "chess/chess.h"
#include "chess/bitboard.h"

#include "util/defines.h"

// Per-square precomputed attack tables for non-sliding pieces, computed once
// by attack_init().

extern Bitboard KING_ATTACKS[SQUARE_NB];
extern Bitboard KNIGHT_ATTACKS[SQUARE_NB];
extern Bitboard PAWN_ATTACKS[COLOR_NB][SQUARE_NB];
extern Bitboard PAWN_PUSHES[COLOR_NB][SQUARE_NB];           // single push target (if any)
extern Bitboard PAWN_DOUBLE_PUSHES[COLOR_NB][SQUARE_NB];    // double push target (only from start rank)

void attack_init();

NODISCARD INLINE Bitboard king_attacks(Square sq) { return KING_ATTACKS[sq]; }
NODISCARD INLINE Bitboard knight_attacks(Square sq) { return KNIGHT_ATTACKS[sq]; }
NODISCARD INLINE Bitboard pawn_attacks(Color c, Square sq) { return PAWN_ATTACKS[c][sq]; }
NODISCARD INLINE Bitboard pawn_pushes(Color c, Square sq) { return PAWN_PUSHES[c][sq]; }
NODISCARD INLINE Bitboard pawn_double_pushes(Color c, Square sq) { return PAWN_DOUBLE_PUSHES[c][sq]; }

NODISCARD Bitboard bishop_attacks(Square sq, Bitboard occupied);
NODISCARD Bitboard rook_attacks(Square sq, Bitboard occupied);

NODISCARD INLINE Bitboard queen_attacks(Square sq, Bitboard occupied)
{
	return bishop_attacks(sq, occupied) | rook_attacks(sq, occupied);
}

// Attacks `to` from `from`, for the given piece. Includes blocker check.
NODISCARD Bitboard piece_attacks(Piece pc, Square from, Bitboard occupied);
