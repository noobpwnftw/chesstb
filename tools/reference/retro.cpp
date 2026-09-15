#include "retro.h"

#include <functional>
#include <string>
#include <unordered_set>

namespace ref {

namespace {

using Emit = std::function<void(const Board&)>;

// The predecessor as generated, plus variants that held an unused en passant
// right: the opponent had just pushed a pawn two squares and the mover declined
// the capture. Only squares where the capture would be legal are offered; any
// other en passant square normalizes away and changes nothing.
void emit_with_ep_variants(const Board& pred, const Emit& emit)
{
	Board plain = pred;
	plain.ep = NO_SQUARE;
	emit(plain);

	const Color pusher = opposite(pred.stm);
	const int pawn_rank = pusher == WHITE ? 3 : 4;
	const int ep_rank = pusher == WHITE ? 2 : 5;
	const int origin_rank = pusher == WHITE ? 1 : 6;
	for (int f = 0; f < 8; ++f)
	{
		if (pred.squares[make_square(f, pawn_rank)] != make_piece(pusher, PAWN)) continue;
		if (pred.squares[make_square(f, ep_rank)] != NO_PIECE) continue;
		if (pred.squares[make_square(f, origin_rank)] != NO_PIECE) continue;
		Board cand = plain;
		cand.ep = static_cast<int8_t>(make_square(f, ep_rank));
		if (cand.has_legal_ep()) emit(cand);
	}
}

// Castling-right supersets consistent with the predecessor's placement. Rights
// are only ever lost going forward, so a predecessor may hold any right its
// successor lacks, for either colour, provided the king and a rook stand where
// that right needs them. Forward validation rejects whatever remains impossible.
void emit_with_rights_supersets(const Board& pred, const Retro_Options& opt, const Emit& emit)
{
	struct Slot { Color color; int side; std::vector<int8_t> options; };
	std::vector<Slot> slots;
	for (Color c : { WHITE, BLACK })
	{
		const int ksq = pred.king_square(c);
		for (int side : { A_SIDE, H_SIDE })
		{
			if (pred.castle[c][side] != NO_SQUARE) continue;
			std::vector<int8_t> options{ static_cast<int8_t>(NO_SQUARE) };
			if (ksq != NO_SQUARE && rank_of(ksq) == home_rank(c) && (opt.chess960 || file_of(ksq) == 4))
			{
				for (int f = 0; f < 8; ++f)
				{
					const int sq = make_square(f, home_rank(c));
					if (pred.squares[sq] != make_piece(c, ROOK)) continue;
					if (side == H_SIDE ? f <= file_of(ksq) : f >= file_of(ksq)) continue;
					if (!opt.chess960 && f != (side == H_SIDE ? 7 : 0)) continue;
					options.push_back(static_cast<int8_t>(sq));
				}
			}
			if (options.size() > 1) slots.push_back({ c, side, std::move(options) });
		}
	}

	std::vector<size_t> choice(slots.size(), 0);
	for (;;)
	{
		Board b = pred;
		for (size_t i = 0; i < slots.size(); ++i)
			b.castle[slots[i].color][slots[i].side] = slots[i].options[choice[i]];
		emit(b);
		size_t i = 0;
		for (; i < slots.size(); ++i)
		{
			if (++choice[i] < slots[i].options.size()) break;
			choice[i] = 0;
		}
		if (i == slots.size()) break;
	}
}

// Squares the man now on `to` could have moved from, with whether it arrived by
// promoting. Only empty squares are returned, and sliders stop at the first
// occupied square, since the path between the two squares is the same before
// and after the move.
struct Origin { int square; bool promoted; };

std::vector<Origin> origins_of(const Board& b, int to, Piece_Type type, Color mover)
{
	std::vector<Origin> out;
	const int f = file_of(to), r = rank_of(to);
	auto empty = [&](int ff, int rr) { return ff >= 0 && ff < 8 && rr >= 0 && rr < 8 && b.squares[make_square(ff, rr)] == NO_PIECE; };

	if (type == PAWN)
	{
		const int back = mover == WHITE ? -1 : 1;
		const int fr = r + back;
		if (fr < 0 || fr > 7) return out;
		if (empty(f, fr)) out.push_back({ make_square(f, fr), false });
		// A double push lands on the pawn's fourth rank. The test is on the
		// destination rank; testing the rank in between drops every double push.
		const int fourth = mover == WHITE ? 3 : 4;
		const int start = mover == WHITE ? 1 : 6;
		if (r == fourth && empty(f, start)) out.push_back({ make_square(f, start), false });
		for (int df : { -1, 1 })
			if (empty(f + df, fr)) out.push_back({ make_square(f + df, fr), false });
		return out;
	}

	if (type == KNIGHT || type == KING)
	{
		const auto* steps = type == KNIGHT ? detail::KNIGHT_STEPS : detail::KING_STEPS;
		for (int i = 0; i < 8; ++i)
			if (empty(f + steps[i][0], r + steps[i][1]))
				out.push_back({ make_square(f + steps[i][0], r + steps[i][1]), false });
	}
	else
	{
		const int first = type == ROOK ? 4 : 0;
		const int last = type == BISHOP ? 4 : 8;
		for (int d = first; d < last; ++d)
		{
			int nf = f + detail::SLIDER_DIRS[d][0], nr = r + detail::SLIDER_DIRS[d][1];
			while (empty(nf, nr))
			{
				out.push_back({ make_square(nf, nr), false });
				nf += detail::SLIDER_DIRS[d][0];
				nr += detail::SLIDER_DIRS[d][1];
			}
		}
	}

	// A knight, bishop, rook or queen on the last rank may be a promoted pawn,
	// pushed from the file below or capturing from a neighbouring one.
	if (type != KING && r == (mover == WHITE ? 7 : 0))
	{
		const int pr = mover == WHITE ? 6 : 1;
		for (int df : { 0, -1, 1 })
			if (empty(f + df, pr)) out.push_back({ make_square(f + df, pr), true });
	}
	return out;
}

void generate_candidates(const Board& target, const Retro_Options& opt, const Emit& emit)
{
	const Color mover = opposite(target.stm);
	const Color victim = target.stm;

	for (int to = 0; to < 64; ++to)
	{
		const Piece p = target.squares[to];
		if (p == NO_PIECE || color_of(p) != mover) continue;
		const Piece_Type type = type_of(p);

		std::vector<Piece> uncaptures{ NO_PIECE };
		for (Piece_Type t : { PAWN, KNIGHT, BISHOP, ROOK, QUEEN })
		{
			if (t == PAWN && (rank_of(to) == 0 || rank_of(to) == 7)) continue;
			uncaptures.push_back(make_piece(victim, t));
		}

		for (const Origin& o : origins_of(target, to, type, mover))
		{
			const bool pawn_move = type == PAWN || o.promoted;
			const bool same_file = file_of(o.square) == file_of(to);
			for (Piece unc : uncaptures)
			{
				// A pawn moves straight only when it does not capture, and
				// diagonally only when it does.
				if (pawn_move && same_file && unc != NO_PIECE) continue;
				if (pawn_move && !same_file && unc == NO_PIECE) continue;
				Board base = target;
				base.stm = mover;
				base.ep = NO_SQUARE;
				base.squares[to] = unc;
				base.squares[o.square] = o.promoted ? make_piece(mover, PAWN) : p;
				emit_with_rights_supersets(base, opt, [&](const Board& b) { emit_with_ep_variants(b, emit); });
			}
		}

		// An en passant capture: the capturing pawn stands on the en passant
		// square, and the captured pawn returns to the square beside its origin.
		if (type == PAWN && rank_of(to) == (mover == WHITE ? 5 : 2))
		{
			const int from_rank = mover == WHITE ? 4 : 3;
			const int victim_sq = make_square(file_of(to), from_rank);
			if (target.squares[victim_sq] == NO_PIECE)
			{
				for (int df : { -1, 1 })
				{
					const int ff = file_of(to) + df;
					if (ff < 0 || ff > 7) continue;
					const int from = make_square(ff, from_rank);
					if (target.squares[from] != NO_PIECE) continue;
					Board pred = target;
					pred.stm = mover;
					pred.squares[to] = NO_PIECE;
					pred.squares[from] = make_piece(mover, PAWN);
					pred.squares[victim_sq] = make_piece(victim, PAWN);
					pred.ep = static_cast<int8_t>(to);
					emit_with_rights_supersets(pred, opt, [&](const Board& b) {
						Board with_ep = b;
						with_ep.ep = static_cast<int8_t>(to);
						emit(with_ep);
					});
				}
			}
		}
	}

	// Castling: the king stands on the g- or c-file and the rook beside it on the
	// f- or d-file. Castling gives up both of the mover's rights, so the
	// predecessor held the one it used and may have held the other.
	const int rank = home_rank(mover);
	for (int side : { H_SIDE, A_SIDE })
	{
		const int king_to = make_square(side == H_SIDE ? 6 : 2, rank);
		const int rook_to = make_square(side == H_SIDE ? 5 : 3, rank);
		if (target.squares[king_to] != make_piece(mover, KING)) continue;
		if (target.squares[rook_to] != make_piece(mover, ROOK)) continue;
		for (int kf = 0; kf < 8; ++kf)
		{
			for (int rf = 0; rf < 8; ++rf)
			{
				if (rf == kf) continue;
				if (side == H_SIDE ? rf <= kf : rf >= kf) continue;
				if (!opt.chess960 && (kf != 4 || rf != (side == H_SIDE ? 7 : 0))) continue;
				const int king_from = make_square(kf, rank);
				const int rook_from = make_square(rf, rank);
				Board pred = target;
				pred.squares[king_to] = NO_PIECE;
				pred.squares[rook_to] = NO_PIECE;
				if (pred.squares[king_from] != NO_PIECE || pred.squares[rook_from] != NO_PIECE) continue;
				pred.stm = mover;
				pred.ep = NO_SQUARE;
				pred.squares[king_from] = make_piece(mover, KING);
				pred.squares[rook_from] = make_piece(mover, ROOK);
				pred.castle[mover][side] = static_cast<int8_t>(rook_from);
				emit_with_rights_supersets(pred, opt, [&](const Board& b) { emit_with_ep_variants(b, emit); });
			}
		}
	}
}

}  // namespace

std::vector<Predecessor> predecessors(const Board& target_in, const Retro_Options& opt)
{
	Board target = target_in;
	target.normalize_ep();
	const std::string target_key = target.key();

	std::unordered_set<std::string> seen;
	std::vector<Predecessor> out;
	Move moves[MAX_MOVES];

	generate_candidates(target, opt, [&](const Board& cand) {
		std::string key = cand.key();
		if (seen.count(key)) return;
		if (!is_valid(cand, opt.validity, opt.chess960)) return;
		const int n = cand.legal_moves(moves);
		for (int i = 0; i < n; ++i)
		{
			Board after = cand;
			after.make(moves[i]);
			if (after.key() != target_key) continue;
			seen.insert(std::move(key));
			Predecessor pred{ cand, moves[i] };
			pred.board.normalize_ep();
			out.push_back(pred);
			return;
		}
	});
	return out;
}

uint64_t unmake_perft(const Board& target, int depth, const Retro_Options& opt)
{
	if (depth == 0) return 1;
	uint64_t n = 0;
	for (const Predecessor& p : predecessors(target, opt))
		n += unmake_perft(p.board, depth - 1, opt);
	return n;
}

}  // namespace ref
