#pragma once

#include "chess/chess.h"

#include "util/defines.h"

// 16-bit packed move:
//   bits 0-5  : to-square
//   bits 6-11 : from-square
//   bits 12-13: promotion piece type - KNIGHT
//   bits 14-15: NORMAL, PROMOTION, EP_CAPTURE or CASTLING
// Castling is king takes own rook (from = king, to = rook).
struct Move
{
	enum Type : uint16_t { NORMAL = 0, PROMOTION = 1 << 14, EP_CAPTURE = 2 << 14, CASTLING = 3 << 14 };

	NODISCARD static constexpr Move make(Square from, Square to, Type type = NORMAL, Piece_Type promo = KNIGHT)
	{
		return Move(static_cast<uint16_t>(type | ((promo - KNIGHT) << 12) | (from << 6) | to));
	}

	NODISCARD static constexpr Move make_quiet(Square from, Square to) { return make(from, to); }
	NODISCARD static constexpr Move make_promotion(Square from, Square to, Piece_Type promo) { return make(from, to, PROMOTION, promo); }
	NODISCARD static constexpr Move make_castling(Square from, Square to) { return make(from, to, CASTLING); }
	NODISCARD static constexpr Move make_ep_capture(Square from, Square to) { return make(from, to, EP_CAPTURE); }

	Move() = default;
	explicit constexpr Move(uint16_t packed) : m_packed(packed) {}

	NODISCARD constexpr Square to() const { return static_cast<Square>(m_packed & 0x3f); }
	NODISCARD constexpr Square from() const { return static_cast<Square>((m_packed >> 6) & 0x3f); }
	NODISCARD constexpr Type type() const { return static_cast<Type>(m_packed & (3 << 14)); }
	NODISCARD constexpr Piece_Type promotion() const { return static_cast<Piece_Type>(((m_packed >> 12) & 3) + KNIGHT); }

	NODISCARD constexpr bool is_promotion() const { return type() == PROMOTION; }
	NODISCARD constexpr bool is_castling() const { return type() == CASTLING; }
	NODISCARD constexpr bool is_ep_capture() const { return type() == EP_CAPTURE; }

	NODISCARD constexpr bool operator==(Move o) const { return m_packed == o.m_packed; }
	NODISCARD constexpr bool operator!=(Move o) const { return m_packed != o.m_packed; }

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
