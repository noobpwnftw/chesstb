#include "chess/notation.h"

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/span.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#if defined(CHESSTB_NOTATION)

namespace
{
	Piece piece_from_char(char c)
	{
		for (int p = 0; p < PIECE_NB; ++p)
			if (c != '?' && PIECE_STRING[p] == c) return static_cast<Piece>(p);
		return PIECE_NONE;
	}
}

Position position_from_fen(const std::string& fen)
{
	Position p;
	p.clear();

	auto bad = [&](const char* what) {
		throw std::runtime_error(std::string("Invalid FEN, ") + what + ": " + fen);
	};

	int idx = 0;
	int rank = 7, file = 0;
	for (; idx < (int)fen.size(); ++idx)
	{
		const char c = fen[idx];
		if (c == ' ') break;
		if (c == '/')
		{
			if (file != 8) bad("rank is not 8 files");
			if (rank == 0) bad("more than 8 ranks");
			--rank; file = 0;
			continue;
		}
		if (c >= '1' && c <= '8')
		{
			file += c - '0';
			if (file > 8) bad("rank is not 8 files");
			continue;
		}
		const Piece pc = piece_from_char(c);
		if (pc == PIECE_NONE)
			throw std::runtime_error(std::string("Invalid FEN char: ") + c);
		if (file == 8) bad("rank is not 8 files");
		p.put_piece(pc, sq_make(static_cast<Rank>(rank), static_cast<File>(file)));
		++file;
	}
	if (rank != 0 || file != 8) bad("board is not 8 ranks");

	// Side to move (one char after the space), WHITE when the field is absent.
	while (idx < (int)fen.size() && fen[idx] == ' ') ++idx;
	if (idx < (int)fen.size())
	{
		const char stm = fen[idx];
		if (stm != 'w' && stm != 'W' && stm != 'b' && stm != 'B') bad("side to move");
		p.m_turn = (stm == 'b' || stm == 'B') ? BLACK : WHITE;
		++idx;
	}

	auto outer_rook = [&](Color c, bool h_side) -> Square {
		const Square ksq = p.king_square(c);
		const Rank rank = castling_home_rank(c);
		Square found = SQ_END;
		for (File f = FILE_A; f < FILE_END; ++f)
		{
			const Square sq = sq_make(rank, f);
			if (!p.m_pieces[piece_make(c, ROOK)].has_square(sq)) continue;
			if (h_side ? (f <= sq_file(ksq)) : (f >= sq_file(ksq))) continue;
			// h-side wants the largest such file, a-side the smallest.
			if (found == SQ_END || h_side) found = sq;
		}
		return found;
	};

	while (idx < (int)fen.size() && fen[idx] == ' ') ++idx;
	for (; idx < (int)fen.size() && fen[idx] != ' '; ++idx)
	{
		const char c = fen[idx];
		if (c == '-') continue;

		Square rook_sq;
		if (c == 'K' || c == 'Q' || c == 'k' || c == 'q')
			rook_sq = outer_rook((c == 'K' || c == 'Q') ? WHITE : BLACK, c == 'K' || c == 'k');
		else if ((c >= 'A' && c <= 'H') || (c >= 'a' && c <= 'h'))
		{
			const Color color = (c >= 'A' && c <= 'H') ? WHITE : BLACK;
			const File f = static_cast<File>((c >= 'A' && c <= 'H') ? c - 'A' : c - 'a');
			rook_sq = sq_make(castling_home_rank(color), f);
		}
		else
		{
			bad("castling field");
			return p;
		}
		if (rook_sq == SQ_END) bad("castling right names no rook");
		p.m_castling_rooks |= rook_sq;
	}

	return p;
}

void position_to_fen(const Position& pos, Span<char> out)
{
	size_t w = 0;
	auto put = [&](char c) { if (w + 1 < out.size()) out[w++] = c; };
	for (int rank = 7; rank >= 0; --rank)
	{
		int run = 0;
		for (int file = 0; file < 8; ++file)
		{
			const Piece pc = pos.piece_at(sq_make(static_cast<Rank>(rank), static_cast<File>(file)));
			if (pc == PIECE_NONE)
			{
				++run;
			}
			else
			{
				if (run) put(char('0' + run));
				run = 0;
				put(PIECE_STRING[pc]);
			}
		}
		if (run) put(char('0' + run));
		if (rank > 0) put('/');
	}
	put(' ');
	put(pos.turn() == WHITE ? 'w' : 'b');
	// Only rights-bearing positions grow the third field. The outermost rook on
	// its side gets a KQkq letter; anything further in needs its file spelled.
	if (pos.castling())
	{
		put(' ');
		for (const Color c : { WHITE, BLACK })
			for (Bitboard rooks = pos.castling_rooks(c); rooks;)
			{
				const Square rook_sq = rooks.pop_last_square();
				const bool h_side = rook_sq > pos.king_square(c);
				const Rank rank = castling_home_rank(c);
				bool outermost = true;
				for (File f = FILE_A; f < FILE_END && outermost; ++f)
					if (pos.piece_bb(piece_make(c, ROOK)).has_square(sq_make(rank, f))
					    && (h_side ? f > sq_file(rook_sq) : f < sq_file(rook_sq)))
						outermost = false;
				char letter;
				if (outermost) letter = h_side ? 'K' : 'Q';
				else           letter = static_cast<char>('A' + sq_file(rook_sq));
				put(c == WHITE ? letter : static_cast<char>(letter - 'A' + 'a'));
			}
	}
	if (w < out.size()) out[w] = '\0';
}

void move_to_string(Move m, char out[])
{
	square_to_string(m.from(), out);
	square_to_string(m.to(), out + 2);
	if (m.is_promotion())
	{
		out[4] = "nbrq"[m.promotion() - KNIGHT];
		out[5] = '\0';
	}
}

Move move_from_string(const Position& pos, const std::string& s)
{
	auto bad = [&]() { throw std::runtime_error("Invalid move: " + s); };
	if (s.size() != 4 && s.size() != 5) bad();
	const Square from = square_from_string(s.substr(0, 2).c_str());
	const Square to   = square_from_string(s.substr(2, 2).c_str());
	if (from == SQ_END || to == SQ_END) bad();

	if (s.size() == 5)
	{
		const char* const promos = "nbrq";
		const char* const p = std::find(promos, promos + 4, s[4]);
		if (p == promos + 4) bad();
		return Move::make_promotion(from, to, static_cast<Piece_Type>(KNIGHT + (p - promos)));
	}

	const Color us = pos.turn();
	if (pos.piece_bb(piece_make(us, KING)).has_square(from)
	    && pos.castling_rooks(us).has_square(to))
		return Move::make_castling(from, to);
	if (pos.piece_bb(piece_make(us, PAWN)).has_square(from)
	    && sq_file(from) != sq_file(to) && pos.is_empty(to))
		return Move::make_ep_capture(from, to);
	return Move::make_quiet(from, to);
}

bool is_piece_config_name(const std::string& name)
{
	if (name.empty() || name.size() > MAX_MAN)
		return false;

	if (name[0] != 'K')
		return false;

	if (std::count(name.begin(), name.end(), 'K') != 2)
		return false;

	if (name.find_first_not_of("KQRBNPpr") != std::string::npos)
		return false;

	// Castling rooks and the opposing pair's pawns are counted per side.
	const size_t second_k = name.find('K', 1);
	auto per_side = [&](char c) {
		const auto white = std::count(name.begin(), name.begin() + second_k, c);
		return std::make_pair(white, std::count(name.begin(), name.end(), c) - white);
	};

	const auto [r_white, r_black] = per_side('r');
	if (r_white > 2 || r_black > 2)
		return false;

	// The single opposing pair is written as one 'p' on each side (e.g. KQpKp)
	// to make it obvious both colors hold a pawn.
	const auto [p_white, p_black] = per_side('p');
	if (p_white != p_black || p_white > 1)
		return false;

	return true;
}

Piece_Config piece_config_from_name(const std::string& name)
{
	if (!is_piece_config_name(name))
		throw std::runtime_error("Invalid PieceConfig: " + name);

	Piece pieces[MAX_MAN];
	size_t n = 0;
	Piece_Config::Castling_Rights_Counts rights{ 0, 0 };
	Color side = WHITE;
	for (const char c : name)
	{
		if (c == 'p') continue;
		if (c == 'r') { ++rights[side]; continue; }
		const Piece_Type pt = piece_type(piece_from_char(c));
		if (n > 0 && pt == KING) side = BLACK;
		pieces[n++] = piece_make(side, pt);
	}

	Piece_Config ps(Const_Span<Piece>(pieces, n), rights);
	if (name.find('p') != std::string::npos) ps.mark_opposing_pair();
	return ps;
}

#endif
