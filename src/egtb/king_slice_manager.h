#pragma once

#include "chess/chess.h"
#include "egtb/piece_config_for_gen.h"

#include "util/defines.h"

// Forward decl breaks the circular include with symmetry.h; full definition
// is in symmetry.h, included from king_slice_manager.cpp.
enum struct Symmetry_Transform : uint8_t;

// Partitions position space by canonical (WK, BK). DIHEDRAL_8: 462 slices.
// Retrograde inter-slice edges follow king-attack neighborhoods.

// SLICE_NONE for illegal pairs (overlap / kings adjacent) and non-canonical raw pairs.
enum Slice_Id : int32_t {
	SLICE_ID_ZERO = 0,
	SLICE_NONE    = -1,
};

struct King_Slice_Manager
{
	Symmetry_Group sym;
	size_t num_slices = 0;

	// Raw (wk, bk) to canonical slice_id and transform.
	struct Pair_Lookup
	{
		int32_t slice_id;             // SLICE_NONE if illegal
		Symmetry_Transform transform;
		uint8_t has_diag_stabilizer;  // residual {id, diag} stabilizer present
	};
	std::array<Pair_Lookup, SQUARE_NB * SQUARE_NB> pair_lookup{};

	std::vector<std::pair<Square, Square>> kings_of_slice;
	// True iff both kings on the long diagonal.
	std::vector<uint8_t> slice_has_stabilizer;

	explicit King_Slice_Manager(Symmetry_Group s);

	NODISCARD INLINE const Pair_Lookup& lookup(Square wk, Square bk) const
	{
		return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
	}

	// Slice IDs reachable by moving exactly one king one step. Used for
	// retrograde planning; predecessors land here or in `slice_id` itself.
	// Sorted and deduped, and a pure function of slice_id -- so it is built once
	// into a CSR table at construction rather than rescanning both kings' attack
	// sets and re-sorting on every call.
	NODISCARD INLINE Const_Span<int32_t> neighbors(int32_t slice_id) const
	{
		if (slice_id < 0 || static_cast<size_t>(slice_id) >= num_slices)
			return Const_Span<int32_t>(m_neighbor_data.data(), size_t(0));
		const size_t lo = m_neighbor_off[static_cast<size_t>(slice_id)];
		const size_t hi = m_neighbor_off[static_cast<size_t>(slice_id) + 1];
		return Const_Span<int32_t>(m_neighbor_data.data() + lo, hi - lo);
	}

private:
	void build_neighbor_table();

	std::vector<int32_t>  m_neighbor_data;  // CSR values, sorted per slice
	std::vector<uint32_t> m_neighbor_off;   // CSR offsets, size num_slices + 1
};
