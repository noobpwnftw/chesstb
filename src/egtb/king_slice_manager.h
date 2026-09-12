#pragma once

#include "chess/chess.h"
#include "chess/castling_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/piece_group.h"

#include "util/defines.h"
#include "util/param.h"

#include <memory>

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

	// Castling managers only.
	bool has_castling = false;
	std::array<size_t, COLOR_NB> castling_rights{ 0, 0 };
	// Parallel to kings_of_slice: each side's castling rooks, ascending by file.
	using Slice_Rooks = std::array<std::array<Square, Castling_Group::MAX_RIGHTS>, COLOR_NB>;
	std::vector<Slice_Rooks> castling_rooks_of_slice;
	std::vector<Castling_Group::Index> castling_index_of_slice;
	// [castle placement * SQUARE_NB + free king square] -> slice id. With both
	// kings pinned there is no free king, and the placement indexes it directly.
	std::vector<int32_t> castling_slice_lookup;

	explicit King_Slice_Manager(Symmetry_Group s);
	King_Slice_Manager(Symmetry_Group s, size_t white_rights, size_t black_rights);

	NODISCARD const Castling_Group& castling_group() const { return *m_castling_group; }
	NODISCARD bool both_kings_pinned() const { return m_castling_group->both_kings_pinned(); }

	NODISCARD const Pair_Lookup& lookup(Square wk, Square bk) const
	{
		ASSERT(!has_castling);
		return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
	}

	// Slice for a castling material, given both kings and each side's castling
	// rooks. The rook lists must be ascending by file.
	NODISCARD int32_t castling_slice_of(Square wk, Square bk,
	                                  Const_Span<Square> white_rooks,
	                                  Const_Span<Square> black_rooks) const
	{
		ASSERT(has_castling);
		const Square king_sq[COLOR_NB] = { wk, bk };
		const Const_Span<Square>* rooks[COLOR_NB] = { &white_rooks, &black_rooks };

		File rook_files[COLOR_NB][Castling_Group::MAX_RIGHTS];
		File king_file[COLOR_NB] = { FILE_END, FILE_END };
		for (const Color c : { WHITE, BLACK })
		{
			if (rooks[c]->size() != castling_rights[c]) return SLICE_NONE;
			if (castling_rights[c] == 0) continue;

			const Rank home = castling_home_rank(c);
			if (sq_rank(king_sq[c]) != home) return SLICE_NONE;
			king_file[c] = sq_file(king_sq[c]);

			for (size_t i = 0; i < rooks[c]->size(); ++i)
			{
				if (sq_rank((*rooks[c])[i]) != home) return SLICE_NONE;
				rook_files[c][i] = sq_file((*rooks[c])[i]);
			}
		}

		const Castling_Group::Index ci = m_castling_group->index_of(
			king_file[WHITE], Const_Span<File>(rook_files[WHITE], castling_rights[WHITE]),
			king_file[BLACK], Const_Span<File>(rook_files[BLACK], castling_rights[BLACK]));
		if (ci == Castling_Group::INDEX_NONE)
			return SLICE_NONE;

		const size_t free_slot = both_kings_pinned()
			? size_t(0)
			: static_cast<size_t>(king_sq[castling_rights[WHITE] == 0 ? WHITE : BLACK]);
		return castling_slice_lookup[static_cast<size_t>(ci) * m_free_king_stride + free_slot];
	}

	NODISCARD int32_t castling_slice_with_free_king(int32_t slice_id, Square free_king) const
	{
		ASSERT(has_castling && !both_kings_pinned());
		return castling_slice_lookup[
			static_cast<size_t>(castling_index_of_slice[slice_id]) * m_free_king_stride
			+ static_cast<size_t>(free_king)];
	}

	NODISCARD Const_Span<int32_t> neighbors(int32_t slice_id) const
	{
		if (slice_id < 0 || static_cast<size_t>(slice_id) >= num_slices)
			return Const_Span<int32_t>(m_neighbor_data.data(), size_t(0));
		const size_t lo = m_neighbor_off[static_cast<size_t>(slice_id)];
		const size_t hi = m_neighbor_off[static_cast<size_t>(slice_id) + 1];
		return Const_Span<int32_t>(m_neighbor_data.data() + lo, hi - lo);
	}

private:
	void build_castling_slices();
	void build_neighbor_table();
	void build_castling_neighbor_table();

	std::unique_ptr<Castling_Group> m_castling_group;
	// SQUARE_NB while one king is free, 1 when both are pinned.
	size_t m_free_king_stride = 1;

	std::vector<int32_t>  m_neighbor_data;  // CSR values, sorted per slice
	std::vector<uint32_t> m_neighbor_off;   // CSR offsets, size num_slices + 1
};

INLINE void apply_transform_to_placements(
	In_Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements,
	const Piece_Config_For_Gen& epsi,
	Symmetry_Transform t)
{
	if (t == Symmetry_Transform::IDENTITY) return;

	auto xform = [t](Square s) { return apply_transform(s, t); };

	(*placements)[WHITE_KINGS] = (*placements)[WHITE_KINGS].with_transformed_squares(xform);
	(*placements)[BLACK_KINGS] = (*placements)[BLACK_KINGS].with_transformed_squares(xform);

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
	ASSERT(!epsi.king_slice_manager().has_castling);

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
			for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
			{
				const Piece_Class c = epsi.populated_classes()[i];
				(*placements)[c] = (*placements)[c].with_transformed_squares(xform);
			}
		}
	}
}
