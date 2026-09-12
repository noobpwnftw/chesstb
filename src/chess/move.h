#pragma once

#include "chess/chess.h"

#include "util/defines.h"

// 16-bit packed move:
//   bits 0-5  : to-square         (0..63)
//   bits 6-11 : from-square       (0..63)
//   bits 12-14: promotion type    (0 = none, else Piece_Type — QUEEN/ROOK/BISHOP/KNIGHT)
//               KING in that field is not a promotion; it tags a castling move
//               (from = king home square, to = g1/c1 and rank-8 counterparts).
//   bit  15   : ep-capture flag   (set on the special en-passant capture)
struct Move
{
	NODISCARD static constexpr Move make_null()
	{
		return Move(static_cast<uint16_t>(0));
	}

	NODISCARD static constexpr Move make_quiet(Square from, Square to)
	{
		return Move(static_cast<uint16_t>((from << 6) | to));
	}

	NODISCARD static constexpr Move make_promotion(Square from, Square to, Piece_Type promo)
	{
		ASSERT(promo == QUEEN || promo == ROOK || promo == BISHOP || promo == KNIGHT);
		return Move(static_cast<uint16_t>((from << 6) | to | (promo << 12)));
	}

	// King-move encoding (e1g1 / e1c1); the rook is implied by `to`.
	NODISCARD static constexpr Move make_castling(Square from, Square to)
	{
		return Move(static_cast<uint16_t>((from << 6) | to | (KING << 12)));
	}

	NODISCARD static constexpr Move make_ep_capture(Square from, Square to)
	{
		return Move(static_cast<uint16_t>((from << 6) | to | (1u << 15)));
	}

	NODISCARD static Move make_from_string(const char* s);

	Move() = default;
	explicit constexpr Move(uint16_t packed) : m_packed(packed) {}

	NODISCARD constexpr Square to() const { return static_cast<Square>(m_packed & 0x3f); }
	NODISCARD constexpr Square from() const { return static_cast<Square>((m_packed >> 6) & 0x3f); }
	NODISCARD constexpr Piece_Type promotion() const { return static_cast<Piece_Type>((m_packed >> 12) & 0x7); }

	NODISCARD constexpr bool is_promotion() const { return promotion() > KING; }
	NODISCARD constexpr bool is_castling() const { return promotion() == KING; }
	NODISCARD constexpr bool is_ep_capture() const { return (m_packed & (1u << 15)) != 0; }
	NODISCARD constexpr bool is_null() const { return m_packed == 0; }

	NODISCARD constexpr bool operator==(Move o) const { return m_packed == o.m_packed; }
	NODISCARD constexpr bool operator!=(Move o) const { return m_packed != o.m_packed; }

	void to_string(char out[]) const;

private:
	uint16_t m_packed;
};
static_assert(sizeof(Move) == 2);

NODISCARD constexpr bool is_pawn_double_push(Move m)
{
	const int dr = static_cast<int>(sq_rank(m.to())) - static_cast<int>(sq_rank(m.from()));
	return dr == 2 || dr == -2;
}

NODISCARD constexpr Square ep_square_of_double_push(Move m)
{
	const int from_rank = static_cast<int>(sq_rank(m.from()));
	const int to_rank   = static_cast<int>(sq_rank(m.to()));
	return sq_make(static_cast<Rank>((from_rank + to_rank) / 2), sq_file(m.from()));
}

enum struct Move_Legality_Lower_Bound
{
	NONE,
	PSEUDO_LEGAL,
	LEGAL
};

extern void move_display(Move move);
