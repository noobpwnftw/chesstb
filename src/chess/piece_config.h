#pragma once

#include "chess.h"

#include "util/defines.h"
#include "util/span.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// One class per piece type per color.
enum Piece_Type_Class : int8_t {
	KINGS, KNIGHTS, BISHOPS, ROOKS, QUEENS, PAWNS, PIECE_TYPE_CLASS_NB = 6
};

enum Piece_Class : int8_t {
	BLACK_KINGS, BLACK_KNIGHTS, BLACK_BISHOPS, BLACK_ROOKS, BLACK_QUEENS, BLACK_PAWNS,
	WHITE_KINGS, WHITE_KNIGHTS, WHITE_BISHOPS, WHITE_ROOKS, WHITE_QUEENS, WHITE_PAWNS,
	PIECE_CLASS_START = 0, PIECE_CLASS_END = 12, PIECE_CLASS_NONE = -1, PIECE_CLASS_NB = 12
};

constexpr Piece_Class& operator++(Piece_Class& p_class)
{
	p_class = static_cast<Piece_Class>(static_cast<int>(p_class) + 1);
	return p_class;
}

NODISCARD constexpr Piece_Class make_piece_class(Color color, Piece_Type_Class pt_class)
{
	ASSERT(pt_class < PIECE_TYPE_CLASS_NB);
	return static_cast<Piece_Class>((color == WHITE ? WHITE_KINGS : BLACK_KINGS) + pt_class);
}

inline constexpr std::array<Piece_Class, PIECE_NB> PIECE_TO_PIECE_CLASS = []() {
	std::array<Piece_Class, PIECE_NB> arr{};
	for (auto& v : arr) v = PIECE_CLASS_NONE;
	arr[BLACK_KING]   = BLACK_KINGS;
	arr[BLACK_QUEEN]  = BLACK_QUEENS;
	arr[BLACK_ROOK]   = BLACK_ROOKS;
	arr[BLACK_BISHOP] = BLACK_BISHOPS;
	arr[BLACK_KNIGHT] = BLACK_KNIGHTS;
	arr[BLACK_PAWN]   = BLACK_PAWNS;
	arr[WHITE_KING]   = WHITE_KINGS;
	arr[WHITE_QUEEN]  = WHITE_QUEENS;
	arr[WHITE_ROOK]   = WHITE_ROOKS;
	arr[WHITE_BISHOP] = WHITE_BISHOPS;
	arr[WHITE_KNIGHT] = WHITE_KNIGHTS;
	arr[WHITE_PAWN]   = WHITE_PAWNS;
	return arr;
}();

NODISCARD constexpr Piece_Class piece_class(Piece p)
{
	return PIECE_TO_PIECE_CLASS[p];
}

struct Piece_Config
{
	// Uppercase letters are ordinary piece types (color set by side ordering).
	// Lowercase 'p' is an opposing pawn pair: one white + one black pawn
	// locked head-to-head, indexed jointly. At most one 'p' per material.
	// Lowercase 'r' is a rook that still holds a castling right, together with
	// the king it castles with: KrK is physically KRK, with the right indexed
	// rather than lost. Two rights per side at most. See "Castling rights" in
	// the README.
	static constexpr char VALID_PIECES[] = "KQRBNPpr";

	static constexpr std::array<int16_t, PIECE_NB> PIECE_STRENGTH_FOR_SIDE_ORDER = []() {
		std::array<int16_t, PIECE_NB> arr{};
		// Kings present on both sides — value irrelevant for side ordering.
		arr[WHITE_KING]   = arr[BLACK_KING]   = 0;
		arr[WHITE_QUEEN]  = arr[BLACK_QUEEN]  = 900;
		arr[WHITE_ROOK]   = arr[BLACK_ROOK]   = 500;
		arr[WHITE_BISHOP] = arr[BLACK_BISHOP] = 330;
		arr[WHITE_KNIGHT] = arr[BLACK_KNIGHT] = 320;
		arr[WHITE_PAWN]   = arr[BLACK_PAWN]   = 100;
		return arr;
	}();

	// Sort order within a side: K, Q, R, B, N, P (descending strength, kings first).
	static constexpr std::array<int8_t, PIECE_NB> PIECE_ORDER = []() {
		std::array<int8_t, PIECE_NB> ret{};
		ret[WHITE_OCCUPY] = 0;
		ret[BLACK_OCCUPY] = 0;
		int8_t i = 1;
		constexpr Color AllColors[] = { WHITE, BLACK };
		for (const Color color : AllColors)
		{
			ret[piece_make(color, KING)]   = i++;
			ret[piece_make(color, QUEEN)]  = i++;
			ret[piece_make(color, ROOK)]   = i++;
			ret[piece_make(color, BISHOP)] = i++;
			ret[piece_make(color, KNIGHT)] = i++;
			ret[piece_make(color, PAWN)]   = i++;
		}
		return ret;
	}();

	static bool sort_pieces(Span<Piece> pieces,
	                        const std::array<size_t, COLOR_NB>& castling_rights = { 0, 0 });

	NODISCARD static bool is_constructible_from(const std::string& name)
	{
		if (name.empty() || name.size() > MAX_MAN)
			return false;

		if (name[0] != 'K')
			return false;

		if (std::count(name.begin(), name.end(), 'K') != 2)
			return false;

		if (name.find_first_not_of(VALID_PIECES) != std::string::npos)
			return false;

		const auto num_r = std::count(name.begin(), name.end(), 'r');
		if (num_r > 4)
			return false;
		if (num_r > 0)
		{
			size_t kings = 0, second_k = std::string::npos;
			for (size_t i = 0; i < name.size(); ++i)
				if (name[i] == 'K' && ++kings == 2) { second_k = i; break; }
			const auto r_white = std::count(name.begin(), name.begin() + second_k, 'r');
			if (r_white > 2 || num_r - r_white > 2)
				return false;
		}

		// The single opposing pair is written as one 'p' on each side (e.g. KQpKp)
		// to make it obvious both colors hold a pawn. So there are either zero
		// 'p's or exactly two -- one before and one after the second king.
		const auto num_p = std::count(name.begin(), name.end(), 'p');
		if (num_p != 0 && num_p != 2)
			return false;
		if (num_p == 2)
		{
			size_t kings = 0, second_k = std::string::npos;
			for (size_t i = 0; i < name.size(); ++i)
				if (name[i] == 'K' && ++kings == 2) { second_k = i; break; }
			const auto p_white = std::count(name.begin(), name.begin() + second_k, 'p');
			if (p_white != 1)
				return false;
		}

		return true;
	}

	NODISCARD static bool is_constructible_from(Const_Span<Piece> pieces)
	{
		if (pieces.size() < 2 || pieces.size() > MAX_MAN)
			return false;

		if (std::count(pieces.begin(), pieces.end(), WHITE_KING) != 1)
			return false;

		if (std::count(pieces.begin(), pieces.end(), BLACK_KING) != 1)
			return false;

		return true;
	}

	Piece_Config(const std::string& s) :
		m_num_pieces(0)
	{
		if (!is_constructible_from(s))
			throw std::runtime_error("Invalid PieceConfig: " + s);

		bool is_black = false;
		for (const char c : s)
		{
			if (c == 'p')
			{
				m_has_opposing_pair = true;
				continue;
			}

			if (c == 'r')
			{
				m_castling_rights[is_black ? BLACK : WHITE] += 1;
				continue;
			}

			const Piece_Type pt = piece_type(piece_from_char(c));
			if (m_num_pieces > 0 && pt == KING)
				is_black = true;

			const Piece p = piece_make(is_black ? BLACK : WHITE, pt);
			m_pieces[m_num_pieces++] = p;
		}

		if (sort_pieces(Span(m_pieces, m_num_pieces), m_castling_rights) && has_castling())
			std::swap(m_castling_rights[WHITE], m_castling_rights[BLACK]);

		for (const Piece p : Const_Span(m_pieces, m_pieces + m_num_pieces))
		{
			m_base_mat_key.add_piece(p);
			m_mirr_mat_key.add_piece(piece_opp_color(p));
		}
		if (m_has_opposing_pair)
		{
			m_base_mat_key.add_pair();
			m_mirr_mat_key.add_pair();
		}
		if (has_castling())
		{
			m_base_mat_key.add_castling(m_castling_rights[WHITE], m_castling_rights[BLACK]);
			m_mirr_mat_key.add_castling(m_castling_rights[BLACK], m_castling_rights[WHITE]);
		}
	}

	using Castling_Rights_Counts = std::array<size_t, COLOR_NB>;
	static constexpr Castling_Rights_Counts NO_CASTLE_RIGHTS{ 0, 0 };

	Piece_Config(Const_Span<Piece> pcs,
	             const Castling_Rights_Counts& castling_rights = NO_CASTLE_RIGHTS) :
		m_num_pieces(0)
	{
		if (!is_constructible_from(pcs))
			throw std::runtime_error("Invalid PieceConfig.");

		std::memcpy(m_pieces, pcs.data(), pcs.size() * sizeof(Piece));

		const bool swapped = sort_pieces(Span(m_pieces, pcs.size()), castling_rights);

		m_castling_rights = castling_rights;
		if (swapped)
			std::swap(m_castling_rights[WHITE], m_castling_rights[BLACK]);

		for (const Piece p : Const_Span(m_pieces, m_pieces + pcs.size()))
		{
			m_base_mat_key.add_piece(p);
			m_mirr_mat_key.add_piece(piece_opp_color(p));
		}
		if (has_castling())
		{
			m_base_mat_key.add_castling(m_castling_rights[WHITE], m_castling_rights[BLACK]);
			m_mirr_mat_key.add_castling(m_castling_rights[BLACK], m_castling_rights[WHITE]);
		}
		m_num_pieces = pcs.size();
	}

	NODISCARD auto pieces() const
	{
		return Const_Span(m_pieces, m_pieces + m_num_pieces);
	}

	NODISCARD std::string name() const
	{
		std::string s;
		bool seen_white_king = false;
		// Castling rooks are not in m_pieces, so they go in where they would have
		// sorted: before the first man of their color that outranks a rook.
		std::array<size_t, COLOR_NB> written{ 0, 0 };
		auto write_castling_rooks_before = [&](int8_t order) {
			for (const Color c : { WHITE, BLACK })
			{
				if (written[c] == m_castling_rights[c]) continue;
				if (PIECE_ORDER[piece_make(c, ROOK)] >= order) continue;
				s.append(m_castling_rights[c] - written[c], 'r');
				written[c] = m_castling_rights[c];
			}
		};
		for (size_t i = 0; i < m_num_pieces; ++i)
		{
			const Piece p = m_pieces[i];
			const Piece_Type pt = piece_type(p);
			write_castling_rooks_before(PIECE_ORDER[p]);
			if (pt == KING)
			{
				if (seen_white_king && m_has_opposing_pair)
					s += 'p';
				seen_white_king = true;
			}
			s += piece_type_to_char(pt);
		}
		write_castling_rooks_before(std::numeric_limits<int8_t>::max());
		if (m_has_opposing_pair)
			s += 'p';
		return s;
	}

	NODISCARD const std::array<size_t, PIECE_NB> piece_counts() const
	{
		std::array<size_t, PIECE_NB> counts;
		std::fill(counts.begin(), counts.end(), 0);
		for (const Piece piece : pieces())
			counts[piece] += 1;
		return counts;
	}

	NODISCARD bool operator==(const Piece_Config& other) const
	{
		return m_num_pieces == other.m_num_pieces
			&& m_has_opposing_pair == other.m_has_opposing_pair
			&& m_castling_rights == other.m_castling_rights
			&& std::equal(m_pieces, m_pieces + m_num_pieces, other.m_pieces);
	}

	NODISCARD size_t num_pieces() const
	{
		return m_num_pieces;
	}

	NODISCARD bool has_opposing_pair() const
	{
		return m_has_opposing_pair;
	}

	NODISCARD bool has_castling() const
	{
		return m_castling_rights[WHITE] + m_castling_rights[BLACK] > 0;
	}
	NODISCARD size_t castling_rights(Color c) const { return m_castling_rights[c]; }
	NODISCARD const Castling_Rights_Counts& castling_rights() const { return m_castling_rights; }
	NODISCARD size_t total_castling_rights() const
	{
		return m_castling_rights[WHITE] + m_castling_rights[BLACK];
	}

	NODISCARD bool is_bare_kings() const
	{
		return m_num_pieces <= 2 && !m_has_opposing_pair && !has_castling();
	}

	NODISCARD bool has_pawns() const
	{
		if (m_has_opposing_pair)
			return true;
		for (const Piece p : pieces())
			if (piece_type(p) == PAWN)
				return true;
		return false;
	}

	NODISCARD Material_Key base_material_key() const
	{
		return m_base_mat_key;
	}

	NODISCARD std::pair<Material_Key, Material_Key> material_keys() const
	{
		return { m_base_mat_key, m_mirr_mat_key };
	}

	NODISCARD Material_Key min_material_key() const
	{
		return std::min(m_base_mat_key, m_mirr_mat_key);
	}

	void mark_opposing_pair()
	{
		m_has_opposing_pair = true;
		m_base_mat_key.add_pair();
		m_mirr_mat_key.add_pair();
	}

	void mark_castling(const Castling_Rights_Counts& rights)
	{
		ASSERT(!has_castling());
		m_castling_rights = rights;
		m_base_mat_key.add_castling(rights[WHITE], rights[BLACK]);
		m_mirr_mat_key.add_castling(rights[BLACK], rights[WHITE]);
	}

private:
	Piece m_pieces[MAX_MAN];
	size_t m_num_pieces;
	bool m_has_opposing_pair = false;
	// Castling rights per color, whose rooks are not in m_pieces.
	Castling_Rights_Counts m_castling_rights{ 0, 0 };
	Material_Key m_base_mat_key;
	Material_Key m_mirr_mat_key;
};
