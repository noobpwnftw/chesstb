#include "chess/attack.h"

#include "chess/chess.h"
#include "chess/bitboard.h"

#include "util/defines.h"

#include <array>
#include <cstdint>
#include <utility>

#if defined(__AVX2__)
  #include <immintrin.h>
#endif
#if defined(_MSC_VER)
  #include <stdlib.h>
#endif

// Slider attacks by hyperbola quintessence: on each line through the slider,
// the attacks are (o - s) ^ reverse(reverse(o) - reverse(s)), o being the
// line's occupancy. For the file and both diagonals a byte swap is that
// reversal (one square per byte); they run together in AVX2 lanes where
// available. Ranks come from a small table.

namespace {

// Squares on the line through sq along (dr, df), both ways, sq excluded.
constexpr uint64_t line_mask(int sq, int dr, int df)
{
	uint64_t mask = 0;
	for (const int sign : { 1, -1 })
		for (int r = sq / 8 + sign * dr, f = sq % 8 + sign * df;
		     r >= 0 && r < 8 && f >= 0 && f < 8; r += sign * dr, f += sign * df)
			mask |= uint64_t{1} << (r * 8 + f);
	return mask;
}

INLINE uint64_t byte_swap(uint64_t b)
{
#if defined(_MSC_VER)
	return _byteswap_uint64(b);
#else
	return __builtin_bswap64(b);
#endif
}

// Rank attacks for a slider on `file`, by the rank's 6 inner occupancy bits
// (the edge squares never change the attack set).
alignas(64) constexpr auto RANK_ATTACKS = []() {
	std::array<std::array<uint8_t, 64>, 8> t{};
	for (int file = 0; file < 8; ++file)
		for (int occ6 = 0; occ6 < 64; ++occ6)
		{
			const int occ = occ6 << 1;
			int attacks = 0;
			for (int f = file + 1; f < 8; ++f) { attacks |= 1 << f; if (occ & (1 << f)) break; }
			for (int f = file - 1; f >= 0; --f) { attacks |= 1 << f; if (occ & (1 << f)) break; }
			t[file][occ6] = static_cast<uint8_t>(attacks);
		}
	return t;
}();

struct alignas(32) Slider_Masks
{
	// File, diagonal, antidiagonal, unused: one AVX2 lane each.
	uint64_t file, diag, antidiag, none;
	uint64_t r, rr;  // 2 * square_bb(sq), 2 * square_bb(63 - sq)
	const uint8_t* rank_attacks;
	int shift;  // 8 * rank

	// [bishop, rook]: callers take the half they need and the rest folds away.
	std::pair<uint64_t, uint64_t> both(uint64_t occupied) const
	{
		const uint64_t rank = static_cast<uint64_t>(rank_attacks[(occupied >> (shift + 1)) & 0x3f]) << shift;
#if defined(__AVX2__)
		// Byte swap within 128-bit lanes.
		const auto bswap = [](__m256i v) {
			return _mm256_shuffle_epi8(v, _mm256_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
			                                              0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
		};
		const __m256i mask = _mm256_load_si256(reinterpret_cast<const __m256i*>(this));
		const __m256i o = _mm256_and_si256(mask, _mm256_set1_epi64x(static_cast<long long>(occupied)));
		const __m256i fwd = _mm256_sub_epi64(o, _mm256_set1_epi64x(static_cast<long long>(r)));
		const __m256i rev = bswap(_mm256_sub_epi64(bswap(o), _mm256_set1_epi64x(static_cast<long long>(rr))));
		const __m256i result = _mm256_and_si256(_mm256_xor_si256(fwd, rev), mask);

		// Lane 0: rook (file only); lanes 1 and 2: bishop.
		const __m256i rook_bishop =
			_mm256_or_si256(result, _mm256_permute4x64_epi64(result, _MM_SHUFFLE(3, 1, 2, 0)));
		return { static_cast<uint64_t>(_mm_extract_epi64(_mm256_castsi256_si128(rook_bishop), 1)),
		         static_cast<uint64_t>(_mm_cvtsi128_si64(_mm256_castsi256_si128(result))) | rank };
#else
		const auto line = [&](uint64_t m) {
			const uint64_t o = occupied & m;
			return ((o - r) ^ byte_swap(byte_swap(o) - rr)) & m;
		};
		return { line(diag) | line(antidiag), line(file) | rank };
#endif
	}
};

alignas(64) constexpr auto SLIDER_MASKS = []() {
	std::array<Slider_Masks, SQUARE_NB> t{};
	for (int sq = 0; sq < SQUARE_NB; ++sq)
		t[sq] = { line_mask(sq, 1, 0), line_mask(sq, 1, 1), line_mask(sq, 1, -1), 0,
		          (uint64_t{1} << sq) * 2, (uint64_t{1} << (63 - sq)) * 2,
		          RANK_ATTACKS[sq % 8].data(), 8 * (sq / 8) };
	return t;
}();

}  // namespace

Bitboard bishop_attacks(Square sq, Bitboard occupied)
{
	return Bitboard(SLIDER_MASKS[sq].both(occupied.bits()).first);
}

Bitboard rook_attacks(Square sq, Bitboard occupied)
{
	return Bitboard(SLIDER_MASKS[sq].both(occupied.bits()).second);
}

Bitboard queen_attacks(Square sq, Bitboard occupied)
{
	const auto [bishop, rook] = SLIDER_MASKS[sq].both(occupied.bits());
	return Bitboard(bishop | rook);
}

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
