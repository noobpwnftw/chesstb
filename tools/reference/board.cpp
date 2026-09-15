#include "board.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace ref {

namespace {

constexpr int KNIGHT_STEPS[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 }, { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
constexpr int KING_STEPS[8][2]   = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 } };
constexpr int DIAGONALS[4][2]    = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
constexpr int ORTHOGONALS[4][2]  = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

inline bool on_board(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
inline uint64_t bit(int sq) { return uint64_t{ 1 } << sq; }

constexpr char PIECE_LETTERS[] = " PNBRQK";

char piece_char(Piece p)
{
	const char c = PIECE_LETTERS[type_of(p)];
	return color_of(p) == WHITE ? c : static_cast<char>(c - 'A' + 'a');
}

Piece piece_from_char(char c)
{
	const Color color = (c >= 'a' && c <= 'z') ? BLACK : WHITE;
	switch (c | 0x20)
	{
		case 'p': return make_piece(color, PAWN);
		case 'n': return make_piece(color, KNIGHT);
		case 'b': return make_piece(color, BISHOP);
		case 'r': return make_piece(color, ROOK);
		case 'q': return make_piece(color, QUEEN);
		case 'k': return make_piece(color, KING);
		default:  return NO_PIECE;
	}
}

std::string square_name(int sq)
{
	return { static_cast<char>('a' + file_of(sq)), static_cast<char>('1' + rank_of(sq)) };
}

}  // namespace

std::string Move::uci() const
{
	std::string s = square_name(from) + square_name(to);
	if (promotion != NO_TYPE)
		s += static_cast<char>(PIECE_LETTERS[promotion] - 'A' + 'a');
	return s;
}

void Board::clear()
{
	squares.fill(NO_PIECE);
	stm = WHITE;
	ep = NO_SQUARE;
	for (auto& side : castle) side = { NO_SQUARE, NO_SQUARE };
}

bool Board::has_rights(Color c) const
{
	return castle[c][A_SIDE] != NO_SQUARE || castle[c][H_SIDE] != NO_SQUARE;
}

bool Board::has_rights() const
{
	return has_rights(WHITE) || has_rights(BLACK);
}

int Board::king_square(Color c) const
{
	int found = NO_SQUARE;
	for (int sq = 0; sq < 64; ++sq)
	{
		if (squares[sq] != make_piece(c, KING)) continue;
		if (found != NO_SQUARE) return NO_SQUARE;
		found = sq;
	}
	return found;
}

uint64_t Board::occupancy() const
{
	uint64_t occ = 0;
	for (int sq = 0; sq < 64; ++sq)
		if (squares[sq] != NO_PIECE) occ |= bit(sq);
	return occ;
}

uint64_t Board::attackers(int sq, Color by, uint64_t occ) const
{
	uint64_t result = 0;
	const int f = file_of(sq), r = rank_of(sq);

	for (const auto& s : KNIGHT_STEPS)
	{
		const int nf = f + s[0], nr = r + s[1];
		if (on_board(nf, nr) && squares[make_square(nf, nr)] == make_piece(by, KNIGHT))
			result |= bit(make_square(nf, nr));
	}
	for (const auto& s : KING_STEPS)
	{
		const int nf = f + s[0], nr = r + s[1];
		if (on_board(nf, nr) && squares[make_square(nf, nr)] == make_piece(by, KING))
			result |= bit(make_square(nf, nr));
	}

	// A white pawn attacks sq from one rank below; a black pawn from one above.
	const int pr = r + (by == WHITE ? -1 : 1);
	for (int df : { -1, 1 })
		if (on_board(f + df, pr) && squares[make_square(f + df, pr)] == make_piece(by, PAWN))
			result |= bit(make_square(f + df, pr));

	auto rays = [&](const int dirs[4][2], Piece_Type slider) {
		for (int d = 0; d < 4; ++d)
		{
			int nf = f + dirs[d][0], nr = r + dirs[d][1];
			while (on_board(nf, nr))
			{
				const int s = make_square(nf, nr);
				const Piece p = squares[s];
				if (p != NO_PIECE && color_of(p) == by && (type_of(p) == slider || type_of(p) == QUEEN))
					result |= bit(s);
				if (occ & bit(s)) break;
				nf += dirs[d][0];
				nr += dirs[d][1];
			}
		}
	};
	rays(DIAGONALS, BISHOP);
	rays(ORTHOGONALS, ROOK);
	return result;
}

bool Board::in_check(Color c) const
{
	const int k = king_square(c);
	return k != NO_SQUARE && attacked(k, opposite(c));
}

int Board::pseudo_legal_moves(Move* out) const
{
	int n = 0;
	const Color us = stm, them = opposite(stm);

	auto add = [&](int from, int to, uint8_t flags = 0, Piece_Type promo = NO_TYPE) {
		Move m;
		m.from = static_cast<int8_t>(from);
		m.to = static_cast<int8_t>(to);
		m.promotion = promo;
		m.flags = flags;
		m.captured = squares[to];
		if (m.captured != NO_PIECE) m.flags |= MF_CAPTURE;
		out[n++] = m;
	};
	// A target square is usable if it is empty or holds an enemy man other than
	// the king; a king can never be captured in a legal position.
	auto usable = [&](int to) {
		const Piece p = squares[to];
		return p == NO_PIECE || (color_of(p) == them && type_of(p) != KING);
	};

	for (int from = 0; from < 64; ++from)
	{
		const Piece p = squares[from];
		if (p == NO_PIECE || color_of(p) != us) continue;
		const int f = file_of(from), r = rank_of(from);

		switch (type_of(p))
		{
			case PAWN:
			{
				const int dir = us == WHITE ? 1 : -1;
				const int last = us == WHITE ? 7 : 0;
				const int start = us == WHITE ? 1 : 6;
				const int r1 = r + dir;
				if (r1 < 0 || r1 > 7) break;
				auto pawn_to = [&](int to, bool capture) {
					if (rank_of(to) == last)
					{
						for (Piece_Type t : { QUEEN, ROOK, BISHOP, KNIGHT })
							add(from, to, capture ? MF_CAPTURE : 0, t);
					}
					else
						add(from, to, capture ? MF_CAPTURE : 0);
				};
				const int one = make_square(f, r1);
				if (squares[one] == NO_PIECE)
				{
					pawn_to(one, false);
					const int two = make_square(f, r + 2 * dir);
					if (r == start && squares[two] == NO_PIECE)
						add(from, two, MF_DOUBLE_PUSH);
				}
				for (int df : { -1, 1 })
				{
					if (!on_board(f + df, r1)) continue;
					const int to = make_square(f + df, r1);
					const Piece t = squares[to];
					if (t != NO_PIECE && color_of(t) == them && type_of(t) != KING)
						pawn_to(to, true);
					else if (to == ep && t == NO_PIECE
					         && squares[make_square(f + df, r)] == make_piece(them, PAWN))
					{
						Move m;
						m.from = static_cast<int8_t>(from);
						m.to = static_cast<int8_t>(to);
						m.flags = MF_CAPTURE | MF_EP;
						m.captured = make_piece(them, PAWN);
						out[n++] = m;
					}
				}
				break;
			}
			case KNIGHT:
			case KING:
			{
				const auto* steps = type_of(p) == KNIGHT ? KNIGHT_STEPS : KING_STEPS;
				for (int i = 0; i < 8; ++i)
				{
					const int nf = f + steps[i][0], nr = r + steps[i][1];
					if (on_board(nf, nr) && usable(make_square(nf, nr)))
						add(from, make_square(nf, nr));
				}
				break;
			}
			default:
			{
				const Piece_Type t = type_of(p);
				auto slide = [&](const int dirs[4][2]) {
					for (int d = 0; d < 4; ++d)
					{
						int nf = f + dirs[d][0], nr = r + dirs[d][1];
						while (on_board(nf, nr))
						{
							const int to = make_square(nf, nr);
							if (usable(to)) add(from, to);
							if (squares[to] != NO_PIECE) break;
							nf += dirs[d][0];
							nr += dirs[d][1];
						}
					}
				};
				if (t == BISHOP || t == QUEEN) slide(DIAGONALS);
				if (t == ROOK || t == QUEEN) slide(ORTHOGONALS);
				break;
			}
		}
	}

	// Castling, Chess960 rules: the king ends on the g- or c-file and the rook on
	// the f- or d-file, whatever files they started on. Every square the king or
	// the rook passes over or lands on must be empty, apart from the two of them.
	const int ksq = king_square(us);
	if (ksq != NO_SQUARE && rank_of(ksq) == home_rank(us))
	{
		for (int side : { H_SIDE, A_SIDE })
		{
			const int rook_from = castle[us][side];
			if (rook_from == NO_SQUARE || squares[rook_from] != make_piece(us, ROOK)) continue;
			const int rank = home_rank(us);
			const int king_to = make_square(side == H_SIDE ? 6 : 2, rank);
			const int rook_to = make_square(side == H_SIDE ? 5 : 3, rank);
			bool clear = true;
			auto span_clear = [&](int a, int b) {
				for (int s = std::min(a, b); s <= std::max(a, b); ++s)
					if (s != ksq && s != rook_from && squares[s] != NO_PIECE) clear = false;
			};
			span_clear(ksq, king_to);
			span_clear(rook_from, rook_to);
			if (!clear) continue;
			Move m;
			m.from = static_cast<int8_t>(ksq);
			m.to = static_cast<int8_t>(king_to);
			m.flags = MF_CASTLE;
			m.rook_from = static_cast<int8_t>(rook_from);
			out[n++] = m;
		}
	}
	return n;
}

// The king may not castle out of, through or into check. Squares it passes over
// are tested with the board as it stands; the square it lands on is tested with
// the king and rook already moved, so a rook that was shielding it counts.
bool Board::castling_is_legal(const Move& m) const
{
	const Color them = opposite(stm);
	if (attacked(m.from, them)) return false;
	const int step = m.to > m.from ? 1 : -1;
	if (m.to != m.from)
		for (int s = m.from + step; s != m.to; s += step)
			if (attacked(s, them)) return false;

	const bool h_side = file_of(m.to) == 6;
	Board after = *this;
	after.squares[m.from] = NO_PIECE;
	after.squares[m.rook_from] = NO_PIECE;
	after.squares[m.to] = make_piece(stm, KING);
	after.squares[make_square(h_side ? 5 : 3, home_rank(stm))] = make_piece(stm, ROOK);
	return !after.attacked(m.to, them);
}

bool Board::leaves_king_safe(const Move& m) const
{
	Board after = *this;
	after.make(m);
	return !after.in_check(stm);
}

int Board::legal_moves(Move* out) const
{
	Move pseudo[MAX_MOVES];
	const int n = pseudo_legal_moves(pseudo);
	int k = 0;
	for (int i = 0; i < n; ++i)
	{
		const bool ok = pseudo[i].is_castle() ? castling_is_legal(pseudo[i]) : leaves_king_safe(pseudo[i]);
		if (ok) out[k++] = pseudo[i];
	}
	return k;
}

std::vector<Move> Board::legal_moves() const
{
	Move buf[MAX_MOVES];
	const int n = legal_moves(buf);
	return std::vector<Move>(buf, buf + n);
}

void Board::make(const Move& m)
{
	const Color us = stm, them = opposite(stm);
	const Piece mover = squares[m.from];

	// Rights first, from the squares as they stand before the move.
	for (Color c : { WHITE, BLACK })
	{
		if (!has_rights(c)) continue;
		if (m.from == king_square(c))
		{
			castle[c] = { NO_SQUARE, NO_SQUARE };
			continue;
		}
		for (int side : { A_SIDE, H_SIDE })
			if (castle[c][side] == m.from || castle[c][side] == m.to)
				castle[c][side] = NO_SQUARE;
	}

	if (m.is_castle())
	{
		const bool h_side = file_of(m.to) == 6;
		squares[m.from] = NO_PIECE;
		squares[m.rook_from] = NO_PIECE;
		squares[m.to] = make_piece(us, KING);
		squares[make_square(h_side ? 5 : 3, home_rank(us))] = make_piece(us, ROOK);
	}
	else
	{
		if (m.is_ep())
			squares[make_square(file_of(m.to), rank_of(m.from))] = NO_PIECE;
		squares[m.to] = m.promotion != NO_TYPE ? make_piece(us, m.promotion) : mover;
		squares[m.from] = NO_PIECE;
	}

	ep = m.is_double_push() ? static_cast<int8_t>((m.from + m.to) / 2) : static_cast<int8_t>(NO_SQUARE);
	stm = them;
}

bool Board::has_legal_ep() const
{
	if (ep == NO_SQUARE) return false;
	Move buf[MAX_MOVES];
	const int n = legal_moves(buf);
	for (int i = 0; i < n; ++i)
		if (buf[i].is_ep()) return true;
	return false;
}

bool Board::is_checkmate() const
{
	Move buf[MAX_MOVES];
	return in_check(stm) && legal_moves(buf) == 0;
}

bool Board::is_stalemate() const
{
	Move buf[MAX_MOVES];
	return !in_check(stm) && legal_moves(buf) == 0;
}

std::string Board::fen() const
{
	std::string s;
	for (int r = 7; r >= 0; --r)
	{
		int run = 0;
		for (int f = 0; f < 8; ++f)
		{
			const Piece p = squares[make_square(f, r)];
			if (p == NO_PIECE) { ++run; continue; }
			if (run) s += static_cast<char>('0' + run);
			run = 0;
			s += piece_char(p);
		}
		if (run) s += static_cast<char>('0' + run);
		if (r) s += '/';
	}
	s += stm == WHITE ? " w " : " b ";

	std::string rights;
	for (Color c : { WHITE, BLACK })
		for (int side : { H_SIDE, A_SIDE })
		{
			const int rook = castle[c][side];
			if (rook == NO_SQUARE) continue;
			bool outermost = true;
			for (int f = 0; f < 8; ++f)
				if (squares[make_square(f, home_rank(c))] == make_piece(c, ROOK)
				    && (side == H_SIDE ? f > file_of(rook) : f < file_of(rook)))
					outermost = false;
			char letter = outermost ? (side == H_SIDE ? 'K' : 'Q') : static_cast<char>('A' + file_of(rook));
			if (c == BLACK) letter = static_cast<char>(letter - 'A' + 'a');
			rights += letter;
		}
	s += rights.empty() ? "-" : rights;
	s += ' ';
	s += has_legal_ep() ? square_name(ep) : "-";
	return s;
}

bool Board::from_fen(const std::string& fen, Board& out, std::string* error)
{
	auto fail = [&](const char* why) {
		if (error) *error = std::string(why) + ": " + fen;
		return false;
	};

	std::istringstream in(fen);
	std::string placement, side = "w", rights = "-", ep_text = "-";
	if (!(in >> placement)) return fail("empty FEN");
	in >> side >> rights >> ep_text;

	Board b;
	b.clear();
	int r = 7, f = 0;
	for (char c : placement)
	{
		if (c == '/')
		{
			if (f != 8 || r == 0) return fail("bad rank");
			--r;
			f = 0;
		}
		else if (c >= '1' && c <= '8')
		{
			f += c - '0';
			if (f > 8) return fail("bad rank");
		}
		else
		{
			const Piece p = piece_from_char(c);
			if (p == NO_PIECE || f >= 8) return fail("bad piece");
			b.squares[make_square(f, r)] = p;
			++f;
		}
	}
	if (r != 0 || f != 8) return fail("bad board");

	if (side == "w") b.stm = WHITE;
	else if (side == "b") b.stm = BLACK;
	else return fail("bad side to move");

	if (rights != "-")
	{
		for (char c : rights)
		{
			const Color color = (c >= 'a' && c <= 'z') ? BLACK : WHITE;
			const char u = static_cast<char>(c & ~0x20);
			const int ksq = b.king_square(color);
			if (ksq == NO_SQUARE || rank_of(ksq) != home_rank(color)) return fail("castling right without king at home");
			int rook = NO_SQUARE;
			if (u == 'K' || u == 'Q')
			{
				const bool h = u == 'K';
				for (int ff = 0; ff < 8; ++ff)
				{
					const int sq = make_square(ff, home_rank(color));
					if (b.squares[sq] != make_piece(color, ROOK)) continue;
					if (h ? ff <= file_of(ksq) : ff >= file_of(ksq)) continue;
					if (rook == NO_SQUARE || h) rook = sq;
				}
			}
			else if (u >= 'A' && u <= 'H')
			{
				rook = make_square(u - 'A', home_rank(color));
			}
			else
				return fail("bad castling field");
			if (rook == NO_SQUARE || b.squares[rook] != make_piece(color, ROOK) || file_of(rook) == file_of(ksq))
				return fail("castling right names no rook");
			b.castle[color][file_of(rook) > file_of(ksq) ? H_SIDE : A_SIDE] = static_cast<int8_t>(rook);
		}
	}

	if (ep_text != "-")
	{
		if (ep_text.size() != 2 || ep_text[0] < 'a' || ep_text[0] > 'h' || ep_text[1] < '1' || ep_text[1] > '8')
			return fail("bad en passant square");
		b.ep = static_cast<int8_t>(make_square(ep_text[0] - 'a', ep_text[1] - '1'));
	}
	out = b;
	return true;
}

bool castling_consistent(const Board& b, bool standard)
{
	for (Color c : { WHITE, BLACK })
	{
		if (!b.has_rights(c)) continue;
		const int ksq = b.king_square(c);
		if (ksq == NO_SQUARE || rank_of(ksq) != home_rank(c)) return false;
		if (standard && file_of(ksq) != 4) return false;
		for (int side : { A_SIDE, H_SIDE })
		{
			const int rook = b.castle[c][side];
			if (rook == NO_SQUARE) continue;
			if (rank_of(rook) != home_rank(c) || b.squares[rook] != make_piece(c, ROOK)) return false;
			if (side == H_SIDE ? file_of(rook) <= file_of(ksq) : file_of(rook) >= file_of(ksq)) return false;
			if (standard && file_of(rook) != (side == H_SIDE ? 7 : 0)) return false;
		}
	}
	return true;
}

bool ep_square_plausible(const Board& b, int ep_sq)
{
	// The side that just moved pushed a pawn from its second rank to its fourth,
	// passing ep_sq.
	const Color pusher = opposite(b.stm);
	const int ep_rank = pusher == WHITE ? 2 : 5;
	if (rank_of(ep_sq) != ep_rank) return false;
	const int dir = pusher == WHITE ? 1 : -1;
	const int pawn_sq = ep_sq + 8 * dir;
	const int origin = ep_sq - 8 * dir;
	return b.squares[pawn_sq] == make_piece(pusher, PAWN)
	    && b.squares[ep_sq] == NO_PIECE
	    && b.squares[origin] == NO_PIECE;
}

bool is_valid(const Board& b, Validity v, bool chess960)
{
	int kings[2] = { 0, 0 }, men[2] = { 0, 0 }, pawns[2] = { 0, 0 };
	for (int sq = 0; sq < 64; ++sq)
	{
		const Piece p = b.squares[sq];
		if (p == NO_PIECE) continue;
		const Color c = color_of(p);
		++men[c];
		if (type_of(p) == KING) ++kings[c];
		if (type_of(p) == PAWN)
		{
			++pawns[c];
			if (rank_of(sq) == 0 || rank_of(sq) == 7) return false;
		}
	}
	if (kings[WHITE] != 1 || kings[BLACK] != 1) return false;
	if (!castling_consistent(b, !chess960)) return false;
	if (b.ep != NO_SQUARE && !ep_square_plausible(b, b.ep)) return false;
	if (b.in_check(opposite(b.stm))) return false;
	if (v == Validity::TABLE) return true;

	if (men[WHITE] > 16 || men[BLACK] > 16 || pawns[WHITE] > 8 || pawns[BLACK] > 8) return false;

	const int ksq = b.king_square(b.stm);
	const uint64_t occ = b.occupancy();
	const uint64_t checkers = b.attackers(ksq, opposite(b.stm), occ);
	if (checkers == 0) return true;
	const int count = __builtin_popcountll(checkers);
	if (count > 2) return false;

	if (b.ep != NO_SQUARE)
	{
		// Check given by a double push: either the pushed pawn gives it, or the
		// push uncovered it. Anything else could not have been the last move.
		const Color pusher = opposite(b.stm);
		const int dir = pusher == WHITE ? 1 : -1;
		const int pushed_to = b.ep + 8 * dir;
		const int pushed_from = b.ep - 8 * dir;
		const uint64_t occ_before = (occ & ~(uint64_t{ 1 } << pushed_to)) | (uint64_t{ 1 } << pushed_from);
		const int top = 63 - __builtin_clzll(checkers);
		if (count > 1 || (top != pushed_to && b.attackers(ksq, pusher, occ_before) != 0))
			return false;
		return true;
	}

	if (count == 2)
	{
		// Two checkers on one line through the king cannot both have been
		// revealed or placed by a single move.
		const int a = __builtin_ctzll(checkers);
		const int c = 63 - __builtin_clzll(checkers);
		const int df = file_of(c) - file_of(a), dr = rank_of(c) - rank_of(a);
		const bool aligned = df == 0 || dr == 0 || std::abs(df) == std::abs(dr);
		if (aligned)
		{
			// The full line through a and c, both directions to the edge.
			const int sf = (df > 0) - (df < 0), sr = (dr > 0) - (dr < 0);
			int f = file_of(a), r = rank_of(a);
			while (f - sf >= 0 && f - sf < 8 && r - sr >= 0 && r - sr < 8) { f -= sf; r -= sr; }
			for (; f >= 0 && f < 8 && r >= 0 && r < 8; f += sf, r += sr)
				if (make_square(f, r) == ksq) return false;
		}
	}
	return true;
}

}  // namespace ref
