#pragma once

#include <array>
#include <cstdint>

#include "util/defines.h"
#include "util/enum.h"
#include "util/span.h"

constexpr size_t MAX_MAN = 32;

enum Color : int8_t {
	WHITE, BLACK, COLOR_NB = 2
};

enum Piece_Type : int8_t {
	PIECE_TYPE_NONE = 0, KING = 1, QUEEN, ROOK, BISHOP, KNIGHT, PAWN, PIECE_TYPE_NB = 8
};

// WHITE_OCCUPY and BLACK_OCCUPY are aliases for the color occupation bitboards in Position.
// They share index space with Piece so PIECE_NONE can be 0.
enum Piece : int8_t {
	WHITE_OCCUPY, WHITE_KING, WHITE_QUEEN, WHITE_ROOK, WHITE_BISHOP, WHITE_KNIGHT, WHITE_PAWN,
	BLACK_OCCUPY = 8, BLACK_KING, BLACK_QUEEN, BLACK_ROOK, BLACK_BISHOP, BLACK_KNIGHT, BLACK_PAWN,
	PIECE_NONE = 0, PIECE_NB = 16
};

constexpr inline Piece piece_occupy(Color color)
{
	static_assert(WHITE_OCCUPY == (WHITE << 3));
	static_assert(BLACK_OCCUPY == (BLACK << 3));
	return static_cast<Piece>(color << 3);
}

constexpr Piece ALL_PIECES[] = {
	WHITE_KING, WHITE_QUEEN, WHITE_ROOK, WHITE_BISHOP, WHITE_KNIGHT, WHITE_PAWN,
	BLACK_KING, BLACK_QUEEN, BLACK_ROOK, BLACK_BISHOP, BLACK_KNIGHT, BLACK_PAWN
};

// In chess every non-king piece can deliver mate; this list is used for
// piece-set filtering (we require at least one of these to make a position
// have a forced result other than draw).
constexpr Piece ALL_FREE_ATTACKING_PIECES[] = {
	WHITE_QUEEN, WHITE_ROOK, WHITE_BISHOP, WHITE_KNIGHT, WHITE_PAWN,
	BLACK_QUEEN, BLACK_ROOK, BLACK_BISHOP, BLACK_KNIGHT, BLACK_PAWN
};

// Squares ordered by rank-major: a1=0, b1=1, ..., h1=7, a2=8, ..., h8=63.
enum Square : int8_t {
	SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
	SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
	SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
	SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
	SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
	SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
	SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
	SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
	SQ_END, SQ_START = 0, SQUARE_NB = 64,
	// Number of squares a pawn can legally occupy (ranks 2..7, all 8 files).
	PAWN_SQUARE_NB = 48
};

ENUM_ENABLE_OPERATOR_INC(Square);
ENUM_ENABLE_OPERATOR_DEC(Square);
ENUM_ENABLE_OPERATOR_ADD(Square);
ENUM_ENABLE_OPERATOR_SUB(Square);
ENUM_ENABLE_OPERATOR_ADD_EQ(Square);
ENUM_ENABLE_OPERATOR_SUB_EQ(Square);
ENUM_ENABLE_OPERATOR_DIFF(Square);

enum File : int8_t {
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_END, FILE_START = 0, FILE_NB = 8
};

ENUM_ENABLE_OPERATOR_INC(File);
ENUM_ENABLE_OPERATOR_DEC(File);
ENUM_ENABLE_OPERATOR_ADD(File);
ENUM_ENABLE_OPERATOR_SUB(File);
ENUM_ENABLE_OPERATOR_ADD_EQ(File);
ENUM_ENABLE_OPERATOR_SUB_EQ(File);
ENUM_ENABLE_OPERATOR_DIFF(File);

enum Rank : int8_t {
	RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8,
	RANK_END, RANK_START = 0, RANK_NB = 8
};

ENUM_ENABLE_OPERATOR_INC(Rank);
ENUM_ENABLE_OPERATOR_DEC(Rank);
ENUM_ENABLE_OPERATOR_ADD(Rank);
ENUM_ENABLE_OPERATOR_SUB(Rank);
ENUM_ENABLE_OPERATOR_ADD_EQ(Rank);
ENUM_ENABLE_OPERATOR_SUB_EQ(Rank);
ENUM_ENABLE_OPERATOR_DIFF(Rank);

constexpr Rank SQ_RANK[SQUARE_NB] = {
	RANK_1, RANK_1, RANK_1, RANK_1, RANK_1, RANK_1, RANK_1, RANK_1,
	RANK_2, RANK_2, RANK_2, RANK_2, RANK_2, RANK_2, RANK_2, RANK_2,
	RANK_3, RANK_3, RANK_3, RANK_3, RANK_3, RANK_3, RANK_3, RANK_3,
	RANK_4, RANK_4, RANK_4, RANK_4, RANK_4, RANK_4, RANK_4, RANK_4,
	RANK_5, RANK_5, RANK_5, RANK_5, RANK_5, RANK_5, RANK_5, RANK_5,
	RANK_6, RANK_6, RANK_6, RANK_6, RANK_6, RANK_6, RANK_6, RANK_6,
	RANK_7, RANK_7, RANK_7, RANK_7, RANK_7, RANK_7, RANK_7, RANK_7,
	RANK_8, RANK_8, RANK_8, RANK_8, RANK_8, RANK_8, RANK_8, RANK_8,
};

constexpr File SQ_FILE[SQUARE_NB] = {
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
	FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H,
};

// Light/dark square color (a1 is dark by convention).
constexpr Color SQ_COLOR[SQUARE_NB] = {
	BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE,
	WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK,
	BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE,
	WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK,
	BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE,
	WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK,
	BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE,
	WHITE, BLACK, WHITE, BLACK, WHITE, BLACK, WHITE, BLACK,
};

constexpr Square SQ_FILE_MIRROR[SQUARE_NB] = {
	SQ_H1, SQ_G1, SQ_F1, SQ_E1, SQ_D1, SQ_C1, SQ_B1, SQ_A1,
	SQ_H2, SQ_G2, SQ_F2, SQ_E2, SQ_D2, SQ_C2, SQ_B2, SQ_A2,
	SQ_H3, SQ_G3, SQ_F3, SQ_E3, SQ_D3, SQ_C3, SQ_B3, SQ_A3,
	SQ_H4, SQ_G4, SQ_F4, SQ_E4, SQ_D4, SQ_C4, SQ_B4, SQ_A4,
	SQ_H5, SQ_G5, SQ_F5, SQ_E5, SQ_D5, SQ_C5, SQ_B5, SQ_A5,
	SQ_H6, SQ_G6, SQ_F6, SQ_E6, SQ_D6, SQ_C6, SQ_B6, SQ_A6,
	SQ_H7, SQ_G7, SQ_F7, SQ_E7, SQ_D7, SQ_C7, SQ_B7, SQ_A7,
	SQ_H8, SQ_G8, SQ_F8, SQ_E8, SQ_D8, SQ_C8, SQ_B8, SQ_A8,
};

constexpr Square SQ_RANK_MIRROR[SQUARE_NB] = {
	SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
	SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
	SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
	SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
	SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
	SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
	SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
	SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
};

// Diagonal mirror (transpose): a1-h8 axis. sq with file f, rank r → square with file r, rank f.
constexpr Square SQ_DIAG_MIRROR[SQUARE_NB] = {
	SQ_A1, SQ_A2, SQ_A3, SQ_A4, SQ_A5, SQ_A6, SQ_A7, SQ_A8,
	SQ_B1, SQ_B2, SQ_B3, SQ_B4, SQ_B5, SQ_B6, SQ_B7, SQ_B8,
	SQ_C1, SQ_C2, SQ_C3, SQ_C4, SQ_C5, SQ_C6, SQ_C7, SQ_C8,
	SQ_D1, SQ_D2, SQ_D3, SQ_D4, SQ_D5, SQ_D6, SQ_D7, SQ_D8,
	SQ_E1, SQ_E2, SQ_E3, SQ_E4, SQ_E5, SQ_E6, SQ_E7, SQ_E8,
	SQ_F1, SQ_F2, SQ_F3, SQ_F4, SQ_F5, SQ_F6, SQ_F7, SQ_F8,
	SQ_G1, SQ_G2, SQ_G3, SQ_G4, SQ_G5, SQ_G6, SQ_G7, SQ_G8,
	SQ_H1, SQ_H2, SQ_H3, SQ_H4, SQ_H5, SQ_H6, SQ_H7, SQ_H8,
};

constexpr int8_t PAWN_MOVE_INC[COLOR_NB] = { 8, -8 };

constexpr char START_FEN[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w";

constexpr size_t MAX_FEN_LENGTH = 120;

// A unique 32-bit signature of the pieces present on the board.
// Assumes piece counts are bounded by 8 of each non-king type.
// Two kings (one white, one black) are assumed to be present.
struct Material_Key
{
private:
	// Base-9 mixed-radix per piece type. Piece counts up to 8 each are
	// representable; counts above 8 (theoretically possible via promotions)
	// would overflow into the next slot — TBs we care about don't exceed 8.
	// White slot bases come first (smaller weights) so that
	// `min(base, mirr)` produces the canonical form by side count comparison.
	static constexpr uint32_t MAT_KEY[PIECE_NB] = {
		0,        0,        6561,     729,      81,       9,        1,        0,
		0,        0,        387420489u, 43046721, 4782969,  531441,   59049,    0,
	};

public:
	constexpr Material_Key() :
		key(0)
	{
	}

	explicit constexpr Material_Key(uint32_t k) :
		key(k)
	{
	}

	constexpr void add_piece(Piece pc)
	{
		key += MAT_KEY[pc];
	}

	NODISCARD constexpr uint32_t value() const
	{
		return key;
	}

	NODISCARD constexpr friend bool operator==(Material_Key lhs, Material_Key rhs) noexcept { return lhs.key == rhs.key; }
	NODISCARD constexpr friend bool operator!=(Material_Key lhs, Material_Key rhs) noexcept { return lhs.key != rhs.key; }
	NODISCARD constexpr friend bool operator< (Material_Key lhs, Material_Key rhs) noexcept { return lhs.key <  rhs.key; }
	NODISCARD constexpr friend bool operator> (Material_Key lhs, Material_Key rhs) noexcept { return lhs.key >  rhs.key; }
	NODISCARD constexpr friend bool operator<=(Material_Key lhs, Material_Key rhs) noexcept { return lhs.key <= rhs.key; }
	NODISCARD constexpr friend bool operator>=(Material_Key lhs, Material_Key rhs) noexcept { return lhs.key >= rhs.key; }

private:
	uint32_t key;
};

// Number of squares a given piece can legally occupy.
// All pieces except pawns: 64. Pawns: 48 (ranks 2..7).
constexpr int8_t PIECE_POSSIBLE_SQUARE_NB[PIECE_NB] = {
	0,                   // WHITE_OCCUPY
	(int8_t)SQUARE_NB,   // WHITE_KING
	(int8_t)SQUARE_NB,   // WHITE_QUEEN
	(int8_t)SQUARE_NB,   // WHITE_ROOK
	(int8_t)SQUARE_NB,   // WHITE_BISHOP
	(int8_t)SQUARE_NB,   // WHITE_KNIGHT
	(int8_t)PAWN_SQUARE_NB, // WHITE_PAWN
	0,                   // unused
	0,                   // BLACK_OCCUPY
	(int8_t)SQUARE_NB,   // BLACK_KING
	(int8_t)SQUARE_NB,   // BLACK_QUEEN
	(int8_t)SQUARE_NB,   // BLACK_ROOK
	(int8_t)SQUARE_NB,   // BLACK_BISHOP
	(int8_t)SQUARE_NB,   // BLACK_KNIGHT
	(int8_t)PAWN_SQUARE_NB, // BLACK_PAWN
	0,                   // unused
};

NODISCARD INLINE constexpr auto possible_sq_nb(Piece piece)
{
	return PIECE_POSSIBLE_SQUARE_NB[piece];
}

// For pieces that live on all 64 squares the possible-square index is just the square index.
// For pawns, square = sq - SQ_A2; only valid for sq in ranks 2..7.
NODISCARD INLINE constexpr int possible_sq_index(Piece piece, Square sq)
{
	const Piece_Type pt = static_cast<Piece_Type>(piece & 7);
	if (pt == PAWN)
		return static_cast<int>(sq) - static_cast<int>(SQ_A2);
	return static_cast<int>(sq);
}

NODISCARD INLINE constexpr Square possible_sq(Piece piece, size_t index)
{
	const Piece_Type pt = static_cast<Piece_Type>(piece & 7);
	if (pt == PAWN)
		return static_cast<Square>(static_cast<int>(SQ_A2) + static_cast<int>(index));
	return static_cast<Square>(index);
}

NODISCARD INLINE constexpr auto pawn_move_inc(Color color)
{
	return PAWN_MOVE_INC[color];
}

NODISCARD INLINE constexpr bool color_is_ok(Color color)
{
	return (color == WHITE || color == BLACK);
}

NODISCARD INLINE constexpr Color color_opp(Color color)
{
	ASSERT(color_is_ok(color));
	return static_cast<Color>(color ^ 1);
}

NODISCARD INLINE constexpr Color color_maybe_opp(Color color, bool opp)
{
	ASSERT(color_is_ok(color));
	return static_cast<Color>(static_cast<int>(color) ^ static_cast<int>(opp));
}

NODISCARD INLINE constexpr bool piece_is_ok(Piece piece)
{
	return ((piece >= WHITE_KING && piece <= WHITE_PAWN) || (piece >= BLACK_KING && piece <= BLACK_PAWN));
}

NODISCARD INLINE constexpr bool piece_type_is_ok(Piece_Type piece)
{
	return piece >= KING && piece <= PAWN;
}

NODISCARD INLINE constexpr Piece_Type piece_type(Piece piece)
{
	return static_cast<Piece_Type>(piece & 7);
}

NODISCARD INLINE constexpr Color piece_color(Piece piece)
{
	ASSERT(piece != WHITE_OCCUPY && piece != BLACK_OCCUPY);
	return static_cast<Color>(piece >> 3);
}

NODISCARD INLINE constexpr Piece piece_make(Color color, Piece_Type type)
{
	ASSERT(color_is_ok(color));
	ASSERT(piece_type_is_ok(type));
	return static_cast<Piece>((color << 3) + type);
}

NODISCARD INLINE constexpr Piece piece_opp_color(Piece piece)
{
	return piece_make(color_opp(piece_color(piece)), piece_type(piece));
}

NODISCARD constexpr bool sq_is_ok(Square sq)
{
	return sq >= SQ_A1 && sq < SQ_END;
}

NODISCARD constexpr bool file_is_ok(File file)
{
	return file >= FILE_A && file < FILE_END;
}

NODISCARD constexpr bool rank_is_ok(Rank rank)
{
	return rank >= RANK_1 && rank < RANK_END;
}

NODISCARD INLINE constexpr File sq_file(Square sq)
{
	ASSERT(sq_is_ok(sq));
	return SQ_FILE[sq];
}

NODISCARD INLINE constexpr Rank sq_rank(Square sq)
{
	ASSERT(sq_is_ok(sq));
	return SQ_RANK[sq];
}

NODISCARD INLINE constexpr Square sq_make(Rank rank, File file)
{
	return static_cast<Square>((rank << 3) + file);
}

NODISCARD INLINE constexpr bool sq_equal_rank(Square sq1, Square sq2)
{
	return SQ_RANK[sq1] == SQ_RANK[sq2];
}

NODISCARD INLINE constexpr bool sq_equal_file(Square sq1, Square sq2)
{
	return SQ_FILE[sq1] == SQ_FILE[sq2];
}

NODISCARD INLINE constexpr Color sq_color(Square sq)
{
	ASSERT(sq >= 0 && sq < SQUARE_NB);
	return SQ_COLOR[sq];
}

NODISCARD INLINE constexpr Square sq_file_mirror(Square sq)
{
	return SQ_FILE_MIRROR[sq];
}

NODISCARD INLINE constexpr Square sq_rank_mirror(Square sq)
{
	return SQ_RANK_MIRROR[sq];
}

NODISCARD INLINE constexpr Square sq_diag_mirror(Square sq)
{
	return SQ_DIAG_MIRROR[sq];
}

// In chess every non-king piece is a free attacker (can deliver checkmate
// against a bare king, possibly in cooperation with own king).
// We keep the name for symmetry with the xiangqi codebase.
NODISCARD INLINE constexpr bool is_piece_free_attacker(Piece piece)
{
	const Piece_Type type = piece_type(piece);
	return type == QUEEN
		|| type == ROOK
		|| type == BISHOP
		|| type == KNIGHT
		|| type == PAWN;
}

// Piece char encoding: KQRBNP (uppercase white, lowercase black).
// Index by Piece enum value: 0..6 are white slots (incl WHITE_OCCUPY at 0),
// 8..14 are black slots (incl BLACK_OCCUPY at 8).
constexpr char PIECE_STRING[20] = "?KQRBNP??kqrbnp?";
constexpr std::array<Piece, 128> PIECE_FROM_CHAR = [](){
	std::array<Piece, 128> arr{};
	arr['K'] = WHITE_KING;
	arr['Q'] = WHITE_QUEEN;
	arr['R'] = WHITE_ROOK;
	arr['B'] = WHITE_BISHOP;
	arr['N'] = WHITE_KNIGHT;
	arr['P'] = WHITE_PAWN;
	arr['k'] = BLACK_KING;
	arr['q'] = BLACK_QUEEN;
	arr['r'] = BLACK_ROOK;
	arr['b'] = BLACK_BISHOP;
	arr['n'] = BLACK_KNIGHT;
	arr['p'] = BLACK_PAWN;
	return arr;
}();

NODISCARD INLINE constexpr File file_from_char(char c)
{
	return static_cast<File>(c - 'a');
}

NODISCARD INLINE constexpr Rank rank_from_char(char c)
{
	return static_cast<Rank>(c - '1');
}

NODISCARD INLINE constexpr char file_to_char(File file)
{
	return static_cast<char>('a' + (file - FILE_A));
}

NODISCARD INLINE constexpr char rank_to_char(Rank rank)
{
	return static_cast<char>('1' + (rank - RANK_1));
}

NODISCARD INLINE constexpr Piece piece_from_char(char c)
{
	if (c >= 0)
		return PIECE_FROM_CHAR[c];
	return PIECE_NONE;
}

NODISCARD INLINE constexpr char piece_to_char(Piece p)
{
	ASSERT(p >= 0 && p < PIECE_NB);
	return PIECE_STRING[p];
}

NODISCARD INLINE constexpr char piece_type_to_char(Piece_Type p)
{
	ASSERT(p >= 0 && p < PIECE_TYPE_NB);
	return PIECE_STRING[p];
}

INLINE constexpr void square_to_string(Square sq, char string[])
{
	ASSERT(sq_is_ok(sq));
	string[0] = file_to_char(sq_file(sq));
	string[1] = rank_to_char(sq_rank(sq));
	string[2] = '\0';
}

NODISCARD INLINE constexpr Square square_from_string(const char string[])
{
	if (string[0] < 'a' || string[0] > 'h')
		return SQ_END;
	if (string[1] < '1' || string[1] > '8')
		return SQ_END;
	if (string[2] != '\0')
		return SQ_END;

	const File file = file_from_char(string[0]);
	const Rank rank = rank_from_char(string[1]);

	return sq_make(rank, file);
}
