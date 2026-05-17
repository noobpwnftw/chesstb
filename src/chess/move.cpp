#include "chess/move.h"
#include "chess/chess.h"

#include <cstdio>
#include <cstring>
#include <iostream>

Move Move::make_from_string(const char* s)
{
	// Format: "<from><to>[<promo>]" — e.g. "e2e4", "e7e8q".
	if (std::strlen(s) < 4)
		return Move::make_null();

	char from_str[3] = { s[0], s[1], 0 };
	char to_str[3]   = { s[2], s[3], 0 };
	const Square from = square_from_string(from_str);
	const Square to   = square_from_string(to_str);
	if (from == SQ_END || to == SQ_END)
		return Move::make_null();

	if (std::strlen(s) >= 5)
	{
		Piece_Type promo = PIECE_TYPE_NONE;
		switch (s[4])
		{
			case 'q': case 'Q': promo = QUEEN;  break;
			case 'r': case 'R': promo = ROOK;   break;
			case 'b': case 'B': promo = BISHOP; break;
			case 'n': case 'N': promo = KNIGHT; break;
			default: return Move::make_null();
		}
		return Move::make_promotion(from, to, promo);
	}
	return Move::make_quiet(from, to);
}

void Move::to_string(char out[]) const
{
	square_to_string(from(), out);
	square_to_string(to(), out + 2);
	if (is_promotion())
	{
		const Piece_Type p = promotion();
		out[4] = (p == QUEEN ? 'q' : p == ROOK ? 'r' : p == BISHOP ? 'b' : 'n');
		out[5] = '\0';
	}
}

void move_display(Move m)
{
	char buf[8] = {};
	m.to_string(buf);
	std::cout << buf;
}
