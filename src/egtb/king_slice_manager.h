#pragma once

#include "chess/chess.h"
#include "chess/castling_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/piece_group.h"

#include "util/defines.h"
#include "util/param.h"

#include <memory>

// Dihedral-8 transforms as a 3-bit mask on every square of `b`, applied in
// order: 1 mirrors the files, 2 the ranks, 4 then the a1-h8 diagonal.
NODISCARD constexpr Bitboard apply_transform(Bitboard b, uint8_t t)
{
	if (t & 1) b = b.mirror_files();
	if (t & 2) b = b.mirror_ranks();
	if (t & 4) b = b.mirror_diag();
	return b;
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
	size_t num_slices;

	// Raw (wk, bk) to canonical slice_id and transform.
	struct Pair_Lookup
	{
		int32_t slice_id;             // SLICE_NONE if illegal
		uint8_t transform;            // apply_transform's mask, 0: none
		uint8_t has_diag_stabilizer;  // residual {id, diag} stabilizer present
	};
	std::array<Pair_Lookup, SQUARE_NB * SQUARE_NB> pair_lookup{};

	std::vector<std::pair<Square, Square>> kings_of_slice;
	// True iff both kings on the long diagonal.
	std::vector<uint8_t> slice_has_stabilizer;

	// Castling managers only.
	bool has_castling = false;
	std::array<size_t, COLOR_NB> castling_rights{ 0, 0 };
	// Parallel to kings_of_slice: each side's castling rooks.
	using Slice_Rooks = std::array<Bitboard, COLOR_NB>;
	std::vector<Slice_Rooks> castling_rooks_of_slice;
	std::vector<Castling_Group::Index> castling_index_of_slice;
	// [castle placement * SQUARE_NB + free king square] -> slice id. With both
	// kings pinned there is no free king, and the placement indexes it directly.
	std::vector<int32_t> castling_slice_lookup;

	explicit King_Slice_Manager(Symmetry_Group s);
	King_Slice_Manager(Symmetry_Group s, size_t white_rights, size_t black_rights);

	NODISCARD const Castling_Group& castling_group() const { return *m_castling_group; }

	NODISCARD const Pair_Lookup& lookup(Square wk, Square bk) const
	{
		ASSERT(!has_castling);
		return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
	}

	// Slice for a castling material, given both kings and each side's castling
	// rooks; SLICE_NONE if they are not on their home rank or not a placement
	// of the rights.
	NODISCARD int32_t castling_slice_of(Square wk, Square bk,
	                                  Bitboard white_rooks, Bitboard black_rooks) const
	{
		ASSERT(has_castling);
		const Square king_sq[COLOR_NB] = { wk, bk };
		const Bitboard rooks[COLOR_NB] = { white_rooks, black_rooks };

		File king_file[COLOR_NB] = { FILE_END, FILE_END };
		uint8_t rook_mask[COLOR_NB] = { 0, 0 };
		for (const Color c : { WHITE, BLACK })
		{
			if (castling_rights[c] == 0)
			{
				if (rooks[c]) return SLICE_NONE;
				continue;
			}
			const Rank home = castling_home_rank(c);
			if (sq_rank(king_sq[c]) != home || (rooks[c] & ~home)) return SLICE_NONE;
			king_file[c] = sq_file(king_sq[c]);
			rook_mask[c] = static_cast<uint8_t>(rooks[c].bits() >> (8 * home));
		}

		const Castling_Group::Index ci = m_castling_group->index_of(
			king_file[WHITE], rook_mask[WHITE], king_file[BLACK], rook_mask[BLACK]);
		if (ci == Castling_Group::INDEX_NONE)
			return SLICE_NONE;

		const size_t free_slot = m_castling_group->both_kings_pinned()
			? size_t(0)
			: static_cast<size_t>(king_sq[castling_rights[WHITE] == 0 ? WHITE : BLACK]);
		return castling_slice_lookup[static_cast<size_t>(ci) * m_free_king_stride + free_slot];
	}

	NODISCARD int32_t castling_slice_with_free_king(int32_t slice_id, Square free_king) const
	{
		ASSERT(has_castling && !m_castling_group->both_kings_pinned());
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

