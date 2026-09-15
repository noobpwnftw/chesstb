#pragma once

// Reverse (retrograde) move generation: for a position, every position from
// which one legal move reaches it.
//
// Soundness holds by construction. Candidates are over-generated and each one is
// kept only if forward generation finds a legal move from it to the target, so
// every predecessor returned is a real one. Completeness is what can go wrong,
// and test_retro establishes it by the bijection test: over whole move trees,
// every forward move must be recovered from the position it reaches.
//
// Identity is the normalized FEN (placement, side to move, castling rights and
// an en passant square only where a legal en passant capture exists). Move
// counters are not part of it, since one un-move cannot determine them.
//
// The cases that make a predecessor set incomplete while it still looks right:
//   1. Castling rights. A predecessor may hold rights its successor lost. A
//      capture on a rook's home square removes the opponent's right, and
//      castling gives up both of the mover's rights at once.
//   2. En passant. A side may decline an available en passant capture, so the
//      predecessor carries an en passant square its successor does not.
//   3. Chess960 castling, where the king and rook can start on many files and
//      either one may already stand on its destination square.

#include "board.h"

#include <cstdint>
#include <vector>

namespace ref {

struct Retro_Options
{
	// Uncastle from any king and rook files. Without it, only the e-file king
	// and the corner rooks of standard chess.
	bool chess960 = false;
	Validity validity = Validity::TABLE;
};

struct Predecessor
{
	Board board;   // en passant normalized
	Move move;     // a legal move in `board` that reaches the target
};

// Every distinct predecessor of `target`, each validated by forward generation.
std::vector<Predecessor> predecessors(const Board& target, const Retro_Options& opt = {});

// Predecessor positions `depth` un-moves back, counted along every path, the
// retrograde analogue of perft. Depth 0 is 1.
uint64_t unmake_perft(const Board& target, int depth, const Retro_Options& opt = {});

namespace detail {
inline constexpr int KNIGHT_STEPS[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 }, { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
inline constexpr int KING_STEPS[8][2]   = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 } };
inline constexpr int SLIDER_DIRS[8][2]  = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
}  // namespace detail

// Quiet un-moves for scheduling retrograde work: a knight, bishop, rook, queen
// or king of the side that just moved stepping back to an empty square. No
// uncaptures, pawns, promotions or castling, and nothing is validated; callers
// decide what a candidate means. `f(now, before)` receives the square the man
// stands on and the square it came from.
template <typename F>
void for_each_quiet_unmove(const Board& b, F&& f)
{
	const Color mover = opposite(b.stm);
	for (int sq = 0; sq < 64; ++sq)
	{
		const Piece p = b.squares[sq];
		if (p == NO_PIECE || color_of(p) != mover) continue;
		const Piece_Type t = type_of(p);
		if (t == PAWN) continue;
		const int f0 = file_of(sq), r0 = rank_of(sq);
		if (t == KNIGHT || t == KING)
		{
			const auto* steps = t == KNIGHT ? detail::KNIGHT_STEPS : detail::KING_STEPS;
			for (int i = 0; i < 8; ++i)
			{
				const int nf = f0 + steps[i][0], nr = r0 + steps[i][1];
				if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
				const int to = make_square(nf, nr);
				if (b.squares[to] == NO_PIECE) f(sq, to);
			}
			continue;
		}
		const int first = t == ROOK ? 4 : 0;
		const int last = t == BISHOP ? 4 : 8;
		for (int d = first; d < last; ++d)
		{
			int nf = f0 + detail::SLIDER_DIRS[d][0], nr = r0 + detail::SLIDER_DIRS[d][1];
			while (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
			{
				const int to = make_square(nf, nr);
				if (b.squares[to] != NO_PIECE) break;
				f(sq, to);
				nf += detail::SLIDER_DIRS[d][0];
				nr += detail::SLIDER_DIRS[d][1];
			}
		}
	}
}

}  // namespace ref
