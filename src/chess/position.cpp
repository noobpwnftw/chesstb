#include "chess/position.h"

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/attack.h"
#include "chess/move.h"

#include "util/defines.h"

#include <stdexcept>
#include <string>

Piece Position::do_move(Move m)
{
	const Square from = m.from();
	const Square to   = m.to();
	const Piece mover = m_squares[from];
	ASSERT(mover != PIECE_NONE);
	ASSERT(piece_color(mover) == m_turn);

	const Color us   = m_turn;
	const Color them = color_opp(us);
	Piece captured = PIECE_NONE;

	const bool castling = m.is_castling();
	const bool h_side = castling && sq_file(to) == FILE_G;
	const Square rook_from = castling ? castling_rook_square(us, h_side) : SQ_END;

	if (m_castling) update_castling_rights(from, to);

	if (castling)
	{
		const Square rook_to = castling_rook_to(us, h_side);
		const Piece rook = piece_make(us, ROOK);
		ASSERT(m_squares[from] == piece_make(us, KING));
		ASSERT(m_squares[rook_from] == rook);
		const Bitboard king_ft = square_bb(from) ^ square_bb(to);
		const Bitboard rook_ft = square_bb(rook_from) ^ square_bb(rook_to);
		m_pieces[mover]            ^= king_ft;
		m_pieces[rook]             ^= rook_ft;
		m_pieces[piece_occupy(us)] ^= king_ft ^ rook_ft;
		m_occupied                 ^= king_ft ^ rook_ft;
		m_squares[from]      = PIECE_NONE;
		m_squares[rook_from] = PIECE_NONE;
		m_squares[to]        = mover;
		m_squares[rook_to]   = rook;
	}
	else if (m.is_promotion())
	{
		if (!is_empty(to))
		{
			captured = m_squares[to];
			ASSERT(piece_color(captured) == them);
			remove_piece(to);
		}
		remove_piece(from);
		put_piece(piece_make(us, m.promotion()), to);
	}
	else if (m.is_ep_capture())
	{
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		captured = m_squares[cap_sq];
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		m_occupied ^= from_to ^ cap_bb;
		m_piece_counts[captured] -= 1;
		m_squares[to]     = mover;
		m_squares[from]   = PIECE_NONE;
		m_squares[cap_sq] = PIECE_NONE;
	}
	else if (!is_empty(to))
	{
		captured = m_squares[to];
		ASSERT(piece_color(captured) == them);
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		m_occupied ^= square_bb(from);
		m_piece_counts[captured] -= 1;
		m_squares[to]   = mover;
		m_squares[from] = PIECE_NONE;
	}
	else
	{
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		m_occupied                 ^= from_to;
		m_squares[to]   = mover;
		m_squares[from] = PIECE_NONE;
	}

	m_turn = them;
	return captured;
}

void Position::undo_move(Move m, Piece captured, uint8_t prev_castling)
{
	m_turn = color_opp(m_turn);
	const Square from = m.from();
	const Square to   = m.to();
	const Color us   = m_turn;
	const Color them = color_opp(us);

	m_castling = prev_castling;

	if (m.is_castling())
	{
		const bool h_side = sq_file(to) == FILE_G;
		const Square rook_from = castling_rook_square(us, h_side);
		const Square rook_to   = castling_rook_to(us, h_side);
		const Piece king = piece_make(us, KING);
		const Piece rook = piece_make(us, ROOK);
		const Bitboard king_ft = square_bb(from) ^ square_bb(to);
		const Bitboard rook_ft = square_bb(rook_from) ^ square_bb(rook_to);
		m_pieces[king]             ^= king_ft;
		m_pieces[rook]             ^= rook_ft;
		m_pieces[piece_occupy(us)] ^= king_ft ^ rook_ft;
		m_occupied                 ^= king_ft ^ rook_ft;
		m_squares[to]        = PIECE_NONE;
		m_squares[rook_to]   = PIECE_NONE;
		m_squares[from]      = king;
		m_squares[rook_from] = rook;
	}
	else if (m.is_promotion())
	{
		remove_piece(to);
		put_piece(piece_make(us, PAWN), from);
		if (captured != PIECE_NONE)
			put_piece(captured, to);
	}
	else if (m.is_ep_capture())
	{
		ASSERT(captured != PIECE_NONE);
		ASSERT(piece_type(captured) == PAWN);
		const Piece mover = m_squares[to];
		const Square cap_sq = sq_make(sq_rank(from), sq_file(to));
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard cap_bb  = square_bb(cap_sq);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= cap_bb;
		m_pieces[piece_occupy(them)] ^= cap_bb;
		m_occupied ^= from_to ^ cap_bb;
		m_piece_counts[captured] += 1;
		m_squares[from]   = mover;
		m_squares[to]     = PIECE_NONE;
		m_squares[cap_sq] = captured;
	}
	else if (captured != PIECE_NONE)
	{
		const Piece mover = m_squares[to];
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		const Bitboard to_bb   = square_bb(to);
		m_pieces[mover]              ^= from_to;
		m_pieces[piece_occupy(us)]   ^= from_to;
		m_pieces[captured]           ^= to_bb;
		m_pieces[piece_occupy(them)] ^= to_bb;
		m_occupied ^= square_bb(from);
		m_piece_counts[captured] += 1;
		m_squares[from] = mover;
		m_squares[to]   = captured;
	}
	else
	{
		const Piece mover = m_squares[to];
		const Bitboard from_to = square_bb(from) ^ square_bb(to);
		m_pieces[mover]            ^= from_to;
		m_pieces[piece_occupy(us)] ^= from_to;
		m_occupied                 ^= from_to;
		m_squares[from] = mover;
		m_squares[to]   = PIECE_NONE;
	}
}

Position Position::from_fen(const std::string& fen)
{
	Position p;
	p.clear();

	// Shape only: piece counts, kings and pawn ranks are the caller's to judge.
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
			if (p.m_squares[sq] != piece_make(c, ROOK)) continue;
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

		Color color;
		bool h_side;
		Square rook_sq;
		if (c == 'K' || c == 'Q' || c == 'k' || c == 'q')
		{
			color  = (c == 'K' || c == 'Q') ? WHITE : BLACK;
			h_side = (c == 'K' || c == 'k');
			rook_sq = outer_rook(color, h_side);
		}
		else if ((c >= 'A' && c <= 'H') || (c >= 'a' && c <= 'h'))
		{
			color = (c >= 'A' && c <= 'H') ? WHITE : BLACK;
			const File f = static_cast<File>((c >= 'A' && c <= 'H') ? c - 'A' : c - 'a');
			rook_sq = sq_make(castling_home_rank(color), f);
			h_side = f > sq_file(p.king_square(color));
		}
		else
		{
			bad("castling field");
			return p;
		}
		if (rook_sq == SQ_END) bad("castling right names no rook");
		p.set_castling_right(color, h_side, rook_sq);
	}
	if (!p.castling_rights_are_consistent()) bad("castling rights without king and rook at home");

	return p;
}

void Position::to_fen(Span<char> out) const
{
	size_t w = 0;
	auto put = [&](char c) { if (w + 1 < out.size()) out[w++] = c; };
	for (int rank = 7; rank >= 0; --rank)
	{
		int run = 0;
		for (int file = 0; file < 8; ++file)
		{
			const Piece pc = m_squares[sq_make(static_cast<Rank>(rank), static_cast<File>(file))];
			if (pc == PIECE_NONE)
			{
				++run;
			}
			else
			{
				if (run) put(char('0' + run));
				run = 0;
				put(piece_to_char(pc));
			}
		}
		if (run) put(char('0' + run));
		if (rank > 0) put('/');
	}
	put(' ');
	put(m_turn == WHITE ? 'w' : 'b');
	// Only rights-bearing positions grow the third field. The outermost rook on
	// its side gets a KQkq letter; anything further in needs its file spelled.
	if (m_castling != NO_CASTLING)
	{
		put(' ');
		for (const Color c : { WHITE, BLACK })
			for (const bool h_side : { true, false })
			{
				if (!can_castle(c, h_side)) continue;
				const Square rook_sq = castling_rook_square(c, h_side);
				const Rank rank = castling_home_rank(c);
				bool outermost = true;
				for (File f = FILE_A; f < FILE_END && outermost; ++f)
					if (m_squares[sq_make(rank, f)] == piece_make(c, ROOK)
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
