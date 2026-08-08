#include "chess/attack.h"

#include "chess/chess.h"
#include "chess/bitboard.h"

#include "util/defines.h"

#include <cstdint>

// PEXT (BMI2) is x86-only and a big speedup. On other archs (e.g. ARM64) we
// fall back to ray-walking sliders — slower but identical results.
#if defined(__BMI2__)
  #include <immintrin.h>
  #define CHESSTB_HAVE_PEXT 1
#else
  #define CHESSTB_HAVE_PEXT 0
#endif

Bitboard KING_ATTACKS[SQUARE_NB];
Bitboard KNIGHT_ATTACKS[SQUARE_NB];
Bitboard PAWN_ATTACKS[COLOR_NB][SQUARE_NB];
Bitboard PAWN_PUSHES[COLOR_NB][SQUARE_NB];
Bitboard PAWN_DOUBLE_PUSHES[COLOR_NB][SQUARE_NB];

namespace {

INLINE uint64_t ray_attacks_raw(Square sq, int dr, int df, uint64_t occupied)
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

INLINE uint64_t ray_mask(Square sq, int dr, int df)
{
	// Mask = ray attacks with empty occupancy, minus the last square on the ray
	// (the edge square: occupancy there can't change the attack pattern further).
	uint64_t mask = 0;
	int r = sq_rank(sq);
	int f = sq_file(sq);
	for (;;)
	{
		const int nr = r + dr;
		const int nf = f + df;
		if (nr < 0 || nr > 7 || nf < 0 || nf > 7)
			break;
		// Look ahead one more — only include current step if it is NOT the last on this ray.
		const int nr2 = nr + dr;
		const int nf2 = nf + df;
		const bool is_edge = (nr2 < 0 || nr2 > 7 || nf2 < 0 || nf2 > 7);
		if (!is_edge)
			mask |= uint64_t(1) << sq_make(static_cast<Rank>(nr), static_cast<File>(nf));
		r = nr; f = nf;
	}
	return mask;
}

INLINE void try_add(Bitboard& bb, int r, int f)
{
	if (r >= 0 && r <= 7 && f >= 0 && f <= 7)
		bb |= sq_make(static_cast<Rank>(r), static_cast<File>(f));
}

#if CHESSTB_HAVE_PEXT
// PEXT slider tables. Indexed by [square][pext(occupancy, mask)].
// Per-square offset layout keeps memory tight (~840 KB total).
struct Slider_Entry
{
	uint64_t mask;
	const uint64_t* attacks;  // points into the flat data array below
};

Slider_Entry BISHOP_PEXT[SQUARE_NB];
Slider_Entry ROOK_PEXT[SQUARE_NB];

// 5248 bishop entries + 102400 rook entries = ~107K * 8 bytes = ~840 KB.
uint64_t BISHOP_DATA[5248];
uint64_t ROOK_DATA[102400];
#endif

}  // namespace

void attack_init()
{
	for (int s = 0; s < SQUARE_NB; ++s)
	{
		const Square sq = static_cast<Square>(s);
		const int r = sq_rank(sq);
		const int f = sq_file(sq);

		// King.
		{
			Bitboard bb = Bitboard::make_empty();
			for (int dr = -1; dr <= 1; ++dr)
				for (int df = -1; df <= 1; ++df)
					if (dr || df)
						try_add(bb, r + dr, f + df);
			KING_ATTACKS[sq] = bb;
		}

		// Knight.
		{
			Bitboard bb = Bitboard::make_empty();
			static constexpr int K[8][2] = {
				{ 1, 2}, { 2, 1}, { 2,-1}, { 1,-2},
				{-1,-2}, {-2,-1}, {-2, 1}, {-1, 2}
			};
			for (auto& d : K)
				try_add(bb, r + d[0], f + d[1]);
			KNIGHT_ATTACKS[sq] = bb;
		}

		// Pawn captures + pushes.
		for (Color c : { WHITE, BLACK })
		{
			Bitboard caps = Bitboard::make_empty();
			Bitboard push = Bitboard::make_empty();
			Bitboard dpush = Bitboard::make_empty();
			const int dr_fwd = (c == WHITE) ? 1 : -1;
			try_add(caps, r + dr_fwd, f - 1);
			try_add(caps, r + dr_fwd, f + 1);
			try_add(push, r + dr_fwd, f);
			const int start_rank = (c == WHITE) ? 1 : 6;
			if (r == start_rank)
				try_add(dpush, r + 2 * dr_fwd, f);
			PAWN_ATTACKS[c][sq] = caps;
			PAWN_PUSHES[c][sq] = push;
			PAWN_DOUBLE_PUSHES[c][sq] = dpush;
		}
	}

#if CHESSTB_HAVE_PEXT
	// Build PEXT slider tables.
	auto fill_pext = [](Slider_Entry* entries, uint64_t* data,
	                     const int (*dirs)[2], int num_dirs)
	{
		size_t offset = 0;
		for (int s = 0; s < SQUARE_NB; ++s)
		{
			const Square sq = static_cast<Square>(s);
			uint64_t mask = 0;
			for (int d = 0; d < num_dirs; ++d)
				mask |= ray_mask(sq, dirs[d][0], dirs[d][1]);

			entries[sq].mask = mask;
			entries[sq].attacks = data + offset;

			const int bits = __builtin_popcountll(mask);
			const size_t num_subsets = size_t(1) << bits;

			uint64_t subset = 0;
			for (size_t i = 0; i < num_subsets; ++i)
			{
				uint64_t attacks = 0;
				for (int d = 0; d < num_dirs; ++d)
					attacks |= ray_attacks_raw(sq, dirs[d][0], dirs[d][1], subset);
				const uint64_t idx = _pext_u64(subset, mask);
				data[offset + idx] = attacks;
				subset = (subset - mask) & mask;
			}
			offset += num_subsets;
		}
	};

	static constexpr int BISHOP_DIRS[4][2] = { { 1, 1}, { 1,-1}, {-1, 1}, {-1,-1} };
	static constexpr int ROOK_DIRS[4][2]   = { { 1, 0}, {-1, 0}, { 0, 1}, { 0,-1} };

	fill_pext(BISHOP_PEXT, BISHOP_DATA, BISHOP_DIRS, 4);
	fill_pext(ROOK_PEXT,   ROOK_DATA,   ROOK_DIRS,   4);
#endif
}

#if CHESSTB_HAVE_PEXT
Bitboard bishop_attacks(Square sq, Bitboard occupied)
{
	const Slider_Entry& e = BISHOP_PEXT[sq];
	return Bitboard(e.attacks[_pext_u64(occupied.bits(), e.mask)]);
}

Bitboard rook_attacks(Square sq, Bitboard occupied)
{
	const Slider_Entry& e = ROOK_PEXT[sq];
	return Bitboard(e.attacks[_pext_u64(occupied.bits(), e.mask)]);
}
#else
// Fallback: ray-walking sliders. Correct on any arch, slower than PEXT but
// fine for development/testing on non-x86 hosts.
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
