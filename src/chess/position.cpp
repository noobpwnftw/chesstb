#include "chess/position.h"

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/attack.h"
#include "chess/move.h"

#include "util/defines.h"

#include <cstring>
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
	const Bitboard bq = m_pieces[piece_make(c, BISHOP)] | m_pieces[piece_make(c, QUEEN)];
	res |= rook_attacks(sq, occ)   & rq;
	res |= bishop_attacks(sq, occ) & bq;
	return res;
}

Piece Position::do_move(Move m)
{
	const Square from = m.from();
	const Square to   = m.to();
	const Piece mover = m_squares[from];
	ASSERT(mover != PIECE_NONE);
	ASSERT(piece_color(mover) == m_turn);

	Piece captured = PIECE_NONE;

	if (m.is_ep_capture())
	{
		// Captured pawn is on the same file as `to` but on `from`'s rank.
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		captured = m_squares[cap_sq];
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		remove_piece(cap_sq);
	}
	else if (!is_empty(to))
	{
		captured = m_squares[to];
		ASSERT(piece_color(captured) != m_turn);
		remove_piece(to);
	}

	remove_piece(from);
	if (m.is_promotion())
	{
		put_piece(piece_make(m_turn, m.promotion()), to);
	}
	else
	{
		put_piece(mover, to);
	}

	m_turn = color_opp(m_turn);
	return captured;
}

void Position::undo_move(Move m, Piece captured)
{
	m_turn = color_opp(m_turn);
	const Square from = m.from();
	const Square to   = m.to();

	const Piece placed = m_squares[to];
	ASSERT(placed != PIECE_NONE);
	remove_piece(to);
	if (m.is_promotion())
		put_piece(piece_make(m_turn, PAWN), from);
	else
		put_piece(placed, from);

	if (m.is_ep_capture())
	{
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		put_piece(captured, cap_sq);
	}
	else if (captured != PIECE_NONE)
	{
		put_piece(captured, to);
	}
}

namespace {

INLINE void add_pawn_moves_with_promos(Out_Param<Move_List> out, Square from, Square to, Color c)
{
	const Rank promo_rank = (c == WHITE) ? RANK_8 : RANK_1;
	if (sq_rank(to) == promo_rank)
	{
		out->add(Move::make_promotion(from, to, QUEEN));
		out->add(Move::make_promotion(from, to, ROOK));
		out->add(Move::make_promotion(from, to, BISHOP));
		out->add(Move::make_promotion(from, to, KNIGHT));
	}
	else
	{
		out->add(Move::make_quiet(from, to));
	}
}

}  // namespace

template <Position::Move_Kind K>
void Position::gen_pseudo_legal(Out_Param<Move_List> out) const
{
	out->clear();
	const Color me = m_turn;
	const Color opp = color_opp(me);
	const Bitboard occ = m_occupied;
	const Bitboard empty = ~occ;
	const Bitboard opp_bb = m_pieces[piece_occupy(opp)];

	// Knight / sliding / king (skip for PAWN_PUSHES; target depends on kind).
	if constexpr (K != Move_Kind::PAWN_PUSHES)
	{
		const Bitboard target =
			(K == Move_Kind::ALL)         ? ~m_pieces[piece_occupy(me)] :
			(K == Move_Kind::QUIETS)      ? empty :
			/* CONVERSIONS */                opp_bb;

		Bitboard knights = m_pieces[piece_make(me, KNIGHT)];
		while (knights)
		{
			const Square from = knights.pop_first_square();
			Bitboard moves = knight_attacks(from) & target;
			while (moves)
				out->add(Move::make_quiet(from, moves.pop_first_square()));
		}

		for (Piece_Type pt : { BISHOP, ROOK, QUEEN })
		{
			Bitboard b = m_pieces[piece_make(me, pt)];
			while (b)
			{
				const Square from = b.pop_first_square();
				Bitboard moves =
					(pt == BISHOP ? bishop_attacks(from, occ)
					 : pt == ROOK ? rook_attacks(from, occ)
					 :              queen_attacks(from, occ));
				moves &= target;
				while (moves)
					out->add(Move::make_quiet(from, moves.pop_first_square()));
			}
		}

		const Square ksq = king_square(me);
		Bitboard kmoves = king_attacks(ksq) & target;
		while (kmoves)
			out->add(Move::make_quiet(ksq, kmoves.pop_first_square()));
	}

	// Pawns (skip for QUIETS; pushes / push-promos / caps gated by kind).
	if constexpr (K != Move_Kind::QUIETS)
	{
		const Rank promo_rank = (me == WHITE) ? RANK_8 : RANK_1;
		Bitboard pawns = m_pieces[piece_make(me, PAWN)];
		while (pawns)
		{
			const Square from = pawns.pop_first_square();

			Bitboard push = pawn_pushes(me, from) & ~occ;
			if (push)
			{
				const Square to = push.peek_first_square();
				const bool is_promo = sq_rank(to) == promo_rank;
				if constexpr (K == Move_Kind::ALL)
				{
					add_pawn_moves_with_promos(out, from, to, me);
					if (!is_promo)
					{
						Bitboard dp = pawn_double_pushes(me, from) & ~occ;
						if (dp) out->add(Move::make_quiet(from, dp.peek_first_square()));
					}
				}
				else if constexpr (K == Move_Kind::CONVERSIONS)
				{
					if (is_promo) add_pawn_moves_with_promos(out, from, to, me);
				}
				else if constexpr (K == Move_Kind::PAWN_PUSHES)
				{
					if (!is_promo)
					{
						out->add(Move::make_quiet(from, to));
						Bitboard dp = pawn_double_pushes(me, from) & ~occ;
						if (dp) out->add(Move::make_quiet(from, dp.peek_first_square()));
					}
				}
			}

			if constexpr (K == Move_Kind::ALL || K == Move_Kind::CONVERSIONS)
			{
				Bitboard caps = pawn_attacks(me, from) & opp_bb;
				while (caps)
				{
					const Square to = caps.pop_first_square();
					add_pawn_moves_with_promos(out, from, to, me);
				}
			}
		}
	}
}

template void Position::gen_pseudo_legal<Position::Move_Kind::ALL>(Out_Param<Move_List>) const;
template void Position::gen_pseudo_legal<Position::Move_Kind::QUIETS>(Out_Param<Move_List>) const;
template void Position::gen_pseudo_legal<Position::Move_Kind::CONVERSIONS>(Out_Param<Move_List>) const;
template void Position::gen_pseudo_legal<Position::Move_Kind::PAWN_PUSHES>(Out_Param<Move_List>) const;

template <bool IncludePawnPushes>
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

	if constexpr (IncludePawnPushes)
	{
		Bitboard pawns = m_pieces[piece_make(mover, PAWN)];
		while (pawns)
		{
			const Square from_cur = pawns.pop_first_square();
			const Rank r = sq_rank(from_cur);
			const File f = sq_file(from_cur);
			if (mover == WHITE)
			{
				if (r >= RANK_3)
				{
					const Square prev1 = sq_make(static_cast<Rank>(r - 1), f);
					if (empty & square_bb(prev1))
						out->add(Move::make_quiet(from_cur, prev1));
				}
				if (r == RANK_4)
				{
					const Square inter = sq_make(RANK_3, f);
					const Square prev2 = sq_make(RANK_2, f);
					if ((empty & square_bb(inter)) && (empty & square_bb(prev2)))
						out->add(Move::make_quiet(from_cur, prev2));
				}
			}
			else
			{
				if (r <= RANK_6)
				{
					const Square prev1 = sq_make(static_cast<Rank>(r + 1), f);
					if (empty & square_bb(prev1))
						out->add(Move::make_quiet(from_cur, prev1));
				}
				if (r == RANK_5)
				{
					const Square inter = sq_make(RANK_6, f);
					const Square prev2 = sq_make(RANK_7, f);
					if ((empty & square_bb(inter)) && (empty & square_bb(prev2)))
						out->add(Move::make_quiet(from_cur, prev2));
				}
			}
		}
	}
}

template void Position::gen_pseudo_legal_pre_quiets<false>(Out_Param<Move_List>) const;
template void Position::gen_pseudo_legal_pre_quiets<true>(Out_Param<Move_List>) const;

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
	Bitboard attackers = Bitboard::make_empty();
	attackers |= pawn_attacks(m_turn, ksq)  & m_pieces[piece_make(opp, PAWN)]   & not_captured;
	attackers |= knight_attacks(ksq)        & m_pieces[piece_make(opp, KNIGHT)] & not_captured;
	attackers |= king_attacks(ksq)          & m_pieces[piece_make(opp, KING)]   & not_captured;
	const Bitboard rq = (m_pieces[piece_make(opp, ROOK)]   | m_pieces[piece_make(opp, QUEEN)]) & not_captured;
	const Bitboard bq = (m_pieces[piece_make(opp, BISHOP)] | m_pieces[piece_make(opp, QUEEN)]) & not_captured;
	attackers |= rook_attacks(ksq, occ_after)   & rq;
	attackers |= bishop_attacks(ksq, occ_after) & bq;

	return attackers == Bitboard::make_empty();
}

bool Position::is_legal() const
{
	// Side NOT to move must not be in check (otherwise the previous move was illegal).
	return !is_attacked_by(king_square(color_opp(m_turn)), m_turn);
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
