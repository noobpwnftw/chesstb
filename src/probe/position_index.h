#pragma once

#include "chess/bitboard.h"
#include "chess/castling_group.h"
#include "chess/pair_group.h"
#include "chess/chess.h"
#include "chess/index_permutation.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/division.h"
#include "util/intrin.h"
#include "util/param.h"
#include "util/span.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

enum Board_Index : uint64_t {
	BOARD_INDEX_ZERO = 0,
	BOARD_INDEX_NONE = std::numeric_limits<uint64_t>::max()
};

enum struct Symmetry_Group : uint8_t {
	NONE,
	FILE_MIRROR,
	DIHEDRAL_8,
};

struct Piece_Group
{
	static constexpr size_t MAX_PIECE_GROUP_SIZE = 7;

	using Placement_Index = uint32_t;

	Piece_Group(Piece pc, size_t count);

	// Colex rank of the pieces on `b`; the legal squares run on from the first.
	NODISCARD Placement_Index index_of(Bitboard b) const;
	// The pieces of placement `idx`.
	NODISCARD Bitboard squares(Placement_Index idx) const;

	NODISCARD size_t table_size() const { return m_table_size; }
	NODISCARD size_t size() const { return m_num_pieces; }
	NODISCARD Piece piece() const { return m_piece; }

private:
	Piece m_piece;
	size_t m_num_pieces;
	size_t m_num_legal_squares;
	Square m_first_square;
	uint32_t m_table_size;
};

#ifdef CHESSTB_INDEX_INVERSE
using Within_Slice_Index = std::array<Piece_Group::Placement_Index, PIECE_CLASS_NB>;

struct Decomposed_Board_Index
{
	int32_t pawn_slice_id;
	int32_t king_slice_id;
	Within_Slice_Index within;
};
#endif

inline constexpr int32_t SLICE_NONE = -1;

struct King_Slice_Manager
{
	size_t num_slices = 0;

	struct Pair_Lookup
	{
		int32_t slice_id;
		// Bits: 1 mirrors files, 2 ranks, 4 then the a1-h8 diagonal.
		uint8_t transform;
		uint8_t has_diag_stabilizer;
	};

	std::array<Pair_Lookup, SQUARE_NB * SQUARE_NB> pair_lookup{};
#ifdef CHESSTB_INDEX_INVERSE
	std::vector<std::pair<Square, Square>> kings_of_slice;
#endif

	// Castling managers only. Must enumerate exactly as the generator's does --
	// the slice ids are on disk. Both sides share Castling_Group for that reason,
	// so only this loop is duplicated, not the placement order.
	bool has_castling = false;
	std::array<size_t, COLOR_NB> castling_rights{ 0, 0 };
#ifdef CHESSTB_INDEX_INVERSE
	using Slice_Rooks = std::array<Bitboard, COLOR_NB>;
	std::vector<Slice_Rooks> castling_rooks_of_slice;
#endif
	std::vector<int32_t> castling_slice_lookup;

	explicit King_Slice_Manager(Symmetry_Group s);
	// Castling: no symmetry.
	King_Slice_Manager(size_t white_rights, size_t black_rights);

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
		if (ci == Castling_Group::INDEX_NONE) return SLICE_NONE;

		const size_t free_slot = m_castling_group->both_kings_pinned()
			? size_t(0)
			: static_cast<size_t>(king_sq[castling_rights[WHITE] == 0 ? WHITE : BLACK]);
		return castling_slice_lookup[static_cast<size_t>(ci) * m_free_king_stride + free_slot];
	}

private:
	std::shared_ptr<Castling_Group> m_castling_group;
	size_t m_free_king_stride = 1;
};

inline constexpr size_t MAX_TOTAL_PAWNS = 6;

// Depends on the pawns only: the opposing pair and each side's other pawns.
struct Pawn_Slice_Manager
{
	Pawn_Slice_Manager(bool has_pair, size_t white_pawns, size_t black_pawns);

	Pawn_Slice_Manager(const Pawn_Slice_Manager&) = delete;
	Pawn_Slice_Manager& operator=(const Pawn_Slice_Manager&) = delete;

	NODISCARD size_t num_slices() const { return m_num_slices; }
	NODISCARD bool has_pawns() const { return m_has_pawns; }

#ifdef CHESSTB_INDEX_INVERSE
	// Index -> position is for the tools only, which define CHESSTB_INDEX_INVERSE:
	// each side's pawns in `slice_id`, the pair's included.
	void pawns_of(int32_t slice_id, Bitboard& white, Bitboard& black) const;
#endif
	// The pair's squares (unused without one) and each side's other pawns.
	NODISCARD int32_t lookup(Square pair_white_sq, Square pair_black_sq,
	                         Bitboard white, Bitboard black) const;

private:
	struct Rank_Block
	{
		int32_t  survivors_before = 0;
		uint64_t bits = 0;
	};

	// Storage id of a cartesian cell: the number of surviving cells below it.
	NODISCARD int32_t storage_of_cartesian(size_t cart) const
	{
		const Rank_Block& blk = m_rank_blocks[cart >> 6];
		const uint64_t below = (uint64_t{1} << (cart & 63)) - 1;
		return blk.survivors_before + static_cast<int32_t>(popcnt(blk.bits & below));
	}
	NODISCARD bool cartesian_survives(size_t cart) const
	{
		return (m_rank_blocks[cart >> 6].bits & (uint64_t{1} << (cart & 63))) != 0;
	}

	bool m_has_pawns = false;
	size_t m_num_slices = 1;
	size_t m_num_cartesian_slices = 1;
	size_t m_pair_table_size = 1;
	size_t m_white_table_size = 1;
	size_t m_black_table_size = 1;
	size_t m_white_free_size = 1;  // without a pair: white's placements per black one
	std::vector<Rank_Block> m_rank_blocks;
#ifdef CHESSTB_INDEX_INVERSE
	Divider<uint64_t> m_pair_table_size_div{};
	Divider<uint64_t> m_white_table_size_div{};
	std::vector<int32_t> m_cartesian_by_storage;
#endif

	std::unique_ptr<const Pair_Group>  m_pair_group;
	std::unique_ptr<const Piece_Group> m_white_group;
	std::unique_ptr<const Piece_Group> m_black_group;
};

// The manager for these pawns, built once and shared by every material that
// has them.
NODISCARD const Pawn_Slice_Manager& pawn_slice_mgr(bool has_pair, size_t white_pawns, size_t black_pawns);

struct Index_Storage_Layout
{
	size_t n = 0;
	std::array<Piece_Class, PIECE_CLASS_NB> order{};
	std::array<size_t, PIECE_CLASS_NB> radix{};
};

template <typename Config>
NODISCARD Index_Storage_Layout make_index_storage_layout(
	const Config& cfg,
	uint32_t perm)
{
	ASSERT(index_permutation_config_is_valid(cfg, perm));

	Index_Storage_Layout layout;
	layout.n = cfg.num_populated_classes();
	const auto order = storage_within_class_order(cfg, perm);
	for (size_t i = 0; i < layout.n; ++i)
	{
		layout.order[i] = order[i];
		layout.radix[i] = cfg.group(order[i]).table_size();
	}
	return layout;
}

struct Position_Index_Config : public Piece_Config
{
private:
	static constexpr uint64_t MAX_NUM_POSITIONS = 0xffffffffffffull;

	NODISCARD static Symmetry_Group pick_symmetry(const Piece_Config& ps)
	{
		if (ps.has_castling())
			return Symmetry_Group::NONE;
		const auto counts = ps.piece_counts();
		const bool has_pawns = counts[WHITE_PAWN] > 0 || counts[BLACK_PAWN] > 0
		                    || ps.has_opposing_pair();
		return has_pawns ? Symmetry_Group::FILE_MIRROR : Symmetry_Group::DIHEDRAL_8;
	}

	NODISCARD bool try_init();

public:
	explicit Position_Index_Config(const Piece_Config& ps);

	Position_Index_Config(const Position_Index_Config&) = delete;
	Position_Index_Config& operator=(const Position_Index_Config&) = delete;
	Position_Index_Config(Position_Index_Config&&) = default;
	Position_Index_Config& operator=(Position_Index_Config&&) = default;

#ifdef CHESSTB_INDEX_INVERSE
	NODISCARD uint64_t num_positions() const { return m_num_positions; }
#endif
	NODISCARD uint64_t pawn_slice_stride() const { return m_pawn_slice_stride; }
	NODISCARD size_t within_slice_size() const { return m_within_slice_size; }
	NODISCARD const King_Slice_Manager& king_slice_manager() const { return *m_king_slice_manager; }
	NODISCARD const Pawn_Slice_Manager& pawn_slice_manager() const { return *m_pawn_slice_manager; }

#ifdef CHESSTB_INDEX_INVERSE
	// One side's castling rooks in `slice`.
	NODISCARD Bitboard castling_rooks(int32_t king_slice_id, Color c) const
	{
		ASSERT(has_castling());
		return m_king_slice_manager->castling_rooks_of_slice[king_slice_id][c];
	}
#endif

	NODISCARD const Piece_Group& group(Piece_Class c) const
	{
		ASSERT(m_groups[c] != nullptr);
		return *m_groups[c];
	}

	NODISCARD bool is_populated(Piece_Class c) const { return m_groups[c] != nullptr; }
	NODISCARD const Piece_Class* populated_classes() const { return m_populated_classes; }
	NODISCARD size_t num_populated_classes() const { return m_num_populated_classes; }
	NODISCARD size_t weight(Piece_Class c) const { return m_weights[c]; }

#ifdef CHESSTB_INDEX_INVERSE
	bool fill_board(const Decomposed_Board_Index& index, Out_Param<Position> board) const;

	void decompose_board_index(Board_Index pos, Out_Param<Decomposed_Board_Index> idx) const;
#endif

private:
	const King_Slice_Manager* m_king_slice_manager = nullptr;
	const Pawn_Slice_Manager* m_pawn_slice_manager = nullptr;

	size_t m_num_pawn_slices = 1;
	uint64_t m_pawn_slice_stride = 0;
	size_t m_within_slice_size = 0;
#ifdef CHESSTB_INDEX_INVERSE
	uint64_t m_num_positions = 0;
#endif

	size_t m_num_populated_classes = 0;
	Piece_Class m_populated_classes[PIECE_CLASS_NB]{};

	std::unique_ptr<const Piece_Group> m_groups[PIECE_CLASS_NB];
	size_t m_weights[PIECE_CLASS_NB]{};
};

NODISCARD Board_Index board_index_of_position(
	const Position_Index_Config& cfg,
	const Index_Storage_Layout& layout,
	const Position& pos);

#ifdef CHESSTB_INDEX_INVERSE
NODISCARD bool position_from_index(
	const Position_Index_Config& cfg, Board_Index idx, Color turn,
	Out_Param<Position> pos);
#endif
