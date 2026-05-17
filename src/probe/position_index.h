#pragma once

#include "chess/chess.h"
#include "chess/index_permutation.h"
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

// Opposing pawn pair (lowercase 'p'). Standalone copy of the generator's
// Pair_Group: the enumeration order, index_of, and find_canonical tie-break MUST
// match egtb/pair_group.h exactly, since the on-disk pawn-slice ids depend on
// them (same duplicated-but-identical contract as Piece_Group). See the comments
// there for the model: white on rank r, black on rank s, r < s, same file.
struct Pair_Group
{
	using Index = uint32_t;

	Pair_Group()
	{
		m_inverse.fill(-1);
		for (int f = 0; f < 8; ++f)
		{
			const File file = static_cast<File>(f);
			for (int wr = static_cast<int>(RANK_2); wr <= static_cast<int>(RANK_6); ++wr)
				for (int br = wr + 1; br <= static_cast<int>(RANK_7); ++br)
				{
					const Square w = sq_make(static_cast<Rank>(wr), file);
					const Square b = sq_make(static_cast<Rank>(br), file);
					m_inverse[inv_key(w, b)] = static_cast<int32_t>(m_white.size());
					m_white.push_back(w);
					m_black.push_back(b);
				}
		}
	}

	NODISCARD size_t table_size() const { return m_white.size(); }
	NODISCARD Square white_square(Index i) const { return m_white[i]; }
	NODISCARD Square black_square(Index i) const { return m_black[i]; }

	NODISCARD Index index_of(Square white_sq, Square black_sq) const
	{
		const int32_t idx = m_inverse[inv_key(white_sq, black_sq)];
		ASSERT(idx >= 0);
		return static_cast<Index>(idx);
	}

	NODISCARD static bool is_opposing_pair(Square white_sq, Square black_sq)
	{
		if (sq_file(white_sq) != sq_file(black_sq))
			return false;
		const int wr = static_cast<int>(sq_rank(white_sq));
		const int br = static_cast<int>(sq_rank(black_sq));
		return wr >= static_cast<int>(RANK_2)
		    && br <= static_cast<int>(RANK_7)
		    && wr < br;
	}

	// For callers that know an opposing pair is present (indexing a stored
	// pair-table position): set the out squares, asserting one exists.
	static void canonical_pair(Const_Span<Square> white_sqs,
	                           Const_Span<Square> black_sqs,
	                           Square& out_white, Square& out_black)
	{
		const bool ok = find_canonical(white_sqs, black_sqs, out_white, out_black);
		ASSERT(ok);
		(void)ok;
	}

	NODISCARD static bool find_canonical(Const_Span<Square> white_sqs,
	                                     Const_Span<Square> black_sqs,
	                                     Square& out_white, Square& out_black)
	{
		bool found = false;
		for (size_t i = 0; i < white_sqs.size(); ++i)
			for (size_t j = 0; j < black_sqs.size(); ++j)
			{
				const Square w = white_sqs[i], b = black_sqs[j];
				if (!is_opposing_pair(w, b))
					continue;
				if (!found || less_pair(w, b, out_white, out_black))
				{
					out_white = w;
					out_black = b;
					found = true;
				}
			}
		return found;
	}

private:
	NODISCARD static int inv_key(Square w, Square b)
	{
		return static_cast<int>(w) * 64 + static_cast<int>(b);
	}

	NODISCARD static bool less_pair(Square w1, Square b1, Square w2, Square b2)
	{
		if (sq_file(w1) != sq_file(w2)) return sq_file(w1) < sq_file(w2);
		if (sq_rank(w1) != sq_rank(w2)) return sq_rank(w1) < sq_rank(w2);
		return sq_rank(b1) < sq_rank(b2);
	}

	std::vector<Square> m_white;
	std::vector<Square> m_black;
	std::array<int32_t, 64 * 64> m_inverse;
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
	bool m_has_pawns = false;
	size_t m_num_slices = 1;
	size_t m_num_cartesian_slices = 1;
	size_t m_pair_table_size = 1;
	size_t m_white_table_size = 1;
	size_t m_black_table_size = 1;

	std::vector<int32_t> m_storage_by_cartesian;
	std::vector<int32_t> m_cartesian_by_storage;

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
NODISCARD inline Index_Storage_Layout make_index_storage_layout(
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
		const auto counts = ps.piece_counts();
		const bool has_pawns = counts[WHITE_PAWN] > 0 || counts[BLACK_PAWN] > 0
		                    || ps.has_frozen_pair();
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
