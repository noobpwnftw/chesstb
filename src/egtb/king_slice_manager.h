#pragma once

#include "chess/chess.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/piece_group.h"

#include "util/defines.h"
#include "util/param.h"

// Dihedral-8 transforms as a 3-bit bitfield (file | rank<<1 | diag<<2),
// applied file -> rank -> diag.
enum struct Symmetry_Transform : uint8_t {
	IDENTITY       = 0,
	FILE           = 1,
	RANK           = 2,
	FILE_RANK      = 3,  // 180-degree rotation
	DIAG           = 4,
	FILE_DIAG      = 5,
	RANK_DIAG      = 6,
	FILE_RANK_DIAG = 7,  // anti-diagonal flip
	NB             = 8
};

NODISCARD constexpr Square apply_transform(Square sq, Symmetry_Transform t)
{
	int f = sq_file(sq);
	int r = sq_rank(sq);
	const uint8_t bits = static_cast<uint8_t>(t);
	if (bits & 1) f = 7 - f;
	if (bits & 2) r = 7 - r;
	if (bits & 4) std::swap(f, r);
	return sq_make(static_cast<Rank>(r), static_cast<File>(f));
}

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

	NODISCARD const Pair_Lookup& lookup(Square wk, Square bk) const
	{
		return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
	}

	// Slice IDs reachable by moving exactly one king one step. Used for
	// retrograde planning; predecessors land here or in `slice_id` itself.
	// Sorted and deduped, and a pure function of slice_id -- so it is built once
	// into a CSR table at construction rather than rescanning both kings' attack
	// sets and re-sorting on every call.
	NODISCARD Const_Span<int32_t> neighbors(int32_t slice_id) const
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

// Apply transform to every Square in every placement (kings, pawns, others).
INLINE void apply_transform_to_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Piece_Config_For_Gen& epsi,
	Symmetry_Transform t)
{
	if (t == Symmetry_Transform::IDENTITY) return;

	auto xform = [t](Square s) { return apply_transform(s, t); };

	(*placements)[WHITE_KINGS] = (*placements)[WHITE_KINGS].with_transformed_squares(xform);
	(*placements)[BLACK_KINGS] = (*placements)[BLACK_KINGS].with_transformed_squares(xform);

	// Transform whatever pawns are present. The placement slot also holds the
	// opposing pair's pawn (folded in with the free pawns), so key off the actual
	// contents rather than is_populated -- a pair-only material has no free-pawn
	// class but still has pawns to orient.
	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
		if ((*placements)[c].size() > 0)
			(*placements)[c] = (*placements)[c].with_transformed_squares(xform);

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		auto& p = (*placements)[c];
		p = p.with_transformed_squares(xform);
	}
}

INLINE void canonicalize_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Piece_Config_For_Gen& epsi)
{
	const Square wk = (*placements)[WHITE_KINGS][0];
	const Square bk = (*placements)[BLACK_KINGS][0];
	const auto& lookup = epsi.king_slice_manager().lookup(wk, bk);

	if (lookup.slice_id == SLICE_NONE) return;

	apply_transform_to_placements(placements, epsi, lookup.transform);

	if (lookup.has_diag_stabilizer)
	{
		auto xform = [](Square s) { return apply_transform(s, Symmetry_Transform::DIAG); };

		size_t within_cur = 0;
		size_t within_alt = 0;
		for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
		{
			const Piece_Class c = epsi.populated_classes()[i];
			const auto& cur = (*placements)[c];
			const auto alt = cur.with_transformed_squares(xform);
			within_cur += epsi.weight(c) * epsi.group(c).compound_index(cur);
			within_alt += epsi.weight(c) * epsi.group(c).compound_index(alt);
		}

		if (within_alt < within_cur)
		{
			// Kings are fixed by the stabilizer, so only non-king classes flip.
			for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
			{
				const Piece_Class c = epsi.populated_classes()[i];
				(*placements)[c] = (*placements)[c].with_transformed_squares(xform);
			}
		}
	}
}
