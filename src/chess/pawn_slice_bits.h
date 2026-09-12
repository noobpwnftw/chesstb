#pragma once

#include "chess/bitboard.h"
#include "chess/pair_group.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Emit survivor bits in the existing cartesian order: pair, white, black.
// Each call supplies up to 64 bits starting at cart; adjacent calls may
// share a destination word when a placement radix is not word-aligned.
// Generator and prober placements have different types but the same API.
template <typename Placement, typename Emit>
void build_pawn_slice_bits(
	const Pair_Group* pair,
	const std::vector<Placement>& white,
	const std::vector<Placement>& black,
	const std::vector<Bitboard>& white_occ,
	const std::vector<Bitboard>& black_occ,
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
				Square ws[] = {pw, sq}, bs[] = {pb, sq};
				Square cw = SQ_END, cb = SQ_END;
				Pair_Group::canonical_pair(Const_Span<Square>(ws, 2), Const_Span<Square>(bs, 1), cw, cb);
				if (cw == pw && cb == pb)
					white_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
				Pair_Group::canonical_pair(Const_Span<Square>(ws, 1), Const_Span<Square>(bs, 2), cw, cb);
				if (cw == pw && cb == pb)
					black_allowed[sq][p >> 6] |= uint64_t{1} << (p & 63);
			}
		}
		auto candidates = [&](const std::vector<Placement>& placements,
		                      const std::array<Mask, SQUARE_NB>& allowed) {
			std::vector<Mask> result(placements.size(), all);
			for (size_t i = 0; i < placements.size(); ++i)
				for (size_t j = 0; j < placements[i].size(); ++j)
					for (size_t word = 0; word < 2; ++word)
						result[i][word] &= allowed[placements[i][j]][word];
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
				for (size_t j = 0; j < black[b].size(); ++j)
					if (Pair_Group::is_opposing_pair(ws, black[b][j]))
						first = std::min(first, static_cast<size_t>(pair->index_of(ws, black[b][j])));
				before[ws] = prefix(first);
			}
			for (size_t w = 0; w < white.size(); ++w, cart += pair->table_size())
			{
				if (white_occ[w] & black_occ[b]) continue;
				Mask survivors;
				for (size_t word = 0; word < 2; ++word)
				{
					survivors[word] = white_candidates[w][word] & black_candidates[b][word];
					for (size_t j = 0; j < white[w].size(); ++j)
						survivors[word] &= before[white[w][j]][word];
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
			for (size_t j = 0; j < white[w].size(); ++j)
				occupied[white[w][j]][w >> 6] |= uint64_t{1} << (w & 63);
		const size_t tail = white.size() & 63;
		for (size_t b = 0; b < black.size(); ++b)
			for (size_t word = 0; word < words; ++word)
			{
				uint64_t blocked = 0;
				for (size_t j = 0; j < black[b].size(); ++j)
					blocked |= occupied[black[b][j]][word];
				uint64_t survivors = ~blocked;
				if (word + 1 == words && tail)
					survivors &= (uint64_t{1} << tail) - 1;
				emit(b * white.size() + word * 64, survivors);
			}
	}
}
