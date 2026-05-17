#pragma once

#include "chess/chess.h"
#include "egtb/piece_config_for_gen.h"

#include "util/defines.h"
#include "util/fixed_vector.h"

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
	// Bound: 8 king-attack squares per color, two colors.
	using Neighbor_List = Fixed_Vector<int32_t, 16>;
	void neighbors(int32_t slice_id, Neighbor_List& out) const;
};
