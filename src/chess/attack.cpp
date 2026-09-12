#include "chess/attack.h"

#include "chess/chess.h"
#include "chess/bitboard.h"

#include "util/defines.h"

#include <cstdint>

#if defined(__BMI2__)
  #include <immintrin.h>
  #define CHESSTB_HAVE_PEXT 1
#else
  #define CHESSTB_HAVE_PEXT 0
#endif

namespace {

uint64_t ray_attacks_raw(Square sq, int dr, int df, uint64_t occupied)
{
	uint64_t attacks = 0;
	int r = sq_rank(sq);
	int f = sq_file(sq);
	for (;;)
	{
		r += dr;
		f += df;
		if (r < 0 || r > 7 || f < 0 || f > 7)
			break;
		const uint64_t b = uint64_t(1) << sq_make(static_cast<Rank>(r), static_cast<File>(f));
		attacks |= b;
		if (occupied & b)
			break;
	}
	return attacks;
}

#if CHESSTB_HAVE_PEXT

constexpr int BISHOP_DIRS[4][2] = { { 1, 1}, { 1,-1}, {-1, 1}, {-1,-1} };
constexpr int ROOK_DIRS[4][2]   = { { 1, 0}, {-1, 0}, { 0, 1}, { 0,-1} };

struct Slider_Tables
{
	struct Entry
	{
		uint64_t mask;
		const uint64_t* attacks;
	};

	Entry bishop[SQUARE_NB];
	Entry rook[SQUARE_NB];
	uint64_t bishop_data[5248];
	uint64_t rook_data[102400];
#ifndef NDEBUG
	bool ready = false;
#endif

	Slider_Tables()
	{
		fill(bishop, bishop_data, BISHOP_DIRS);
		fill(rook, rook_data, ROOK_DIRS);
#ifndef NDEBUG
		ready = true;
#endif
	}

private:
	// Mask = ray attacks with empty occupancy, minus the last square on the ray
	// (the edge square: occupancy there can't change the attack pattern further).
	static uint64_t ray_mask(Square sq, int dr, int df)
	{
		uint64_t mask = 0;
		int r = sq_rank(sq);
		int f = sq_file(sq);
		for (;;)
		{
			const int nr = r + dr;
			const int nf = f + df;
			if (nr < 0 || nr > 7 || nf < 0 || nf > 7)
				break;
			const int nr2 = nr + dr;
			const int nf2 = nf + df;
			const bool is_edge = (nr2 < 0 || nr2 > 7 || nf2 < 0 || nf2 > 7);
			if (!is_edge)
				mask |= uint64_t(1) << sq_make(static_cast<Rank>(nr), static_cast<File>(nf));
			r = nr; f = nf;
		}
		return mask;
	}

	static void fill(Entry* entries, uint64_t* data, const int (&dirs)[4][2])
	{
		size_t offset = 0;
		for (int s = 0; s < SQUARE_NB; ++s)
		{
			const Square sq = static_cast<Square>(s);
			uint64_t mask = 0;
			for (auto& d : dirs)
				mask |= ray_mask(sq, d[0], d[1]);

			entries[sq].mask = mask;
			entries[sq].attacks = data + offset;

			const int bits = __builtin_popcountll(mask);
			const size_t num_subsets = size_t(1) << bits;

			uint64_t subset = 0;
			for (size_t i = 0; i < num_subsets; ++i)
			{
				uint64_t attacks = 0;
				for (auto& d : dirs)
					attacks |= ray_attacks_raw(sq, d[0], d[1], subset);
				const uint64_t idx = _pext_u64(subset, mask);
				data[offset + idx] = attacks;
				subset = (subset - mask) & mask;
			}
			offset += num_subsets;
		}
	}
};

const Slider_Tables SLIDERS;

#endif

}  // namespace

#if CHESSTB_HAVE_PEXT
Bitboard bishop_attacks(Square sq, Bitboard occupied)
{
	ASSERT(SLIDERS.ready);
	const auto& e = SLIDERS.bishop[sq];
	return Bitboard(e.attacks[_pext_u64(occupied.bits(), e.mask)]);
}

Bitboard rook_attacks(Square sq, Bitboard occupied)
{
	ASSERT(SLIDERS.ready);
	const auto& e = SLIDERS.rook[sq];
	return Bitboard(e.attacks[_pext_u64(occupied.bits(), e.mask)]);
}
#else
Bitboard bishop_attacks(Square sq, Bitboard occupied)
{
	const uint64_t occ = occupied.bits();
	return Bitboard(
		  ray_attacks_raw(sq,  1,  1, occ)
		| ray_attacks_raw(sq,  1, -1, occ)
		| ray_attacks_raw(sq, -1,  1, occ)
		| ray_attacks_raw(sq, -1, -1, occ));
}

Bitboard rook_attacks(Square sq, Bitboard occupied)
{
	const uint64_t occ = occupied.bits();
	return Bitboard(
		  ray_attacks_raw(sq,  1,  0, occ)
		| ray_attacks_raw(sq, -1,  0, occ)
		| ray_attacks_raw(sq,  0,  1, occ)
		| ray_attacks_raw(sq,  0, -1, occ));
}
#endif

Bitboard piece_attacks(Piece pc, Square from, Bitboard occupied)
{
	switch (piece_type(pc))
	{
		case KING:   return king_attacks(from);
		case KNIGHT: return knight_attacks(from);
		case BISHOP: return bishop_attacks(from, occupied);
		case ROOK:   return rook_attacks(from, occupied);
		case QUEEN:  return queen_attacks(from, occupied);
		case PAWN:   return pawn_attacks(piece_color(pc), from);
		default:     ASSUME(false); return Bitboard::make_empty();
	}
}
