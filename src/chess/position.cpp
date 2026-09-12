#include "chess/position.h"

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/attack.h"
#include "chess/move.h"

#include "util/defines.h"


Piece Position::do_move(Move m)
{
	const Square from = m.from();
	const Square to   = m.to();
	const Piece mover = piece_at(from);
	ASSERT(mover != PIECE_NONE);
	ASSERT(piece_color(mover) == m_turn);

	const Color us   = m_turn;
	const Color them = color_opp(us);
	Piece captured = PIECE_NONE;

	if (m_castling_rooks) update_castling_rights(from, to);

	if (m.is_castling())
	{
		const bool h_side = to > from;
		const Square king_to = castling_king_to(us, h_side);
		const Square rook_to = castling_rook_to(us, h_side);
		const Piece rook = piece_make(us, ROOK);
		ASSERT(mover == piece_make(us, KING));
		ASSERT(piece_at(to) == rook);
		const Bitboard king_ft = square_bb(from) ^ square_bb(king_to);
		const Bitboard rook_ft = square_bb(to) ^ square_bb(rook_to);
		m_pieces[mover]            ^= king_ft;
		m_pieces[rook]             ^= rook_ft;
		m_pieces[piece_occupy(us)] ^= king_ft ^ rook_ft;
		set_square(from, PIECE_NONE);
		set_square(to, PIECE_NONE);
		set_square(king_to, mover);
		set_square(rook_to, rook);
	}
	else if (m.is_promotion())
	{
		if (!is_empty(to))
		{
			captured = piece_at(to);
			ASSERT(piece_color(captured) == them);
			remove_piece(to);
		}
		remove_piece(from);
		put_piece(piece_make(us, m.promotion()), to);
	}
	else if (m.is_ep_capture())
	{
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		captured = piece_at(cap_sq);
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		set_square(to, mover);
		set_square(from, PIECE_NONE);
		set_square(cap_sq, PIECE_NONE);
	}
	else if (!is_empty(to))
	{
		captured = piece_at(to);
		ASSERT(piece_color(captured) == them);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		set_square(to, mover);
		set_square(from, PIECE_NONE);
	}
	else
	{
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		set_square(to, mover);
		set_square(from, PIECE_NONE);
	}

	m_turn = them;
	return captured;
}

void Position::undo_move(Move m, Piece captured, Bitboard prev_castling)
{
	m_turn = color_opp(m_turn);
	const Square from = m.from();
	const Square to   = m.to();
	const Color us   = m_turn;
	const Color them = color_opp(us);

	m_castling_rooks = prev_castling;

	if (m.is_castling())
	{
		const bool h_side = to > from;
		const Square king_to = castling_king_to(us, h_side);
		const Square rook_to = castling_rook_to(us, h_side);
		const Piece king = piece_make(us, KING);
		const Piece rook = piece_make(us, ROOK);
		const Bitboard king_ft = square_bb(from) ^ square_bb(king_to);
		const Bitboard rook_ft = square_bb(to) ^ square_bb(rook_to);
		m_pieces[king]             ^= king_ft;
		m_pieces[rook]             ^= rook_ft;
		m_pieces[piece_occupy(us)] ^= king_ft ^ rook_ft;
		set_square(king_to, PIECE_NONE);
		set_square(rook_to, PIECE_NONE);
		set_square(from, king);
		set_square(to, rook);
	}
	else if (m.is_promotion())
	{
		remove_piece(to);
		put_piece(piece_make(us, PAWN), from);
		if (captured != PIECE_NONE)
			put_piece(captured, to);
	}
	else if (m.is_ep_capture())
	{
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Piece mover = piece_at(to);
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		set_square(from, mover);
		set_square(to, PIECE_NONE);
		set_square(cap_sq, captured);
	}
	else if (captured != PIECE_NONE)
	{
		const Piece mover = piece_at(to);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		set_square(from, mover);
		set_square(to, captured);
	}
	else
	{
		const Piece mover = piece_at(to);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		set_square(from, mover);
		set_square(to, PIECE_NONE);
	}
}
