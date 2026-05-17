#pragma once

#include "chess/chess.h"
#include "egtb/piece_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/slice_manager.h"

#include "util/defines.h"
#include "util/param.h"

#include <array>
#include <cstdint>

// Dihedral-8 transforms as a 3-bit bitfield (file | rank<<1 | diag<<2),
// applied file -> rank -> diag.
enum struct Symmetry_Transform : uint8_t {
	IDENTITY      = 0,
	FILE          = 1,
	RANK          = 2,
	FILE_RANK     = 3,  // 180-degree rotation
	DIAG          = 4,
	FILE_DIAG     = 5,
	RANK_DIAG     = 6,
	FILE_RANK_DIAG = 7, // anti-diagonal flip
	NB            = 8
};

NODISCARD INLINE constexpr Square apply_transform(Square sq, Symmetry_Transform t)
{
	int f = sq_file(sq);
	int r = sq_rank(sq);
	const uint8_t bits = static_cast<uint8_t>(t);
	if (bits & 1) f = 7 - f;
	if (bits & 2) r = 7 - r;
	if (bits & 4) std::swap(f, r);
	return sq_make(static_cast<Rank>(r), static_cast<File>(f));
}

// Apply transform to every Square in every placement (kings, pawns, others).
inline void apply_transform_to_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Piece_Config_For_Gen& epsi,
	Symmetry_Transform t)
{
	if (t == Symmetry_Transform::IDENTITY) return;

	auto xform = [t](Square s) { return apply_transform(s, t); };

	(*placements)[WHITE_KINGS] = (*placements)[WHITE_KINGS].with_transformed_squares(xform);
	(*placements)[BLACK_KINGS] = (*placements)[BLACK_KINGS].with_transformed_squares(xform);

	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
		if (epsi.is_populated(c))
			(*placements)[c] = (*placements)[c].with_transformed_squares(xform);

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		auto& p = (*placements)[c];
		p = p.with_transformed_squares(xform);
	}
}

inline Symmetry_Transform canonicalize_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Piece_Config_For_Gen& epsi)
{
	const Square wk = (*placements)[WHITE_KINGS][0];
	const Square bk = (*placements)[BLACK_KINGS][0];
	const auto& lookup = epsi.slice_manager().lookup(wk, bk);

	if (lookup.slice_id == SLICE_NONE)
		return Symmetry_Transform::IDENTITY;

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

	return lookup.transform;
}
