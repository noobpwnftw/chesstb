#pragma once

#include <cstdint>

#include "util/defines.h"
#include "util/enum.h"
#include "util/span.h"

inline constexpr size_t MAX_MAN = 32;

enum Color : int8_t {
	WHITE, BLACK, COLOR_NB = 2
};

enum Piece_Type : int8_t {
	PIECE_TYPE_NONE = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING, PIECE_TYPE_NB = 8
};

enum Piece : int8_t {
	WHITE_OCCUPY, WHITE_PAWN, WHITE_KNIGHT, WHITE_BISHOP, WHITE_ROOK, WHITE_QUEEN, WHITE_KING,
	BLACK_OCCUPY = 8, BLACK_PAWN, BLACK_KNIGHT, BLACK_BISHOP, BLACK_ROOK, BLACK_QUEEN, BLACK_KING,
	PIECE_NONE = 0, PIECE_NB = 16
};

NODISCARD constexpr Piece piece_occupy(Color color)
{
	static_assert(WHITE_OCCUPY == (WHITE << 3));
	static_assert(BLACK_OCCUPY == (BLACK << 3));
	return static_cast<Piece>(color << 3);
}

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

struct Material_Key
{
private:
	static constexpr uint32_t MAT_KEY[PIECE_NB] = {
		0,        1,        9,        81,       729,      6561,       0,        0,
		0,        59049,    531441,   4782969,  43046721, 387420489u, 0,        0,
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

	constexpr void add_pair() { pair_flag = true; }

	NODISCARD constexpr bool has_pair() const { return pair_flag; }

	constexpr void add_castling(size_t white_rights, size_t black_rights)
	{
		ASSERT(white_rights <= 2 && black_rights <= 2);
		for (size_t i = 0; i < white_rights; ++i) add_piece(WHITE_ROOK);
		for (size_t i = 0; i < black_rights; ++i) add_piece(BLACK_ROOK);
		castling_code = static_cast<uint8_t>(white_rights * 3 + black_rights);
	}

	NODISCARD constexpr bool has_castling() const { return castling_code != 0; }

	NODISCARD constexpr uint8_t castling_index() const { return castling_code; }

	NODISCARD constexpr uint32_t value() const { return key; }

	NODISCARD constexpr friend bool operator==(Material_Key lhs, Material_Key rhs) noexcept { return lhs.key == rhs.key && lhs.pair_flag == rhs.pair_flag && lhs.castling_code == rhs.castling_code; }
	NODISCARD constexpr friend bool operator!=(Material_Key lhs, Material_Key rhs) noexcept { return !(lhs == rhs); }
	NODISCARD constexpr friend bool operator< (Material_Key lhs, Material_Key rhs) noexcept
	{
		if (lhs.key != rhs.key) return lhs.key < rhs.key;
		if (lhs.pair_flag != rhs.pair_flag) return !lhs.pair_flag;
		return lhs.castling_code < rhs.castling_code;
	}
	NODISCARD constexpr friend bool operator> (Material_Key lhs, Material_Key rhs) noexcept { return rhs < lhs; }
	NODISCARD constexpr friend bool operator<=(Material_Key lhs, Material_Key rhs) noexcept { return !(rhs < lhs); }
	NODISCARD constexpr friend bool operator>=(Material_Key lhs, Material_Key rhs) noexcept { return !(lhs < rhs); }

private:
	uint32_t key;
	bool pair_flag = false;
	uint8_t castling_code = 0;
};

struct Material_Key_Hash
{
	NODISCARD size_t operator()(Material_Key k) const noexcept
	{
		return (static_cast<size_t>(k.value()) * 1099511628211ull + k.has_pair()) * 9
		     + k.castling_index();
	}
};

NODISCARD constexpr Color color_opp(Color color)
{
	return static_cast<Color>(color ^ 1);
}

NODISCARD constexpr Color color_maybe_opp(Color color, bool opp)
{
	return static_cast<Color>(static_cast<int>(color) ^ static_cast<int>(opp));
}

NODISCARD constexpr Piece_Type piece_type(Piece piece)
{
	return static_cast<Piece_Type>(piece & 7);
}

NODISCARD constexpr size_t possible_sq_nb(Piece piece)
{
	return piece_type(piece) == PAWN ? PAWN_SQUARE_NB : SQUARE_NB;
}

NODISCARD constexpr Square possible_sq(Piece piece, size_t index)
{
	return static_cast<Square>((piece_type(piece) == PAWN ? SQ_A2 : SQ_A1) + index);
}

NODISCARD constexpr Color piece_color(Piece piece)
{
	ASSERT(piece != WHITE_OCCUPY && piece != BLACK_OCCUPY);
	return static_cast<Color>(piece >> 3);
}

NODISCARD constexpr Piece piece_make(Color color, Piece_Type type)
{
	return static_cast<Piece>((color << 3) + type);
}

NODISCARD constexpr Piece piece_opp_color(Piece piece)
{
	return piece_make(color_opp(piece_color(piece)), piece_type(piece));
}

NODISCARD constexpr File sq_file(Square sq)
{
	return static_cast<File>(sq & 7);
}

NODISCARD constexpr Rank sq_rank(Square sq)
{
	return static_cast<Rank>(sq >> 3);
}

NODISCARD constexpr Square sq_make(Rank rank, File file)
{
	return static_cast<Square>((rank << 3) + file);
}

NODISCARD constexpr Rank castling_home_rank(Color c)
{
	return c == WHITE ? RANK_1 : RANK_8;
}

// Both king and rook land on the same squares they would in standard chess,
// whatever files they started on: g1/f1 castling toward h, c1/d1 toward a.
NODISCARD constexpr Square castling_king_to(Color c, bool h_side)
{
	return sq_make(castling_home_rank(c), h_side ? FILE_G : FILE_C);
}

NODISCARD constexpr Square castling_rook_to(Color c, bool h_side)
{
	return sq_make(castling_home_rank(c), h_side ? FILE_F : FILE_D);
}

NODISCARD constexpr Square sq_rank_mirror(Square sq)
{
	return static_cast<Square>(sq ^ 56);
}

// Piece chars by Piece value (uppercase white, lowercase black; ? at the OCCUPY slots).
inline constexpr char PIECE_STRING[20] = "?PNBRQK??pnbrqk?";
