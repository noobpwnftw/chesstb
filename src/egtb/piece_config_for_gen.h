#pragma once

#include "egtb/piece_group.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"
#include "util/division.h"
#include "util/enum.h"
#include "util/param.h"
#include "util/utility.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// Symmetry group applicable to a piece configuration.
enum struct Symmetry_Group : uint8_t {
	FILE_MIRROR,   // pawnful: identity + horizontal flip
	DIHEDRAL_8,    // pawnless: full dihedral
};

// Linear position index. Layout: pawn_slice_id*pawn_slice_stride
// + king_slice_id*within_slice_size + within_slice_idx.
enum Board_Index : size_t {
	BOARD_INDEX_ZERO = 0,
	BOARD_INDEX_NONE = std::numeric_limits<size_t>::max()
};

ENUM_ENABLE_OPERATOR_INC(Board_Index);
ENUM_ENABLE_OPERATOR_DEC(Board_Index);
ENUM_ENABLE_OPERATOR_ADD(Board_Index);
ENUM_ENABLE_OPERATOR_SUB(Board_Index);
ENUM_ENABLE_OPERATOR_DIFF(Board_Index);
ENUM_ENABLE_OPERATOR_ADD_EQ(Board_Index);
ENUM_ENABLE_OPERATOR_SUB_EQ(Board_Index);

// Per-class indices for non-king, non-pawn classes only. Kings are in
// king_slice_id; pawns are in pawn_slice_id.
using Within_Slice_Index = std::array<Piece_Group::Placement_Index, PIECE_CLASS_NB>;

struct Decomposed_Board_Index
{
	int32_t pawn_slice_id = 0;   // 0 for pawnless
	int32_t king_slice_id = 0;
	Within_Slice_Index within{};
};

// White-king fundamental-domain squares per symmetry group.
constexpr Square ANCHOR_FILE_MIRROR_SQUARES[] = {
	SQ_A1, SQ_B1, SQ_C1, SQ_D1,
	SQ_A2, SQ_B2, SQ_C2, SQ_D2,
	SQ_A3, SQ_B3, SQ_C3, SQ_D3,
	SQ_A4, SQ_B4, SQ_C4, SQ_D4,
	SQ_A5, SQ_B5, SQ_C5, SQ_D5,
	SQ_A6, SQ_B6, SQ_C6, SQ_D6,
	SQ_A7, SQ_B7, SQ_C7, SQ_D7,
	SQ_A8, SQ_B8, SQ_C8, SQ_D8,
};
static_assert(sizeof(ANCHOR_FILE_MIRROR_SQUARES) / sizeof(Square) == 32);

constexpr Square ANCHOR_TRIANGLE_SQUARES[] = {
	SQ_A1, SQ_B1, SQ_C1, SQ_D1,
	SQ_B2, SQ_C2, SQ_D2,
	SQ_C3, SQ_D3,
	SQ_D4,
};
static_assert(sizeof(ANCHOR_TRIANGLE_SQUARES) / sizeof(Square) == 10);

struct King_Slice_Manager;
NODISCARD const King_Slice_Manager& get_slice_manager(Symmetry_Group sym);

// Owns Piece_Groups for non-king classes and lays out the slice-based
// Board_Index. Kings and pawns are handled by their respective slice managers.
struct Piece_Config_For_Gen : public Piece_Config
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
	NODISCARD static std::optional<size_t> num_positions_safe(const Piece_Config& ps);

	explicit Piece_Config_For_Gen(const Piece_Config& ps);
	Piece_Config_For_Gen(const Piece_Config& ps, std::nothrow_t);

	Piece_Config_For_Gen(const Piece_Config_For_Gen&) = delete;
	Piece_Config_For_Gen& operator=(const Piece_Config_For_Gen&) = delete;
	Piece_Config_For_Gen(Piece_Config_For_Gen&&) = default;
	Piece_Config_For_Gen& operator=(Piece_Config_For_Gen&&) = default;

	NODISCARD size_t num_positions() const { return m_num_positions; }

	// total = num_pawn_slices * num_king_slices * within_slice_size.
	// Pawnless: num_pawn_slices == 1.
	NODISCARD size_t num_pawn_slices() const { return m_num_pawn_slices; }
	NODISCARD size_t num_king_slices() const { return m_num_king_slices; }
	NODISCARD size_t within_slice_size() const { return m_within_slice_size; }
	NODISCARD size_t pawn_slice_stride() const { return m_pawn_slice_stride; }

	// Fast pid-of-idx: mulh+shift instead of a runtime DIV. Hot in per-cell loops.
	NODISCARD size_t pawn_slice_of(Board_Index idx) const
	{
		if (m_num_pawn_slices <= 1) return 0;
		return static_cast<size_t>(idx) / m_pawn_slice_stride_div;
	}

	NODISCARD size_t num_slices() const
	{
		return m_within_slice_size == 0 ? 0 : m_num_positions / m_within_slice_size;
	}

	NODISCARD Symmetry_Group symmetry() const { return m_symmetry; }
	NODISCARD const King_Slice_Manager& slice_manager() const { return *m_king_slice_manager; }
	NODISCARD const Pawn_Slice_Manager& pawn_slice_manager() const { return *m_pawn_slice_manager; }

	NODISCARD const Piece_Group& group(Piece_Class c) const
	{
		ASSERT(m_groups[c] != nullptr);
		return *m_groups[c];
	}

	NODISCARD bool is_populated(Piece_Class c) const
	{
		return m_groups[c] != nullptr;
	}

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

	// Incremental advance in board-index order, avoiding a full re-decompose.
	void step_to_next(In_Out_Param<Decomposed_Board_Index> idx) const
	{
		for (size_t i = 0; i < m_num_populated_classes; ++i)
		{
			const Piece_Class ix = m_populated_classes[i];
			idx->within[ix] = static_cast<Piece_Group::Placement_Index>(
				idx->within[ix] + 1);
			if (idx->within[ix] < m_num_positions_by_group[ix])
				return;
			idx->within[ix] = Piece_Group::Placement_Index(0);
		}
		idx->king_slice_id += 1;
		if (static_cast<size_t>(idx->king_slice_id) < m_num_king_slices)
			return;
		idx->king_slice_id = 0;
		idx->pawn_slice_id += 1;
	}

	// Materialise the Position at `index`. ASSUME_LEGAL=true skips overlap and
	// slice-range checks and always returns true; =false returns false on conflict.
	// Also populates the canonical-frame placements array (unpopulated classes get
	// a default-constructed empty Placement).
	template <bool ASSUME_LEGAL>
	bool fill_board(const Decomposed_Board_Index& index, Out_Param<Position> board,
	                Out_Param<std::array<Piece_Group::Placement, PIECE_CLASS_NB>> placements) const;

	void decompose_board_index(Board_Index pos, Out_Param<Decomposed_Board_Index> idx) const
	{
		size_t p = static_cast<size_t>(pos);
		if (m_num_pawn_slices > 1)
		{
			const size_t q = p / m_pawn_slice_stride_div;
			idx->pawn_slice_id = static_cast<int32_t>(q);
			p -= q * m_pawn_slice_stride;
		}
		else
		{
			idx->pawn_slice_id = 0;
		}
		if (m_within_slice_size > 1)
		{
			const size_t q = p / m_within_slice_size_div;
			idx->king_slice_id = static_cast<int32_t>(q);
			p -= q * m_within_slice_size;
		}
		else
		{
			idx->king_slice_id = static_cast<int32_t>(p);
			p = 0;
		}
		size_t within = p;
		idx->within.fill(Piece_Group::Placement_Index(0));
		// First populated class has weight 1; absorb remainder directly.
		for (ptrdiff_t i = m_num_populated_classes - 1; i >= 1; --i)
		{
			const Piece_Class ix = m_populated_classes[i];
			ASSERT(m_weights[ix] > 1);
			const size_t group_idx = within / m_weights_div[ix];
			within -= group_idx * m_weights[ix];
			idx->within[ix] = static_cast<Piece_Group::Placement_Index>(group_idx);
		}
		if (m_num_populated_classes > 0)
		{
			idx->within[m_populated_classes[0]] =
				static_cast<Piece_Group::Placement_Index>(within);
		}
	}

	NODISCARD bool both_sides_have_free_attackers() const { return m_both_sides_have_free_attackers; }

	// Raw D8 (pawnless) / D2 (pawnful) board positions represented by one
	// stored cell. Returns 0 for:
	//   - phantom cells (non-canonical half of a diag-stabilizer pair),
	//   - cells where some non-king piece sits on a king square,
	//   - cells where two non-king pieces share a square (cross-class collision).
	// Non-zero results (2/4/8) are the orbit size in the relevant symmetry.
	// Caller must invoke prepare_orbit_weight_table() once first.
	NODISCARD uint8_t orbit_weight(const Decomposed_Board_Index& d) const;

	// Builds the per-within table needed by orbit_weight in DIHEDRAL_8.
	// No-op for FILE_MIRROR. Idempotent. Not thread-safe — call from a single
	// thread before any concurrent orbit_weight calls.
	void prepare_orbit_weight_table();

private:

	Symmetry_Group m_symmetry = Symmetry_Group::FILE_MIRROR;
	const King_Slice_Manager* m_king_slice_manager = nullptr;
	std::unique_ptr<Pawn_Slice_Manager> m_pawn_slice_manager;
	bool m_initialized_ok = false;
	bool m_both_sides_have_free_attackers = false;

	// num_positions = num_pawn_slices * pawn_slice_stride
	// pawn_slice_stride = num_king_slices * within_slice_size
	size_t m_num_pawn_slices = 1;
	size_t m_num_king_slices = 0;
	size_t m_pawn_slice_stride = 0;
	size_t m_within_slice_size = 0;
	size_t m_num_positions = 0;
	// Only initialized when the corresponding stride is > 1; Divider rejects <= 1.
	Divider<uint64_t> m_pawn_slice_stride_div{};
	Divider<uint64_t> m_within_slice_size_div{};

	size_t m_num_populated_classes = 0;
	Piece_Class m_populated_classes[PIECE_CLASS_NB]{};

	std::unique_ptr<Piece_Group> m_groups_owned[PIECE_CLASS_NB];
	const Piece_Group* m_groups[PIECE_CLASS_NB]{};
	size_t m_num_positions_by_group[PIECE_CLASS_NB]{};
	size_t m_weights[PIECE_CLASS_NB]{};
	int8_t m_piece_counts_cached[PIECE_NB]{};
	// Only initialized for classes with weight > 1 (all but the first populated).
	Divider<uint64_t> m_weights_div[PIECE_CLASS_NB]{};

	// Per-within orbit weight (0/4/8) for DIHEDRAL_8 stabilized slices; empty
	// for FILE_MIRROR or pure DIHEDRAL_8 non-stab slices (which use a constant).
	std::vector<uint8_t> m_orbit_weight_within{};
};
