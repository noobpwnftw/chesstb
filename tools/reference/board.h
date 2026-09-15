#pragma once

// An independent chess board for the reference tools: move generation, legality,
// FEN and Chess960 castling. It shares no code with src/chess on purpose, so a
// defect in one cannot hide the same defect in the other. Nothing here is fast;
// everything here should be easy to check by reading.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ref {

enum Color : uint8_t { WHITE = 0, BLACK = 1 };

inline Color opposite(Color c) { return static_cast<Color>(c ^ 1); }

enum Piece_Type : uint8_t { NO_TYPE = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

// 0 is an empty square; otherwise type | color << 3.
using Piece = uint8_t;
inline constexpr Piece NO_PIECE = 0;

inline Piece make_piece(Color c, Piece_Type t) { return static_cast<Piece>(t | (c << 3)); }
inline Piece_Type type_of(Piece p) { return static_cast<Piece_Type>(p & 7); }
inline Color color_of(Piece p) { return static_cast<Color>(p >> 3); }

inline constexpr int NO_SQUARE = -1;

inline int file_of(int sq) { return sq & 7; }
inline int rank_of(int sq) { return sq >> 3; }
inline int make_square(int file, int rank) { return rank * 8 + file; }
inline int home_rank(Color c) { return c == WHITE ? 0 : 7; }

// Castling sides, indexing Board::castle.
inline constexpr int A_SIDE = 0;
inline constexpr int H_SIDE = 1;

enum Move_Flag : uint8_t
{
	MF_CAPTURE     = 1,
	MF_EP          = 2,
	MF_CASTLE      = 4,
	MF_DOUBLE_PUSH = 8,
};

struct Move
{
	int8_t from = 0;
	int8_t to = 0;                  // castling: the king's destination square
	Piece_Type promotion = NO_TYPE;
	uint8_t flags = 0;
	Piece captured = NO_PIECE;
	int8_t rook_from = NO_SQUARE;   // castling only

	bool is_capture() const { return (flags & MF_CAPTURE) != 0; }
	bool is_ep() const { return (flags & MF_EP) != 0; }
	bool is_castle() const { return (flags & MF_CASTLE) != 0; }
	bool is_double_push() const { return (flags & MF_DOUBLE_PUSH) != 0; }

	// UCI text; castling is written as the king's move, e1g1.
	std::string uci() const;
};

// How strict a validity check is.
//   TABLE   what a tablebase index holds: one king per side, no pawn on a back
//           rank, the side not to move not in check, castling rights with their
//           king and rook at home, and an en passant square only where a double
//           push could have left one.
//   STRICT  TABLE, plus the limits python-chess applies in Board.is_valid for
//           standard chess: at most 16 men and 8 pawns per side, at most two
//           checkers, and no check that no single previous move could give.
enum struct Validity { TABLE, STRICT };

inline constexpr int MAX_MOVES = 256;

struct Board
{
	std::array<Piece, 64> squares{};
	Color stm = WHITE;
	int8_t ep = NO_SQUARE;
	// Castling rights as rook squares: castle[c][A_SIDE] toward the a-file,
	// castle[c][H_SIDE] toward the h-file, NO_SQUARE where the right is absent.
	std::array<std::array<int8_t, 2>, 2> castle{ { { NO_SQUARE, NO_SQUARE }, { NO_SQUARE, NO_SQUARE } } };

	void clear();
	void put(int sq, Piece p) { squares[sq] = p; }
	Piece at(int sq) const { return squares[sq]; }

	bool has_rights() const;
	bool has_rights(Color c) const;

	// The square of `c`'s king, NO_SQUARE unless there is exactly one.
	int king_square(Color c) const;

	uint64_t occupancy() const;

	// Bitboard of `by`'s men attacking `sq`. Sliders stop at the first square set
	// in `occ`; the men themselves are read from the board. With occ equal to the
	// board's own occupancy this is the ordinary attack set.
	uint64_t attackers(int sq, Color by, uint64_t occ) const;
	bool attacked(int sq, Color by) const { return attackers(sq, by, occupancy()) != 0; }
	bool in_check(Color c) const;

	// Legal moves for the side to move, including en passant when ep is set.
	// Returns the number written; `out` must hold MAX_MOVES.
	int legal_moves(Move* out) const;
	std::vector<Move> legal_moves() const;

	// Apply a move produced by legal_moves. Castling rights follow the usual
	// rule: a king move drops both of its rights, and a move from or to a
	// right's rook square drops that right, whichever side it belongs to.
	void make(const Move& m);

	bool has_legal_ep() const;
	void normalize_ep() { if (!has_legal_ep()) ep = NO_SQUARE; }

	bool is_checkmate() const;
	bool is_stalemate() const;

	// Four-field FEN: placement, side, castling, en passant. The outermost rook
	// on a side is written KQkq, an inner one by its file letter. The en passant
	// square is written only when a legal en passant capture exists.
	std::string fen() const;

	// Accepts KQkq and Shredder file letters, and ignores any move counters.
	static bool from_fen(const std::string& fen, Board& out, std::string* error = nullptr);

	// Position identity for retrograde work: the normalized FEN.
	std::string key() const { return fen(); }

	bool operator==(const Board& o) const
	{
		return squares == o.squares && stm == o.stm && ep == o.ep && castle == o.castle;
	}

private:
	int pseudo_legal_moves(Move* out) const;
	bool castling_is_legal(const Move& m) const;
	bool leaves_king_safe(const Move& m) const;
};

// Castling rights are consistent when each right's king and rook stand on the
// home rank with the rook on its own side of the king. With `standard`, the
// king must also be on the e-file and the rook in the corner.
bool castling_consistent(const Board& b, bool standard);

bool is_valid(const Board& b, Validity v, bool chess960);

// Squares a pawn's double push could have left as the en passant square, given
// who is to move: the pawn stands on its fourth rank, and the two squares behind
// it are empty. The capture itself is not checked.
bool ep_square_plausible(const Board& b, int ep_sq);

}  // namespace ref
