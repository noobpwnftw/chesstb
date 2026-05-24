#pragma once

#include "chess/chess.h"

#include "util/defines.h"

#include <algorithm>
#include <cstdint>

// 16-bit packed move:
//   bits 0-5  : to-square         (0..63)
//   bits 6-11 : from-square       (0..63)
//   bits 12-14: promotion type    (0 = none, else Piece_Type — QUEEN/ROOK/BISHOP/KNIGHT)
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

	NODISCARD constexpr bool is_promotion() const { return promotion() != PIECE_TYPE_NONE; }
	NODISCARD constexpr bool is_ep_capture() const { return (m_packed & (1u << 15)) != 0; }
	NODISCARD constexpr bool is_null() const { return m_packed == 0; }

	NODISCARD constexpr bool operator==(Move o) const { return m_packed == o.m_packed; }
	NODISCARD constexpr bool operator!=(Move o) const { return m_packed != o.m_packed; }

	void to_string(char out[]) const;

private:
	uint16_t m_packed;
};
static_assert(sizeof(Move) == 2);

// Square-only predicate: returns true iff the move spans exactly two ranks.
// Only meaningful for pawn pushes (the caller already knows the moving piece);
// for non-pawn moves it has no chess interpretation.
NODISCARD INLINE constexpr bool is_pawn_double_push(Move m)
{
	const int dr = static_cast<int>(sq_rank(m.to())) - static_cast<int>(sq_rank(m.from()));
	return dr == 2 || dr == -2;
}

// Stack-allocated move list. Chess max pseudo-legal moves in a single position
// is bounded by ~218 (theoretical), but the realistic cap for TB-relevant
// positions is well under 100. We keep CAPACITY at 218 to be safe.
struct Move_List
{
	static constexpr size_t CAPACITY = 218;

	Move_List() : m_size(0) {}

	INLINE void clear() { m_size = 0; }

	INLINE void add(Move move)
	{
		ASSERT(m_size < CAPACITY);
		m_moves[m_size++] = move;
	}

	INLINE void pop_last()
	{
		ASSERT(m_size > 0);
		m_size -= 1;
	}

	INLINE void swap_with_last_and_pop(size_t idx)
	{
		ASSERT(idx < m_size);
		std::swap(m_moves[idx], m_moves[--m_size]);
	}

	NODISCARD INLINE size_t size() const { return m_size; }
	NODISCARD INLINE bool   empty() const { return m_size == 0; }
	NODISCARD INLINE Move   operator[](size_t pos) const { return m_moves[pos]; }
	NODISCARD INLINE const Move* begin() const { return m_moves; }
	NODISCARD INLINE       Move* begin()       { return m_moves; }
	NODISCARD INLINE const Move* end()   const { return m_moves + m_size; }
	NODISCARD INLINE       Move* end()         { return m_moves + m_size; }

	template <typename F>
	INLINE void remove_if(F&& func)
	{
		const auto new_end = std::remove_if(begin(), end(), std::forward<F>(func));
		m_size = static_cast<uint16_t>(std::distance(begin(), new_end));
	}

private:
	uint16_t m_size;
	Move m_moves[CAPACITY];
};

enum struct Move_Legality_Lower_Bound
{
	NONE,
	PSEUDO_LEGAL,
	LEGAL
};

extern void move_display(Move move);
