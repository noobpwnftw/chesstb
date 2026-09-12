#pragma once

#include "chess/bitboard.h"
#include "chess/pair_group.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Emits survivor bits in cartesian order (pair, white, black). `white` and
// `black` list each side's free-pawn placements (one empty board if none);
// each call gives up to 64 bits from `cart`, sharing a word across calls
// when a radix isn't word-aligned.
template <typename Emit>
void build_pawn_slice_bits(
	const Pair_Group* pair,
	const std::vector<Bitboard>& white,
	const std::vector<Bitboard>& black,
	Emit&& emit)
{
	if (pair)
	{
		using Mask = std::array<uint64_t, 2>;
		static_assert(Pair_Group::PLACEMENT_COUNT == 120);
		auto prefix = [](size_t n) -> Mask {
			return { n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1,
			         n <= 64 ? 0 : (uint64_t{1} << (n - 64)) - 1 };
		};
		const Mask all = prefix(pair->table_size());
		std::array<Mask, SQUARE_NB> white_allowed{}, black_allowed{};
		for (size_t p = 0; p < pair->table_size(); ++p)
		{
			const auto pi = static_cast<Pair_Group::Index>(p);
			const Square pw = pair->white_square(pi), pb = pair->black_square(pi);
			for (Square sq = SQ_A2; sq <= SQ_H7; sq = static_cast<Square>(sq + 1))
			{
				if (sq == pw || sq == pb) continue;
				Square cw = SQ_END, cb = SQ_END;
				Pair_Group::canonical_pair(square_bb(pw) | sq, square_bb(pb), cw, cb);
				if (cw == pw && cb == pb)
					white_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
				Pair_Group::canonical_pair(square_bb(pw), square_bb(pb) | sq, cw, cb);
				if (cw == pw && cb == pb)
					black_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
			}
		}
		auto candidates = [&](const std::vector<Bitboard>& placements,
		                      const std::array<Mask, SQUARE_NB>& allowed) {
			std::vector<Mask> result(placements.size(), all);
			for (size_t i = 0; i < placements.size(); ++i)
				for (Bitboard b = placements[i]; b;)
				{
					const Square sq = b.pop_first_square();
					for (size_t word = 0; word < 2; ++word)
						result[i][word] &= allowed[sq][word];
				}
			return result;
		};
		const auto white_candidates = candidates(white, white_allowed);
		const auto black_candidates = candidates(black, black_allowed);
		size_t cart = 0;
		for (size_t b = 0; b < black.size(); ++b)
		{
			// Pair indices follow canonical ordering. The designated pair
			// must precede every pair among the additional pawns as well.
			std::array<Mask, SQUARE_NB> before;
			before.fill(all);
			for (Square ws = SQ_A2; ws <= SQ_H7; ws = static_cast<Square>(ws + 1))
			{
				size_t first = pair->table_size();
				for (Bitboard t = black[b]; t;)
				{
					const Square bs = t.pop_first_square();
					if (Pair_Group::is_opposing_pair(ws, bs))
						first = std::min(first, static_cast<size_t>(pair->index_of(ws, bs)));
				}
				before[ws] = prefix(first);
			}
			for (size_t w = 0; w < white.size(); ++w, cart += pair->table_size())
			{
				if (white[w] & black[b]) continue;
				Mask survivors;
				for (size_t word = 0; word < 2; ++word)
				{
					survivors[word] = white_candidates[w][word] & black_candidates[b][word];
					for (Bitboard t = white[w]; t;)
						survivors[word] &= before[t.pop_first_square()][word];
				}
				emit(cart, survivors[0]);
				emit(cart + 64, survivors[1]);
			}
		}
	}
	else
	{
		// Each black placement excludes every white placement touching
		// any of its squares. Compute a whole word of placements at once.
		const size_t words = (white.size() + 63) / 64;
		std::array<std::vector<uint64_t>, SQUARE_NB> occupied;
		for (auto& bits : occupied) bits.resize(words);
		for (size_t w = 0; w < white.size(); ++w)
			for (Bitboard t = white[w]; t;)
				occupied[t.pop_first_square()][w >> 6] |= uint64_t{1} << (w & 63);
		const size_t tail = white.size() & 63;
		for (size_t b = 0; b < black.size(); ++b)
			for (size_t word = 0; word < words; ++word)
			{
				uint64_t blocked = 0;
				for (Bitboard t = black[b]; t;)
					blocked |= occupied[t.pop_first_square()][word];
				uint64_t survivors = ~blocked;
				if (word + 1 == words && tail)
					survivors &= (uint64_t{1} << tail) - 1;
				emit(b * white.size() + word * 64, survivors);
			}
	}
}
