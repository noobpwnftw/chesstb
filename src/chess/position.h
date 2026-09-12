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
	Bitboard m_occupied;             // = m_pieces[WHITE_OCCUPY] | m_pieces[BLACK_OCCUPY]
	int8_t   m_piece_counts[PIECE_NB];
	Piece    m_squares[SQUARE_NB];
	Color    m_turn;
	uint8_t  m_castling;             // Castling_Rights bitmask; 0 for rights-free tables
	// Home square of each right's rook, by castling_right_index. Only the entries
	// whose bit is set in m_castling carry meaning.
	Square   m_castling_rook[CASTLING_RIGHT_NB];

	void clear()
	{
		std::memset(this, 0, sizeof(*this));
	}

	NODISCARD Color turn() const { return m_turn; }
	void set_turn(Color c) { m_turn = c; }

	NODISCARD uint8_t castling() const { return m_castling; }

	NODISCARD bool can_castle(Color c, bool h_side) const
	{
		return (m_castling & castling_right(c, h_side)) != 0;
	}

	NODISCARD Square castling_rook_square(Color c, bool h_side) const
	{
		ASSERT(can_castle(c, h_side));
		return m_castling_rook[castling_right_index(c, h_side)];
	}

	void set_castling_right(Color c, bool h_side, Square rook_sq)
	{
		m_castling |= castling_right(c, h_side);
		m_castling_rook[castling_right_index(c, h_side)] = rook_sq;
	}

	void copy_castling_from(const Position& other)
	{
		m_castling = other.m_castling;
		std::memcpy(m_castling_rook, other.m_castling_rook, sizeof(m_castling_rook));
	}

	NODISCARD bool castling_rights_are_consistent() const
	{
		for (const Color c : { WHITE, BLACK })
		{
			if (!(m_castling & castling_rights_of(c))) continue;
			const Square ksq = king_square(c);
			if (sq_rank(ksq) != castling_home_rank(c)) return false;
			for (const bool h_side : { true, false })
			{
				if (!can_castle(c, h_side)) continue;
				const Square r = m_castling_rook[castling_right_index(c, h_side)];
				if (m_squares[r] != piece_make(c, ROOK)) return false;
				if (sq_rank(r) != castling_home_rank(c)) return false;
				if (h_side ? (sq_file(r) <= sq_file(ksq)) : (sq_file(r) >= sq_file(ksq)))
					return false;
			}
		}
		return true;
	}

	void update_castling_rights(Square from, Square to)
	{
		for (const Color c : { WHITE, BLACK })
		{
			if (!(m_castling & castling_rights_of(c))) continue;
			if (from == king_square(c))
			{
				m_castling &= static_cast<uint8_t>(~castling_rights_of(c));
				continue;
			}
			for (const bool h_side : { true, false })
			{
				if (!can_castle(c, h_side)) continue;
				const Square r = m_castling_rook[castling_right_index(c, h_side)];
				if (r == from || r == to)
					m_castling &= static_cast<uint8_t>(~castling_right(c, h_side));
			}
		}
	}

	NODISCARD bool is_empty(Square sq) const { return m_squares[sq] == PIECE_NONE; }

	NODISCARD FORCE_INLINE bool move_is_capture(Move m) const
	{
		if (m.is_castling()) return false;
		return m.is_ep_capture() || m_squares[m.to()] != PIECE_NONE;
	}
	NODISCARD Piece piece_at(Square sq) const { return m_squares[sq]; }
	NODISCARD const Bitboard& occupied() const { return m_occupied; }
	NODISCARD const Bitboard& color_bb(Color c) const { return m_pieces[piece_occupy(c)]; }
	NODISCARD const Bitboard& piece_bb(Piece p) const { return m_pieces[p]; }

	NODISCARD FORCE_INLINE Square king_square(Color c) const
	{
		ASSERT(m_pieces[piece_make(c, KING)].num_set_bits() == 1);
		return m_pieces[piece_make(c, KING)].peek_first_square();
	}

	// Put / remove a piece, keeping bitboards, m_squares and counts in sync.
	FORCE_INLINE void put_piece(Piece pc, Square sq)
	{
		ASSERT(m_squares[sq] == PIECE_NONE);
		const Bitboard sb = square_bb(sq);
		m_pieces[pc] |= sb;
		m_pieces[piece_occupy(piece_color(pc))] |= sb;
		m_occupied |= sb;
		m_squares[sq] = pc;
		m_piece_counts[pc] += 1;
	}

	FORCE_INLINE void remove_piece(Square sq)
	{
		const Piece pc = m_squares[sq];
		ASSERT(pc != PIECE_NONE);
		const Bitboard sb = square_bb(sq);
		m_pieces[pc] ^= sb;
		m_pieces[piece_occupy(piece_color(pc))] ^= sb;
		m_occupied ^= sb;
		m_squares[sq] = PIECE_NONE;
		m_piece_counts[pc] -= 1;
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
		return attackers_to(sq, attacker_color, m_occupied) != Bitboard::make_empty();
	}

	NODISCARD FORCE_INLINE bool is_in_check() const
	{
		return is_attacked_by(king_square(m_turn), color_opp(m_turn));
	}

	// do_move returns the piece captured (PIECE_NONE if none) and folds the
	// castling-rights update in, so undo needs the pre-move mask handed back.
	Piece do_move(Move m);
	void  undo_move(Move m, Piece captured, uint8_t prev_castling);

	// No EP, no king-in-check filter. `f` returns true to stop; returns true iff it did.
	template <typename F>
	GEN_FORCE_INLINE bool visit_pseudo_legal_moves(F&& f) const
	{
		const Color me = m_turn;
		const Bitboard occ = m_occupied;
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

		if (m_castling & castling_rights_of(me))
		{
			for (const bool h_side : { true, false })
			{
				if (!can_castle(me, h_side)) continue;
				const Square rfrom = castling_rook_square(me, h_side);
				const Square kto = castling_king_to(me, h_side);
				const Square rto = castling_rook_to(me, h_side);
				const Bitboard path = (rank_span_bb(ksq, kto) | rank_span_bb(rfrom, rto))
				                    & ~(square_bb(ksq) | square_bb(rfrom));
				if (occ & path) continue;
				if (f(Move::make_castling(ksq, kto))) return true;
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
			if (m_squares[from] != piece_make(me, PAWN)) continue;

			const Square cap_sq = sq_make(pawn_rank, target_file);
			if (m_squares[cap_sq] != piece_make(color_opp(me), PAWN)) continue;

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
		const Bitboard occ = m_occupied;
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
			if (is_attacked_by(from, opp)) return false;
			if (to != from)
			{
				const int step = (to > from) ? 1 : -1;
				for (Square s = from + step; s != to; s = s + step)
					if (is_attacked_by(s, opp)) return false;
			}

			const bool h_side = sq_file(to) == FILE_G;
			const Square rook_from = castling_rook_square(m_turn, h_side);
			const Square rook_to   = castling_rook_to(m_turn, h_side);
			const Bitboard occ_castled =
				((m_occupied ^ square_bb(from)) ^ square_bb(rook_from))
				| square_bb(to) | square_bb(rook_to);
			return attackers_to(to, opp, occ_castled) == Bitboard::make_empty();
		}
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
		const Bitboard occ = m_occupied;
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

	static Position from_fen(const std::string& fen);
	void to_fen(Span<char> out) const;

	// Carries no en-passant state — the caller must mirror its own ep square.
	NODISCARD Position mirror() const
	{
		Position swapped;
		swapped.clear();
		Bitboard occ = m_occupied;
		while (occ)
		{
			const Square sq = occ.pop_first_square();
			swapped.put_piece(piece_opp_color(m_squares[sq]), sq_rank_mirror(sq));
		}
		swapped.m_turn = color_opp(m_turn);
		swapped.m_castling = castling_rights_color_swapped(m_castling);
		for (const Color c : { WHITE, BLACK })
			for (const bool h_side : { true, false })
				if (can_castle(c, h_side))
					swapped.m_castling_rook[castling_right_index(color_opp(c), h_side)] =
						sq_rank_mirror(m_castling_rook[castling_right_index(c, h_side)]);
		return swapped;
	}

	NODISCARD Material_Key material_key() const
	{
		Material_Key k;
		for (Piece pc : ALL_PIECES)
		{
			const size_t cnt = m_pieces[pc].num_set_bits();
			for (size_t i = 0; i < cnt; ++i) k.add_piece(pc);
		}
		return k;
	}
};
static_assert(std::is_trivially_copyable_v<Position>);
