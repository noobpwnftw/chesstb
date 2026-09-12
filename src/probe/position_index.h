#pragma once

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

enum Board_Index : size_t {
	BOARD_INDEX_ZERO = 0,
	BOARD_INDEX_NONE = std::numeric_limits<size_t>::max()
};

enum struct Symmetry_Group : uint8_t {
	NONE,
	FILE_MIRROR,
	DIHEDRAL_8,
};

enum struct Symmetry_Transform : uint8_t {
	IDENTITY       = 0,
	FILE           = 1,
	RANK           = 2,
	FILE_RANK      = 3,
	DIAG           = 4,
	FILE_DIAG      = 5,
	RANK_DIAG      = 6,
	FILE_RANK_DIAG = 7
};

struct Piece_Group
{
	static constexpr size_t MAX_PIECE_GROUP_SIZE = 7;

	using Placement_Index = uint32_t;

	struct alignas(8) Placement
	{
		Placement() : m_size(0) {}

		template <typename F>
		NODISCARD Placement with_transformed_squares(F&& f) const
		{
			Placement dst;
			dst.m_size = m_size;
			for (size_t i = 0; i < static_cast<size_t>(m_size); ++i)
				dst.m_squares[i] = f(m_squares[i]);
			dst.sort();
			return dst;
		}

		INLINE void add(Square s)
		{
			ASSERT(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
			size_t i = m_size;
			while (i > 0 && m_squares[i - 1] > s)
			{
				m_squares[i] = m_squares[i - 1];
				--i;
			}
			m_squares[i] = s;
			m_size += 1;
		}

		INLINE void add_unsorted(Square s)
		{
			ASSERT(static_cast<size_t>(m_size) < MAX_PIECE_GROUP_SIZE);
			m_squares[m_size++] = s;
		}

		INLINE void clear() { m_size = 0; }

		// Lift one square out, keeping the rest in order: how a castling rook
		// leaves its color's rook class, which indexes only the free ones. A
		// square that is not here is left alone rather than driving m_size
		// negative -- see the generator's copy.
		INLINE void remove_square(Square s)
		{
			const size_t n = static_cast<size_t>(m_size);
			size_t i = 0;
			while (i < n && m_squares[i] != s) ++i;
			if (i == n) return;
			for (; i + 1 < n; ++i) m_squares[i] = m_squares[i + 1];
			m_size -= 1;
		}

		INLINE void sort()
		{
			for (size_t i = 1; i < static_cast<size_t>(m_size); ++i)
			{
				const Square v = m_squares[i];
				size_t j = i;
				while (j > 0 && m_squares[j - 1] > v)
				{
					m_squares[j] = m_squares[j - 1];
					--j;
				}
				m_squares[j] = v;
			}
		}

		NODISCARD INLINE Square operator[](size_t i) const
		{
			ASSERT(i < static_cast<size_t>(m_size));
			return m_squares[i];
		}

		NODISCARD INLINE size_t size() const { return m_size; }
		NODISCARD INLINE const Square* begin() const { return m_squares; }

	private:
		Square m_squares[MAX_PIECE_GROUP_SIZE];
		int8_t m_size;
	};

	Piece_Group(Piece pc, size_t count);

	NODISCARD Placement_Index compound_index(const Placement& list) const;
	NODISCARD Placement squares(Placement_Index idx) const;

	NODISCARD size_t table_size() const { return m_table_size; }
	NODISCARD size_t size() const { return m_num_pieces; }
	NODISCARD Piece piece() const { return m_piece; }

private:
	Piece m_piece;
	size_t m_num_pieces;
	size_t m_num_legal_squares;
	uint32_t m_table_size;

	int8_t m_sq_to_pos[SQUARE_NB];
	Square m_pos_to_sq[SQUARE_NB];
};


using Within_Slice_Index = std::array<Piece_Group::Placement_Index, PIECE_CLASS_NB>;

struct Decomposed_Board_Index
{
	int32_t pawn_slice_id;
	int32_t king_slice_id;
	Within_Slice_Index within;
};

inline constexpr int32_t SLICE_NONE = -1;

struct King_Slice_Manager
{
	Symmetry_Group sym;
	size_t num_slices = 0;

	struct Pair_Lookup
	{
		int32_t slice_id;
		Symmetry_Transform transform;
		uint8_t has_diag_stabilizer;
	};

	std::array<Pair_Lookup, SQUARE_NB * SQUARE_NB> pair_lookup{};
	std::vector<std::pair<Square, Square>> kings_of_slice;

	// Castling managers only. Must enumerate exactly as the generator's does --
	// the slice ids are on disk. Both sides share Castling_Group for that reason,
	// so only this loop is duplicated, not the placement order.
	bool has_castling = false;
	std::array<size_t, COLOR_NB> castling_rights{ 0, 0 };
	using Slice_Rooks = std::array<std::array<Square, Castling_Group::MAX_RIGHTS>, COLOR_NB>;
	std::vector<Slice_Rooks> castling_rooks_of_slice;
	std::vector<Castling_Group::Index> castling_index_of_slice;
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
		if (ci == Castling_Group::INDEX_NONE) return SLICE_NONE;

		const size_t free_slot = both_kings_pinned()
			? size_t(0)
			: static_cast<size_t>(king_sq[castling_rights[WHITE] == 0 ? WHITE : BLACK]);
		return castling_slice_lookup[static_cast<size_t>(ci) * m_free_king_stride + free_slot];
	}

private:
	std::shared_ptr<Castling_Group> m_castling_group;
	size_t m_free_king_stride = 1;
};

inline constexpr size_t MAX_TOTAL_PAWNS = 6;

struct Pawn_Slice_Manager
{
	Pawn_Slice_Manager(const Pair_Group* pair,
	                   const Piece_Group* white_pawns,
	                   const Piece_Group* black_pawns);

	NODISCARD size_t num_slices() const { return m_num_slices; }
	NODISCARD bool has_pawns() const { return m_has_pawns; }
	NODISCARD bool has_pair() const { return m_pair_group != nullptr; }

	struct Decomposed
	{
		Pair_Group::Index            pair_idx;
		Piece_Group::Placement_Index white_idx;
		Piece_Group::Placement_Index black_idx;
	};

	NODISCARD Decomposed decompose(int32_t slice_id) const;
	NODISCARD int32_t compose(Pair_Group::Index pair,
	                          Piece_Group::Placement_Index w,
	                          Piece_Group::Placement_Index b) const;
	NODISCARD int32_t lookup_from_squares(
		Square pair_white_sq, Square pair_black_sq,
		Const_Span<Square> white_pawn_squares,
		Const_Span<Square> black_pawn_squares) const;

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
	Divider<uint64_t> m_pair_table_size_div{};
	Divider<uint64_t> m_white_table_size_div{};

	std::vector<Rank_Block> m_rank_blocks;
	std::vector<int32_t>    m_cartesian_by_storage;

	const Pair_Group*  m_pair_group  = nullptr;
	const Piece_Group* m_white_group = nullptr;
	const Piece_Group* m_black_group = nullptr;
};

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
	static constexpr size_t MAX_NUM_POSITIONS = 0xffffffffffffull;

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

	NODISCARD size_t num_positions() const { return m_num_positions; }
	NODISCARD size_t within_slice_size() const { return m_within_slice_size; }
	NODISCARD const King_Slice_Manager& king_slice_manager() const { return *m_king_slice_manager; }
	NODISCARD const Pawn_Slice_Manager& pawn_slice_manager() const { return *m_pawn_slice_manager; }
	NODISCARD const Pair_Group* pair_group() const { return m_pair_group.get(); }

	// Squares one side's castling rooks stand on in `slice`, ascending by file.
	NODISCARD Const_Span<Square> castling_rook_squares(int32_t king_slice_id, Color c) const
	{
		ASSERT(has_castling());
		const auto& rooks = m_king_slice_manager->castling_rooks_of_slice[king_slice_id][c];
		return Const_Span<Square>(rooks.data(), castling_rights(c));
	}

	NODISCARD const Piece_Group& group(Piece_Class c) const
	{
		ASSERT(m_groups[c] != nullptr);
		return *m_groups[c];
	}

	NODISCARD bool is_populated(Piece_Class c) const { return m_groups[c] != nullptr; }
	NODISCARD const Piece_Class* populated_classes() const { return m_populated_classes; }
	NODISCARD size_t num_populated_classes() const { return m_num_populated_classes; }
	NODISCARD size_t weight(Piece_Class c) const { return m_weights[c]; }

	NODISCARD Board_Index compose_board_index(
		const Decomposed_Board_Index& idx,
		const Index_Storage_Layout& layout) const
	{
		size_t within = 0;
		size_t w = 1;
		for (size_t i = 0; i < layout.n; ++i)
		{
			within += w * static_cast<size_t>(idx.within[layout.order[i]]);
			w *= layout.radix[i];
		}
		const size_t outer = static_cast<size_t>(idx.pawn_slice_id) * m_pawn_slice_stride
		                   + static_cast<size_t>(idx.king_slice_id) * m_within_slice_size;
		return static_cast<Board_Index>(outer + within);
	}

	bool fill_board(const Decomposed_Board_Index& index, Out_Param<Position> board) const;

	void decompose_board_index(Board_Index pos, Out_Param<Decomposed_Board_Index> idx) const;

private:
	const King_Slice_Manager* m_king_slice_manager = nullptr;
	std::unique_ptr<Pawn_Slice_Manager> m_pawn_slice_manager;
	std::unique_ptr<Pair_Group> m_pair_group;  // null unless this material has 'p'

	size_t m_num_pawn_slices = 1;
	size_t m_pawn_slice_stride = 0;
	size_t m_within_slice_size = 0;
	size_t m_num_positions = 0;

	size_t m_num_populated_classes = 0;
	Piece_Class m_populated_classes[PIECE_CLASS_NB]{};

	std::unique_ptr<Piece_Group> m_groups_owned[PIECE_CLASS_NB];
	const Piece_Group* m_groups[PIECE_CLASS_NB]{};
	size_t m_weights[PIECE_CLASS_NB]{};
	int8_t m_piece_counts_cached[PIECE_NB]{};
};

NODISCARD Board_Index board_index_of_position(
	const Position_Index_Config& cfg,
	const Index_Storage_Layout& layout,
	const Position& pos);

NODISCARD bool position_from_index(
	const Position_Index_Config& cfg, Board_Index idx, Color turn,
	Out_Param<Position> pos);
