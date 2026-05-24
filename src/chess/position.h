#pragma once

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/attack.h"
#include "chess/move.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/span.h"

#include <array>
#include <cstdint>
#include <string>

// Chess position. Holds piece bitboards (indexed by Piece, with WHITE_OCCUPY /
// BLACK_OCCUPY slots doubling as color-occupancy aliases), a piece-per-square
// array, piece counts, and side to move.
//
// No castling state (TB assumption: kings have already moved).
// No en-passant state — handled externally at probe time as a virtual capture.
// No move history — undo_move requires the caller to pass back the captured piece.
struct Position
{
	Bitboard m_pieces[PIECE_NB];     // [WHITE_OCCUPY] = all white, [BLACK_OCCUPY] = all black
	Bitboard m_occupied;             // = m_pieces[WHITE_OCCUPY] | m_pieces[BLACK_OCCUPY]
	int8_t   m_piece_counts[PIECE_NB];
	Piece    m_squares[SQUARE_NB];
	Color    m_turn;

	// Zero everything.
	void clear()
	{
		for (auto& bb : m_pieces) bb = Bitboard::make_empty();
		m_occupied = Bitboard::make_empty();
		for (auto& c  : m_piece_counts) c = 0;
		for (auto& s  : m_squares) s = PIECE_NONE;
		m_turn = WHITE;
	}

	NODISCARD INLINE Color turn() const { return m_turn; }
	INLINE void set_turn(Color c) { m_turn = c; }

	NODISCARD INLINE bool is_empty(Square sq) const { return m_squares[sq] == PIECE_NONE; }
	NODISCARD INLINE Piece piece_at(Square sq) const { return m_squares[sq]; }
	NODISCARD INLINE const Bitboard& occupied() const { return m_occupied; }
	NODISCARD INLINE const Bitboard& color_bb(Color c) const { return m_pieces[piece_occupy(c)]; }
	NODISCARD INLINE const Bitboard& piece_bb(Piece p) const { return m_pieces[p]; }

	NODISCARD INLINE Square king_square(Color c) const
	{
		ASSERT(m_pieces[piece_make(c, KING)].num_set_bits() == 1);
		return m_pieces[piece_make(c, KING)].peek_first_square();
	}

	// Put / remove a piece (mutates bitboards, counts, squares; not the occupied alias).
	INLINE void put_piece(Piece pc, Square sq)
	{
		ASSERT(m_squares[sq] == PIECE_NONE);
		const Bitboard sb = square_bb(sq);
		m_pieces[pc] |= sb;
		m_pieces[piece_occupy(piece_color(pc))] |= sb;
		m_occupied |= sb;
		m_squares[sq] = pc;
		m_piece_counts[pc] += 1;
	}

	INLINE void remove_piece(Square sq)
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

	// Set of attackers of color `attacker_color` that hit `sq`, given the
	// stated occupancy. Useful for in-check detection and SEE-like queries.
	NODISCARD Bitboard attackers_to(Square sq, Color attacker_color, Bitboard occupied) const;

	NODISCARD INLINE bool is_attacked_by(Square sq, Color attacker_color) const
	{
		return attackers_to(sq, attacker_color, m_occupied) != Bitboard::make_empty();
	}

	NODISCARD INLINE bool is_in_check(Color side) const
	{
		return is_attacked_by(king_square(side), color_opp(side));
	}

	NODISCARD INLINE bool is_in_check() const
	{
		return is_in_check(m_turn);
	}

	// Apply / undo a (non-ep, non-castling) move. Promotion supported.
	// Returns the piece captured (or PIECE_NONE).
	Piece do_move(Move m);
	void  undo_move(Move m, Piece captured);

	// Move generator selector. No EP, no castling, no king-in-check filter.
	//   ALL         — every pseudo-legal move
	//   PAWN_PUSHES — non-promo pawn pushes (single + double) only
	enum class Move_Kind : uint8_t { ALL, PAWN_PUSHES };
	template <Move_Kind K>
	void gen_pseudo_legal_moves(Out_Param<Move_List> out) const;

	// Pre-edges (inverted: Move::from() = current square, Move::to() = source).
	// Generates moves of opp(m_turn). IncludePawnPushes=false for DTC
	// (pawn pushes are DTZ-zeroing, not same-material retrograde edges).
	template <bool IncludePawnPushes>
	void gen_pseudo_legal_pre_quiets(Out_Param<Move_List> out) const;

	NODISCARD bool is_pseudo_legal_move_legal(Move m) const;

	NODISCARD bool is_legal() const;  // standing check on the current state

	// FEN I/O. FEN parsing only reads the piece placement + side-to-move fields
	// and ignores castling / en passant / counters (TB positions don't carry them).
	static Position from_fen(const std::string& fen);
	void to_fen(Span<char> out) const;

	// Material signature of the pieces currently on the board (turn-agnostic).
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
