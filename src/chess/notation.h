#pragma once

// Text forms: squares, FEN, UCI moves and material names, with CHESSTB_NOTATION.

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/span.h"

#include <string>

#if defined(CHESSTB_NOTATION)

inline constexpr size_t MAX_FEN_LENGTH = 120;

constexpr void square_to_string(Square sq, char out[])
{
	out[0] = static_cast<char>('a' + sq_file(sq));
	out[1] = static_cast<char>('1' + sq_rank(sq));
	out[2] = '\0';
}

// SQ_END unless `s` is exactly a square name.
NODISCARD constexpr Square square_from_string(const char s[])
{
	if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8' || s[2] != '\0')
		return SQ_END;
	return sq_make(static_cast<Rank>(s[1] - '1'), static_cast<File>(s[0] - 'a'));
}

// Shape only: piece counts, kings and pawn ranks are the caller's to judge.
// Throws std::runtime_error on a malformed FEN.
NODISCARD Position position_from_fen(const std::string& fen);
void position_to_fen(const Position& pos, Span<char> out);

// UCI with castling as king takes rook (UCI_Chess960). `out` holds 6 chars.
void move_to_string(Move m, char out[]);
// The move `s` names in `pos`; throws std::runtime_error on a malformed one.
// Legality is the caller's to judge.
NODISCARD Move move_from_string(const Position& pos, const std::string& s);

// Material names such as KQKR, KPpKp (opposing pair) or KrKR (castling rook).
NODISCARD bool is_piece_config_name(const std::string& name);
// Throws std::runtime_error unless is_piece_config_name(name).
NODISCARD Piece_Config piece_config_from_name(const std::string& name);

#endif
