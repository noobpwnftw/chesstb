#include "chess/position.h"

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/attack.h"
#include "chess/move.h"

#include "util/defines.h"

#include <stdexcept>
#include <string>

Bitboard Position::attackers_to(Square sq, Color c, Bitboard occ) const
{
	Bitboard res = Bitboard::make_empty();
	// Pawn: a c-pawn at X attacks sq ⟺ X ∈ pawn_attacks(opp(c), sq).
	res |= pawn_attacks(color_opp(c), sq) & m_pieces[piece_make(c, PAWN)];
	res |= knight_attacks(sq)             & m_pieces[piece_make(c, KNIGHT)];
	res |= king_attacks(sq)               & m_pieces[piece_make(c, KING)];
	const Bitboard rq = m_pieces[piece_make(c, ROOK)]   | m_pieces[piece_make(c, QUEEN)];
	if (rq) res |= rook_attacks(sq, occ)   & rq;
	const Bitboard bq = m_pieces[piece_make(c, BISHOP)] | m_pieces[piece_make(c, QUEEN)];
	if (bq) res |= bishop_attacks(sq, occ) & bq;
	return res;
}

Piece Position::do_move(Move m)
{
	const Square from = m.from();
	const Square to   = m.to();
	const Piece mover = m_squares[from];
	ASSERT(mover != PIECE_NONE);
	ASSERT(piece_color(mover) == m_turn);

	const Color us   = m_turn;
	const Color them = color_opp(us);
	Piece captured = PIECE_NONE;

	if (m.is_promotion())
	{
		if (!is_empty(to))
		{
			captured = m_squares[to];
			ASSERT(piece_color(captured) == them);
			remove_piece(to);
		}
		remove_piece(from);
		put_piece(piece_make(us, m.promotion()), to);
	}
	else if (m.is_ep_capture())
	{
		// Captured pawn is on the same file as `to` but on `from`'s rank.
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		captured = m_squares[cap_sq];
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		// `to` was empty (EP target); both `from` and `cap_sq` clear.
		m_occupied ^= from_to ^ cap_bb;
		m_piece_counts[captured] -= 1;
		m_squares[to]     = mover;
		m_squares[from]   = PIECE_NONE;
		m_squares[cap_sq] = PIECE_NONE;
	}
	else if (!is_empty(to))
	{
		captured = m_squares[to];
		ASSERT(piece_color(captured) == them);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		// `to` stays occupied through the swap; only `from` flips.
		m_occupied ^= square_bb(from);
		m_piece_counts[captured] -= 1;
		m_squares[to]   = mover;
		m_squares[from] = PIECE_NONE;
	}
	else
	{
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		m_occupied                 ^= from_to;
		m_squares[to]   = mover;
		m_squares[from] = PIECE_NONE;
	}

	m_turn = them;
	return captured;
}

void Position::undo_move(Move m, Piece captured)
{
	m_turn = color_opp(m_turn);
	const Square from = m.from();
	const Square to   = m.to();
	const Color us   = m_turn;
	const Color them = color_opp(us);

	if (m.is_promotion())
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
		const Piece mover = m_squares[to];
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		m_occupied ^= from_to ^ cap_bb;
		m_piece_counts[captured] += 1;
		m_squares[from]   = mover;
		m_squares[to]     = PIECE_NONE;
		m_squares[cap_sq] = captured;
	}
	else if (captured != PIECE_NONE)
	{
		const Piece mover = m_squares[to];
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		m_occupied ^= square_bb(from);
		m_piece_counts[captured] += 1;
		m_squares[from] = mover;
		m_squares[to]   = captured;
	}
	else
	{
		const Piece mover = m_squares[to];
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		m_occupied                 ^= from_to;
		m_squares[from] = mover;
		m_squares[to]   = PIECE_NONE;
	}
}

void Position::gen_pseudo_legal_pre_quiets(Out_Param<Move_List> out) const
{
	// Inverted moves of opp(m_turn): from=current, to=where-they-came-from.
	out->clear();
	const Color mover = color_opp(m_turn);
	const Bitboard occ = m_occupied;
	const Bitboard empty = ~occ;

	// Knights.
	{
		Bitboard b = m_pieces[piece_make(mover, KNIGHT)];
		while (b)
		{
			const Square from_cur = b.pop_first_square();
			Bitboard candidates = knight_attacks(from_cur) & empty;
			while (candidates)
				out->add(Move::make_quiet(from_cur, candidates.pop_first_square()));
		}
	}

	// Bishops, rooks, queens (sliding attacks reverse identically).
	for (Piece_Type pt : { BISHOP, ROOK, QUEEN })
	{
		Bitboard b = m_pieces[piece_make(mover, pt)];
		while (b)
		{
			const Square from_cur = b.pop_first_square();
			Bitboard candidates =
				(pt == BISHOP ? bishop_attacks(from_cur, occ)
				 : pt == ROOK ? rook_attacks(from_cur, occ)
				 :              queen_attacks(from_cur, occ));
			candidates &= empty;
			while (candidates)
				out->add(Move::make_quiet(from_cur, candidates.pop_first_square()));
		}
	}

	// King.
	{
		const Square from_cur = king_square(mover);
		Bitboard candidates = king_attacks(from_cur) & empty;
		while (candidates)
			out->add(Move::make_quiet(from_cur, candidates.pop_first_square()));
	}
}

bool Position::is_pseudo_legal_move_legal(Move m) const
{
	const Color opp = color_opp(m_turn);
	const Square from = m.from();
	const Square to = m.to();
	const Square ksq_old = king_square(m_turn);
	const Square ksq = (from == ksq_old) ? to : ksq_old;

	Bitboard occ_after = (m_occupied ^ square_bb(from)) | square_bb(to);
	Bitboard captured_mask = Bitboard::make_empty();
	if (m.is_ep_capture())
	{
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		occ_after ^= square_bb(cap_sq);
		captured_mask = square_bb(cap_sq);
	}
	else if (m_squares[to] != PIECE_NONE)
	{
		// Regular capture: 'to' was occupied before by opp; occupancy stays set
		// (mover replaces capturee), but opp's piece bbs still hold 'to' since
		// we don't mutate. Mask that bit out of the attacker set.
		captured_mask = square_bb(to);
	}

	const Bitboard not_captured = ~captured_mask;

	if (pawn_attacks(m_turn, ksq) & m_pieces[piece_make(opp, PAWN)]   & not_captured) return false;
	if (knight_attacks(ksq)       & m_pieces[piece_make(opp, KNIGHT)] & not_captured) return false;
	if (king_attacks(ksq)         & m_pieces[piece_make(opp, KING)]   & not_captured) return false;

	const Bitboard rq = (m_pieces[piece_make(opp, ROOK)]   | m_pieces[piece_make(opp, QUEEN)]) & not_captured;
	if (rq && (rook_attacks(ksq, occ_after) & rq)) return false;
	const Bitboard bq = (m_pieces[piece_make(opp, BISHOP)] | m_pieces[piece_make(opp, QUEEN)]) & not_captured;
	if (bq && (bishop_attacks(ksq, occ_after) & bq)) return false;

	return true;
}

bool Position::is_legal() const
{
	// Side NOT to move must not be in check (otherwise the previous move was illegal).
	return !is_attacked_by(king_square(color_opp(m_turn)), m_turn);
}

Bitboard Position::pinned_pieces() const
{
	const Color me  = m_turn;
	const Color opp = color_opp(me);
	const Square ksq = king_square(me);
	const Bitboard occ = m_occupied;
	const Bitboard own = m_pieces[piece_occupy(me)];

	const Bitboard opp_rq = m_pieces[piece_make(opp, ROOK)]   | m_pieces[piece_make(opp, QUEEN)];
	const Bitboard opp_bq = m_pieces[piece_make(opp, BISHOP)] | m_pieces[piece_make(opp, QUEEN)];

	Bitboard pinned = Bitboard::make_empty();

	// For each enemy slider type: a pinned piece is the king's first blocker on a
	// line; removing it must expose an enemy slider of that type behind it.
	if (opp_rq)
	{
		const Bitboard ray = rook_attacks(ksq, occ);  // squares the king sees on rook lines
		Bitboard blockers = ray & own;
		while (blockers)
		{
			const Square s = blockers.pop_first_square();
			const Bitboard behind = rook_attacks(ksq, occ ^ square_bb(s)) & ~ray;
			if (behind & opp_rq) pinned |= square_bb(s);
		}
	}
	if (opp_bq)
	{
		const Bitboard ray = bishop_attacks(ksq, occ);
		Bitboard blockers = ray & own;
		while (blockers)
		{
			const Square s = blockers.pop_first_square();
			const Bitboard behind = bishop_attacks(ksq, occ ^ square_bb(s)) & ~ray;
			if (behind & opp_bq) pinned |= square_bb(s);
		}
	}
	return pinned;
}

Position Position::from_fen(const std::string& fen)
{
	Position p;
	p.clear();

	int idx = 0;
	int rank = 7, file = 0;
	for (; idx < (int)fen.size(); ++idx)
	{
		const char c = fen[idx];
		if (c == ' ') break;
		if (c == '/') { --rank; file = 0; continue; }
		if (c >= '1' && c <= '8') { file += c - '0'; continue; }
		const Piece pc = piece_from_char(c);
		if (pc == PIECE_NONE)
			throw std::runtime_error(std::string("Invalid FEN char: ") + c);
		p.put_piece(pc, sq_make(static_cast<Rank>(rank), static_cast<File>(file)));
		++file;
	}

	// Side to move (one char after the space).
	while (idx < (int)fen.size() && fen[idx] == ' ') ++idx;
	if (idx < (int)fen.size())
	{
		const char stm = fen[idx];
		p.m_turn = (stm == 'b' || stm == 'B') ? BLACK : WHITE;
	}

	return p;
}

void Position::to_fen(Span<char> out) const
{
	size_t w = 0;
	auto put = [&](char c) { if (w + 1 < out.size()) out[w++] = c; };
	for (int rank = 7; rank >= 0; --rank)
	{
		int run = 0;
		for (int file = 0; file < 8; ++file)
		{
			const Piece pc = m_squares[sq_make(static_cast<Rank>(rank), static_cast<File>(file))];
			if (pc == PIECE_NONE)
			{
				++run;
			}
			else
			{
				if (run) put(char('0' + run));
				run = 0;
				put(piece_to_char(pc));
			}
		}
		if (run) put(char('0' + run));
		if (rank > 0) put('/');
	}
	put(' ');
	put(m_turn == WHITE ? 'w' : 'b');
	if (w < out.size()) out[w] = '\0';
}
