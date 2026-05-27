#pragma once

#include "chess/chess.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/span.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

enum Board_Index : size_t {
	BOARD_INDEX_NONE = std::numeric_limits<size_t>::max()
};

enum struct Symmetry_Group : uint8_t {
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
		NODISCARD INLINE Placement with_transformed_squares(F&& f) const
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

	NODISCARD INLINE size_t table_size() const { return m_table_size; }
	NODISCARD INLINE Piece piece() const { return m_piece; }

private:
	Piece m_piece;
	size_t m_num_pieces;
	size_t m_num_legal_squares;
	uint64_t m_table_size;

	int8_t m_sq_to_pos[SQUARE_NB];
	Square m_pos_to_sq[SQUARE_NB];
};

using Within_Slice_Index = std::array<Piece_Group::Placement_Index, PIECE_CLASS_NB>;

struct Decomposed_Board_Index
{
	int32_t pawn_slice_id = 0;
	int32_t king_slice_id = 0;
	Within_Slice_Index within{};
};

constexpr int32_t SLICE_NONE = -1;

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

	explicit King_Slice_Manager(Symmetry_Group s);

	NODISCARD INLINE const Pair_Lookup& lookup(Square wk, Square bk) const
	{
		return pair_lookup[static_cast<int>(wk) * SQUARE_NB + static_cast<int>(bk)];
	}
};

struct Pawn_Slice_Manager
{
	Pawn_Slice_Manager() = default;
	Pawn_Slice_Manager(const Piece_Group* white_pawns,
	                   const Piece_Group* black_pawns);

	NODISCARD size_t num_slices() const { return m_num_slices; }
	NODISCARD bool has_pawns() const { return m_has_pawns; }

	struct Decomposed
	{
		Piece_Group::Placement_Index white_idx;
		Piece_Group::Placement_Index black_idx;
	};

	NODISCARD Decomposed decompose(int32_t slice_id) const;
	NODISCARD int32_t compose(Piece_Group::Placement_Index w,
	                          Piece_Group::Placement_Index b) const;
	NODISCARD int32_t lookup_from_squares(
		Const_Span<Square> white_pawn_squares,
		Const_Span<Square> black_pawn_squares) const;

private:
	bool m_has_pawns = false;
	size_t m_num_slices = 1;
	size_t m_num_cartesian_slices = 1;
	size_t m_white_table_size = 1;
	size_t m_black_table_size = 1;

	std::vector<int32_t> m_storage_by_cartesian;
	std::vector<int32_t> m_cartesian_by_storage;

	const Piece_Group* m_white_group = nullptr;
	const Piece_Group* m_black_group = nullptr;
};

struct Position_Index_Config : public Piece_Config
{
private:
	static constexpr size_t MAX_NUM_POSITIONS = 0xffffffffffffull;

	NODISCARD static Symmetry_Group pick_symmetry(const Piece_Config& ps)
	{
		const auto counts = ps.piece_counts();
		const bool has_pawns = counts[WHITE_PAWN] > 0 || counts[BLACK_PAWN] > 0;
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
	NODISCARD const King_Slice_Manager& slice_manager() const { return *m_king_slice_manager; }
	NODISCARD const Pawn_Slice_Manager& pawn_slice_manager() const { return *m_pawn_slice_manager; }

	NODISCARD const Piece_Group& group(Piece_Class c) const
	{
		ASSERT(m_groups[c] != nullptr);
		return *m_groups[c];
	}

	NODISCARD bool is_populated(Piece_Class c) const { return m_groups[c] != nullptr; }
	NODISCARD const Piece_Class* populated_classes() const { return m_populated_classes; }
	NODISCARD size_t num_populated_classes() const { return m_num_populated_classes; }
	NODISCARD size_t weight(Piece_Class c) const { return m_weights[c]; }

	NODISCARD Board_Index compose_board_index(const Decomposed_Board_Index& idx) const
	{
		size_t within = 0;
		for (size_t i = 0; i < m_num_populated_classes; ++i)
		{
			const Piece_Class ix = m_populated_classes[i];
			within += m_weights[ix] * static_cast<size_t>(idx.within[ix]);
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
	const Position_Index_Config& cfg, const Position& pos);

NODISCARD bool position_from_index(
	const Position_Index_Config& cfg, Board_Index idx, Color turn,
	Out_Param<Position> pos);
