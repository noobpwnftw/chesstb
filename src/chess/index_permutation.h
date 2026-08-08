#pragma once

#include "chess/piece_config.h"

#include "util/defines.h"

// Permutation index over the populated non-pawn classes, stored as a bare uint32
// (perm 0 == native order). Validity is just the FACTORIAL range bound.
constexpr std::array<uint32_t, 9> FACTORIAL = {
	1u, 1u, 2u, 6u, 24u, 120u, 720u, 5040u, 40320u
};

template <typename Config>
NODISCARD inline bool index_permutation_config_is_valid(
	const Config& cfg,
	uint32_t perm)
{
	const size_t n = cfg.num_populated_classes();
	return n <= 8
	    && perm < FACTORIAL[n];
}

template <typename Config>
NODISCARD inline std::array<Piece_Class, PIECE_CLASS_NB> storage_within_class_order(
	const Config& cfg,
	uint32_t perm)
{
	ASSERT(index_permutation_config_is_valid(cfg, perm));
	const size_t n = cfg.num_populated_classes();

	std::array<Piece_Class, PIECE_CLASS_NB> available{};
	for (size_t i = 0; i < n; ++i)
		available[i] = cfg.populated_classes()[i];

	std::array<Piece_Class, PIECE_CLASS_NB> order{};
	uint32_t idx = perm;
	for (size_t i = 0; i < n; ++i)
	{
		const uint32_t f = FACTORIAL[n - 1 - i];  // entries are all >= 1
		const size_t pick = idx / f;
		idx %= f;
		ASSERT(pick < n - i);
		order[i] = available[pick];
		for (size_t j = pick; j + 1 < n - i; ++j)
			available[j] = available[j + 1];
	}
	return order;
}
