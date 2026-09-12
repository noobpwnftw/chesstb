#pragma once

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/move.h"
#include "chess/attack.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/span.h"

// No en-passant state — callers pass an ep square in where they need one.
// No move history — undo_move requires the caller to pass back the captured piece.
struct Position
{
	Bitboard m_pieces[PIECE_NB];     // [WHITE_OCCUPY] = all white, [BLACK_OCCUPY] = all black
#if defined(CHESSTB_GEN)
	Piece    m_squares[SQUARE_NB];
#endif
	Color    m_turn;
	Bitboard m_castling_rooks;       // the rooks still holding a castling right

	void clear()
	{
		std::memset(this, 0, sizeof(*this));
	}

	NODISCARD Color turn() const { return m_turn; }
	void set_turn(Color c) { m_turn = c; }

	NODISCARD Bitboard castling() const { return m_castling_rooks; }

	// The rooks of `c` still holding a castling right.
	NODISCARD Bitboard castling_rooks(Color c) const { return m_castling_rooks & castling_home_rank(c); }

	void update_castling_rights(Square from, Square to)
	{
		for (const Color c : { WHITE, BLACK })
			if (from == king_square(c)) m_castling_rooks &= ~castling_rooks(c);
		m_castling_rooks &= ~(square_bb(from) | square_bb(to));
	}

	NODISCARD bool is_empty(Square sq) const { return !occupied().has_square(sq); }

	NODISCARD FORCE_INLINE bool move_is_capture(Move m) const
	{
		if (m.is_castling()) return false;
		return m.is_ep_capture() || occupied().has_square(m.to());
	}

	NODISCARD FORCE_INLINE Piece piece_at(Square sq) const
	{
#if defined(CHESSTB_GEN)
		return m_squares[sq];
#else
		if (!occupied().has_square(sq)) return PIECE_NONE;
		const Color c = m_pieces[WHITE_OCCUPY].has_square(sq) ? WHITE : BLACK;
		Piece_Type pt = PAWN;
		while (!m_pieces[piece_make(c, pt)].has_square(sq)) pt = static_cast<Piece_Type>(pt + 1);
		return piece_make(c, pt);
#endif
	}

	FORCE_INLINE void set_square([[maybe_unused]] Square sq, [[maybe_unused]] Piece pc)
	{
#if defined(CHESSTB_GEN)
		m_squares[sq] = pc;
#endif
	}
	NODISCARD Bitboard occupied() const { return m_pieces[WHITE_OCCUPY] | m_pieces[BLACK_OCCUPY]; }
	NODISCARD const Bitboard& color_bb(Color c) const { return m_pieces[piece_occupy(c)]; }
	NODISCARD const Bitboard& piece_bb(Piece p) const { return m_pieces[p]; }

	NODISCARD FORCE_INLINE Square king_square(Color c) const
	{
		ASSERT(m_pieces[piece_make(c, KING)].num_set_bits() == 1);
		return m_pieces[piece_make(c, KING)].peek_first_square();
	}

	FORCE_INLINE void put_piece(Piece pc, Square sq)
	{
		ASSERT(is_empty(sq));
		const Bitboard sb = square_bb(sq);
		m_pieces[pc] |= sb;
		m_pieces[piece_occupy(piece_color(pc))] |= sb;
		set_square(sq, pc);
	}

	FORCE_INLINE void remove_piece(Square sq)
	{
		const Piece pc = piece_at(sq);
		ASSERT(pc != PIECE_NONE);
		const Bitboard sb = square_bb(sq);
		m_pieces[pc] ^= sb;
		m_pieces[piece_occupy(piece_color(pc))] ^= sb;
		set_square(sq, PIECE_NONE);
	}

	NODISCARD FORCE_INLINE Bitboard attackers_to(Square sq, Color c, Bitboard occ) const
	{
		Bitboard res = Bitboard::make_empty();
		// Color flipped: a c-pawn attacks sq iff it stands in pawn_attacks(opp(c), sq).
		res |= pawn_attacks(color_opp(c), sq) & m_pieces[piece_make(c, PAWN)];
		res |= knight_attacks(sq)             & m_pieces[piece_make(c, KNIGHT)];
		res |= king_attacks(sq)               & m_pieces[piece_make(c, KING)];
		const Bitboard rq = m_pieces[piece_make(c, ROOK)]   | m_pieces[piece_make(c, QUEEN)];
		if (rq) res |= rook_attacks(sq, occ)   & rq;
		const Bitboard bq = m_pieces[piece_make(c, BISHOP)] | m_pieces[piece_make(c, QUEEN)];
		if (bq) res |= bishop_attacks(sq, occ) & bq;
		return res;
	}

	NODISCARD FORCE_INLINE bool is_attacked_by(Square sq, Color attacker_color) const
	{
		return attackers_to(sq, attacker_color, occupied()) != Bitboard::make_empty();
	}

	NODISCARD FORCE_INLINE bool is_in_check() const
	{
		return is_attacked_by(king_square(m_turn), color_opp(m_turn));
	}

	// do_move returns the piece captured (PIECE_NONE if none) and folds the
	// castling-rights update in, so undo needs the pre-move rights handed back.
	Piece do_move(Move m);
	void  undo_move(Move m, Piece captured, Bitboard prev_castling);

	// No EP, no king-in-check filter. `f` returns true to stop; returns true iff it did.
	template <typename F>
	GEN_FORCE_INLINE bool visit_pseudo_legal_moves(F&& f) const
	{
		const Color me = m_turn;
		const Bitboard occ = occupied();
		const Bitboard target = ~m_pieces[piece_occupy(me)];

		Bitboard knights = m_pieces[piece_make(me, KNIGHT)];
		while (knights)
		{
			const Square from = knights.pop_first_square();
			Bitboard moves = knight_attacks(from) & target;
			while (moves)
				if (f(Move::make_quiet(from, moves.pop_first_square()))) return true;
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
					if (f(Move::make_quiet(from, moves.pop_first_square()))) return true;
			}
		}

		const Square ksq = king_square(me);
		Bitboard kmoves = king_attacks(ksq) & target;
		while (kmoves)
			if (f(Move::make_quiet(ksq, kmoves.pop_first_square()))) return true;

		if (castling_rooks(me))
		{
			for (Bitboard rooks = castling_rooks(me); rooks;)
			{
				const Square rfrom = rooks.pop_last_square();
				const bool h_side = rfrom > ksq;
				const Square kto = castling_king_to(me, h_side);
				const Square rto = castling_rook_to(me, h_side);
				const Bitboard path = (rank_span_bb(ksq, kto) | rank_span_bb(rfrom, rto))
				                    & ~(square_bb(ksq) | square_bb(rfrom));
				if (occ & path) continue;
				if (f(Move::make_castling(ksq, rfrom))) return true;
			}
		}

		const Rank promo_rank = (me == WHITE) ? RANK_8 : RANK_1;
		auto visit_pawn_move = [&](Square from, Square to) FORCE_INLINE_LAMBDA {
			if (sq_rank(to) == promo_rank)
			{
				return f(Move::make_promotion(from, to, QUEEN))
				    || f(Move::make_promotion(from, to, ROOK))
				    || f(Move::make_promotion(from, to, BISHOP))
				    || f(Move::make_promotion(from, to, KNIGHT));
			}
			return f(Move::make_quiet(from, to));
		};
		Bitboard pawns = m_pieces[piece_make(me, PAWN)];
		while (pawns)
		{
			const Square from = pawns.pop_first_square();

			Bitboard push = pawn_pushes(me, from) & ~occ;
			if (push)
			{
				const Square to = push.peek_first_square();
				if (visit_pawn_move(from, to)) return true;
				if (sq_rank(to) != promo_rank)
				{
					Bitboard dp = pawn_double_pushes(me, from) & ~occ;
					if (dp && f(Move::make_quiet(from, dp.peek_first_square()))) return true;
				}
			}

			Bitboard caps = pawn_attacks(me, from) & m_pieces[piece_occupy(color_opp(me))];
			while (caps)
			{
				const Square to = caps.pop_first_square();
				if (visit_pawn_move(from, to)) return true;
			}
		}
		return false;
	}

	// The en-passant captures available to the side to move given `ep_square`
	// (SQ_END for none). Position stores no EP state, so the square is supplied.
	template <typename F>
	GEN_FORCE_INLINE bool visit_pseudo_legal_ep_captures(Square ep_square, F&& f) const
	{
		if (ep_square == SQ_END) return false;
		const Color me = m_turn;
		const Rank target_rank = (me == WHITE) ? RANK_6 : RANK_3;
		const Rank pawn_rank   = (me == WHITE) ? RANK_5 : RANK_4;
		if (sq_rank(ep_square) != target_rank) return false;

		const File target_file = sq_file(ep_square);
		for (int df : { -1, +1 })
		{
			const int fi = static_cast<int>(target_file) + df;
			if (fi < 0 || fi >= 8) continue;
			const Square from = sq_make(pawn_rank, static_cast<File>(fi));
			if (!m_pieces[piece_make(me, PAWN)].has_square(from)) continue;

			const Square cap_sq = sq_make(pawn_rank, target_file);
			if (!m_pieces[piece_make(color_opp(me), PAWN)].has_square(cap_sq)) continue;

			if (f(Move::make_ep_capture(from, ep_square))) return true;
		}
		return false;
	}

	// Legal moves, no en passant. `f` returns true to stop; returns true iff it did.
	template <typename F>
	FORCE_INLINE bool visit_legal_moves(F&& f) const
	{
		const Legality ctx = legality_context();
		return visit_pseudo_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
			return is_pseudo_legal_move_legal(m, ctx) && f(m);
		});
	}

	// Legal en-passant captures. Returns true iff `f` stopped it early.
	template <typename F>
	FORCE_INLINE bool visit_legal_ep_captures(Square ep_square, F&& f) const
	{
		return visit_pseudo_legal_ep_captures(ep_square, [&](Move m) FORCE_INLINE_LAMBDA {
			return is_pseudo_legal_move_legal(m) && f(m);
		});
	}

	NODISCARD FORCE_INLINE bool is_checkmate(Square ep_square = SQ_END) const
	{
		if (!is_in_check()) return false;

		// In check, so a Legality context's !in_check fast path could never fire.
		auto legal = [&](Move m) FORCE_INLINE_LAMBDA {
			return is_pseudo_legal_move_legal(m);
		};
		return !(visit_pseudo_legal_moves(legal)
		         || visit_pseudo_legal_ep_captures(ep_square, legal));
	}

	// Predecessor quiet moves, inverted: Move::from() = current square, Move::to()
	// = where the mover came from. Knights, sliders and king only — pawn moves are
	// excluded because retro generation cannot cross pawn slices. No early exit.
	template <typename F>
	FORCE_INLINE void visit_pseudo_legal_pre_quiets(F&& f) const
	{
		const Color mover = color_opp(m_turn);
		const Bitboard occ = occupied();
		const Bitboard empty = ~occ;

		Bitboard knights = m_pieces[piece_make(mover, KNIGHT)];
		while (knights)
		{
			const Square from_cur = knights.pop_first_square();
			Bitboard candidates = knight_attacks(from_cur) & empty;
			while (candidates)
				f(Move::make_quiet(from_cur, candidates.pop_first_square()));
		}

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
					f(Move::make_quiet(from_cur, candidates.pop_first_square()));
			}
		}

		const Square ksq = king_square(mover);
		Bitboard kmoves = king_attacks(ksq) & empty;
		while (kmoves)
			f(Move::make_quiet(ksq, kmoves.pop_first_square()));
	}

	NODISCARD GEN_FORCE_INLINE bool is_pseudo_legal_move_legal(Move m) const
	{
		const Color opp = color_opp(m_turn);
		const Square from = m.from();
		const Square to = m.to();

		if (m.is_castling())
		{
			const bool h_side = to > from;
			const Square king_to = castling_king_to(m_turn, h_side);
			const Square rook_to = castling_rook_to(m_turn, h_side);
			if (is_attacked_by(from, opp)) return false;
			if (king_to != from)
			{
				const int step = (king_to > from) ? 1 : -1;
				for (Square s = from + step; s != king_to; s = s + step)
					if (is_attacked_by(s, opp)) return false;
			}

			const Bitboard occ_castled =
				((occupied() ^ square_bb(from)) ^ square_bb(to))
				| square_bb(king_to) | square_bb(rook_to);
			return attackers_to(king_to, opp, occ_castled) == Bitboard::make_empty();
		}
		const Square ksq_old = king_square(m_turn);
		const Square ksq = (from == ksq_old) ? to : ksq_old;

		Bitboard occ_after = (occupied() ^ square_bb(from)) | square_bb(to);
		Bitboard captured_mask = Bitboard::make_empty();
		if (m.is_ep_capture())
		{
			const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
			occ_after ^= square_bb(cap_sq);
			captured_mask = square_bb(cap_sq);
		}
		else if (occupied().has_square(to))
		{
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

	NODISCARD FORCE_INLINE Bitboard pinned_pieces() const
	{
		const Color me  = m_turn;
		const Color opp = color_opp(me);
		const Square ksq = king_square(me);
		const Bitboard occ = occupied();
		const Bitboard own = m_pieces[piece_occupy(me)];

		const Bitboard opp_rq = m_pieces[piece_make(opp, ROOK)]   | m_pieces[piece_make(opp, QUEEN)];
		const Bitboard opp_bq = m_pieces[piece_make(opp, BISHOP)] | m_pieces[piece_make(opp, QUEEN)];

		Bitboard pinned = Bitboard::make_empty();

		if (opp_rq)
		{
			const Bitboard ray = rook_attacks(ksq, occ);
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

	struct Legality
	{
		Bitboard pinned;
		Square   ksq;
		bool     in_check;
	};
	NODISCARD FORCE_INLINE Legality legality_context() const
	{
		return { pinned_pieces(), king_square(m_turn), is_in_check() };
	}

	NODISCARD FORCE_INLINE bool is_pseudo_legal_move_legal(Move m, const Legality& ctx) const
	{
		if (!ctx.in_check && m.from() != ctx.ksq
		    && !(square_bb(m.from()) & ctx.pinned) && !m.is_ep_capture())
			return true;
		return is_pseudo_legal_move_legal(m);
	}

	// True iff the side that just moved did not leave its own king attacked.
	NODISCARD FORCE_INLINE bool is_legal() const
	{
		return !is_attacked_by(king_square(color_opp(m_turn)), m_turn);
	}

	// Carries no en-passant state — the caller must mirror its own ep square.
	NODISCARD Position mirror() const
	{
		Position swapped;
		swapped.clear();
		Bitboard occ = occupied();
		while (occ)
		{
			const Square sq = occ.pop_first_square();
			swapped.put_piece(piece_opp_color(piece_at(sq)), sq_rank_mirror(sq));
		}
		swapped.m_turn = color_opp(m_turn);
		swapped.m_castling_rooks = m_castling_rooks.mirror_ranks();
		return swapped;
	}

	NODISCARD Material_Key material_key() const
	{
		Material_Key k;
		for (const Color c : { WHITE, BLACK })
			for (int t = PAWN; t <= KING; ++t)
			{
				const Piece pc = piece_make(c, Piece_Type(t));
				for (size_t i = m_pieces[pc].num_set_bits(); i > 0; --i) k.add_piece(pc);
			}
		return k;
	}
};
static_assert(std::is_trivially_copyable_v<Position>);
