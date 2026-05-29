#pragma once

#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/division.h"

#include <array>
#include <cstddef>
#include <cstdint>

// The index permutation config is just the compact permutation index over the
// material's populated non-pawn classes (see populated_class_order_index). It
// is serialized as a bare uint32; validity is checked by the FACTORIAL range
// bound in index_permutation_config_is_valid, so no separate magic is stored.
constexpr std::array<uint32_t, 9> FACTORIAL = {
	1u, 1u, 2u, 6u, 24u, 120u, 720u, 5040u, 40320u
};

// Preferred final-file class order. The actual file stores this as a compact
// permutation index over the material's populated non-pawn classes.
constexpr std::array<Piece_Class, 8> PREFERRED_STORAGE_WITHIN_CLASS_ORDER = {
	BLACK_KNIGHTS, BLACK_BISHOPS, BLACK_ROOKS, BLACK_QUEENS,
	WHITE_KNIGHTS, WHITE_BISHOPS, WHITE_ROOKS, WHITE_QUEENS,
};

template <typename Config>
NODISCARD inline uint32_t populated_class_order_index(
	const Config& cfg,
	const std::array<Piece_Class, PIECE_CLASS_NB>& storage_order)
{
	const size_t n = cfg.num_populated_classes();
	ASSERT(n <= 8);

	std::array<Piece_Class, PIECE_CLASS_NB> logical{};
	for (size_t i = 0; i < n; ++i)
		logical[i] = cfg.populated_classes()[i];

	uint32_t idx = 0;
	for (size_t i = 0; i < n; ++i)
	{
		size_t pos = i;
		while (pos < n && logical[pos] != storage_order[i])
			++pos;
		ASSERT(pos < n);
		idx += static_cast<uint32_t>(pos - i) * FACTORIAL[n - 1 - i];
		for (size_t j = pos; j > i; --j)
			logical[j] = logical[j - 1];
		logical[i] = storage_order[i];
	}
	return idx;
}

template <typename Config>
NODISCARD inline uint32_t default_index_permutation_config(const Config& cfg)
{
	std::array<Piece_Class, PIECE_CLASS_NB> order{};
	size_t n = 0;
	for (const Piece_Class c : PREFERRED_STORAGE_WITHIN_CLASS_ORDER)
		if (cfg.is_populated(c))
			order[n++] = c;
	ASSERT(n == cfg.num_populated_classes());

	return populated_class_order_index(cfg, order);
}

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

// Loop-invariant conversion data for a fixed (cfg, perm): the perm decode and
// per-class radices/weights, precomputed once via make_index_permutation_plan
// and reused for every index conversion (build at load, then convert per
// position). Holds Dividers so the conversions avoid hardware DIV/MOD.
struct Index_Permutation_Plan
{
	size_t n = 0;
	size_t within = 1;
	Divider<uint64_t> within_div{};                              // valid iff within > 1

	// Storage digit order (perm-decoded) and per-digit radix = table_size.
	std::array<Piece_Class, PIECE_CLASS_NB> storage_order{};
	std::array<size_t, PIECE_CLASS_NB> radix{};
	std::array<Divider<uint64_t>, PIECE_CLASS_NB> radix_div{};   // valid iff radix[i] > 1

	// Logical (populated) order and per-class within-weight.
	std::array<Piece_Class, PIECE_CLASS_NB> logical_order{};
	std::array<size_t, PIECE_CLASS_NB> weight{};
	std::array<Divider<uint64_t>, PIECE_CLASS_NB> weight_div{};  // valid iff weight[i] > 1
};

template <typename Config>
NODISCARD inline Index_Permutation_Plan make_index_permutation_plan(
	const Config& cfg,
	uint32_t perm)
{
	ASSERT(index_permutation_config_is_valid(cfg, perm));

	Index_Permutation_Plan p;
	p.n = cfg.num_populated_classes();
	p.within = cfg.within_slice_size();
	if (p.within > 1)
		p.within_div = Divider<uint64_t>(p.within);

	const auto order = storage_within_class_order(cfg, perm);
	for (size_t i = 0; i < p.n; ++i)
	{
		const Piece_Class c = order[i];
		p.storage_order[i] = c;
		p.radix[i] = cfg.group(c).table_size();
		if (p.radix[i] > 1)
			p.radix_div[i] = Divider<uint64_t>(p.radix[i]);
	}
	for (size_t i = 0; i < p.n; ++i)
	{
		const Piece_Class c = cfg.populated_classes()[i];
		p.logical_order[i] = c;
		p.weight[i] = cfg.weight(c);
		if (p.weight[i] > 1)
			p.weight_div[i] = Divider<uint64_t>(p.weight[i]);
	}
	return p;
}

// Magic-division quotient, remainder by multiply-subtract (cf. egtb_gen.h::split).
NODISCARD inline size_t storage_within_to_logical_within(
	const Index_Permutation_Plan& p,
	size_t storage)
{
	std::array<size_t, PIECE_CLASS_NB> placement{};
	for (size_t i = 0; i < p.n; ++i)
	{
		const size_t r = p.radix[i];
		if (r > 1)
		{
			const size_t q = storage / p.radix_div[i];
			placement[p.storage_order[i]] = storage - q * r;
			storage = q;
		}
		else
		{
			placement[p.storage_order[i]] = 0;
		}
	}
	ASSERT(storage == 0);

	size_t logical = 0;
	for (size_t i = 0; i < p.n; ++i)
		logical += p.weight[i] * placement[p.logical_order[i]];
	return logical;
}

NODISCARD inline size_t logical_within_to_storage_within(
	const Index_Permutation_Plan& p,
	size_t logical)
{
	std::array<size_t, PIECE_CLASS_NB> placement{};
	// First populated class has weight 1; absorb the remainder directly.
	for (ptrdiff_t i = static_cast<ptrdiff_t>(p.n) - 1; i >= 1; --i)
	{
		const size_t q = logical / p.weight_div[i];
		placement[p.logical_order[i]] = q;
		logical -= q * p.weight[i];
	}
	if (p.n > 0)
		placement[p.logical_order[0]] = logical;

	size_t storage = 0;
	size_t w = 1;
	for (size_t i = 0; i < p.n; ++i)
	{
		storage += w * placement[p.storage_order[i]];
		w *= p.radix[i];
	}
	return storage;
}

NODISCARD inline size_t logical_index_to_storage_index(
	const Index_Permutation_Plan& p,
	size_t logical)
{
	if (p.within <= 1) return logical;
	const size_t outer = logical / p.within_div;
	const size_t in = logical - outer * p.within;
	return outer * p.within + logical_within_to_storage_within(p, in);
}

NODISCARD inline size_t storage_index_to_logical_index(
	const Index_Permutation_Plan& p,
	size_t storage)
{
	if (p.within <= 1) return storage;
	const size_t outer = storage / p.within_div;
	const size_t in = storage - outer * p.within;
	return outer * p.within + storage_within_to_logical_within(p, in);
}
