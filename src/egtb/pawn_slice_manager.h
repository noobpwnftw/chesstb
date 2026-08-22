#pragma once

#include "chess/chess.h"

#include "egtb/pair_group.h"
#include "egtb/piece_group.h"

#include "util/defines.h"
#include "util/division.h"
#include "util/intrin.h"
#include "util/lazy.h"
#include "util/span.h"

#include <cstdint>
#include <vector>

// Cap on pawns across both colors, counting the opposing pair's two members.
// Slice ids are int32_t, and so are the cartesian cell ids they are ranked from
// (m_cartesian_by_storage), so the whole cartesian pawn space must fit in int32.
// That space is
//     PLACEMENT_COUNT^(has pair) * C(48, free white) * C(48, free black)
// and is maximised by the most balanced free split, so bounding the total pawn
// count is what keeps the product in range:
//
//   6 pawns, worst split 3W+3B:  C(48,3)^2           =   299,151,616   ok
//   7 pawns, worst split 4W+3B:  C(48,4)*C(48,3)     = 3,365,455,680   overflows
//   7 with a pair, worst 3W+2B:  120*C(48,3)*C(48,2) = 2,341,186,560   overflows
//
// Memory says the same thing: m_cartesian_by_storage is 4 bytes per surviving
// cell, so the first rejected shapes already want 9+ GiB just to enumerate.
// Note this is a tighter bound than the per-group MAX_PIECE_GROUP_SIZE -- that
// one caps a single color's free pawns, this one caps their product.
inline constexpr size_t MAX_TOTAL_PAWNS = 6;

// Worst-case cartesian slice count over every split of `total_pawns` between the
// colors. Splits that exceed the per-group cap are skipped: those are rejected
// by Piece_Group itself, not representable at any size.
NODISCARD constexpr uint64_t max_cartesian_pawn_slices(size_t total_pawns, bool with_pair)
{
    if (with_pair && total_pawns < 2) return 0;
    const size_t free_pawns  = total_pawns - (with_pair ? 2 : 0);
    const size_t pawn_squares = possible_sq_nb(WHITE_PAWN);
    const uint64_t pair_cells = with_pair ? Pair_Group::PLACEMENT_COUNT : 1;
    uint64_t worst = 0;
    for (size_t w = 0; w <= free_pawns; ++w)
    {
        const size_t b = free_pawns - w;
        if (w > Piece_Group::MAX_PIECE_GROUP_SIZE || b > Piece_Group::MAX_PIECE_GROUP_SIZE)
            continue;
        const uint64_t cells =
            pair_cells * binomial(pawn_squares, w) * binomial(pawn_squares, b);
        if (cells > worst) worst = cells;
    }
    return worst;
}

static_assert(max_cartesian_pawn_slices(MAX_TOTAL_PAWNS, false)
              <= static_cast<uint64_t>(INT32_MAX));
static_assert(max_cartesian_pawn_slices(MAX_TOTAL_PAWNS, true)
              <= static_cast<uint64_t>(INT32_MAX));

struct Pawn_Slice_Manager
{
    Pawn_Slice_Manager(const Pair_Group* pair,
                       const Piece_Group* white_pawns,
                       const Piece_Group* black_pawns);

    NODISCARD size_t num_slices() const { return m_num_slices; }
    NODISCARD bool   has_pawns()  const { return m_has_pawns; }
    NODISCARD bool   has_pair()   const { return m_pair_group != nullptr; }

    struct Decomposed
    {
        Pair_Group::Index            pair_idx;   // 0 when there is no pair
        Piece_Group::Placement_Index white_idx;  // free white pawns
        Piece_Group::Placement_Index black_idx;  // free black pawns
    };
    NODISCARD Decomposed decompose(int32_t slice_id) const;
    NODISCARD int32_t compose(Pair_Group::Index pair,
                              Piece_Group::Placement_Index w,
                              Piece_Group::Placement_Index b) const;

    // Reverse lookup from a board. `pair_white_sq` / `pair_black_sq` give the
    // pair members (SQ_END for both when there is no pair); the free-pawn spans
    // must EXCLUDE those two squares.
    NODISCARD int32_t lookup_from_squares(
        Square pair_white_sq, Square pair_black_sq,
        Const_Span<Square> white_pawn_squares,
        Const_Span<Square> black_pawn_squares) const;

    NODISCARD int32_t slice_after_pawn_push(
        int32_t slice_id, Color mover, Square from, Square to) const;

    NODISCARD int32_t mirror_slice_of(int32_t slice_id) const;

    // Total remaining pawn pushes in the slice.
    NODISCARD int slice_life(int32_t slice_id) const;

    NODISCARD const std::vector<std::vector<int32_t>>& pair_topo_batches() const
    {
        return *m_pair_topo_batches;
    }

    NODISCARD std::vector<int32_t> pair_members(int32_t slice_id) const
    {
        const int32_t mir = mirror_slice_of(slice_id);
        if (mir == slice_id) return { slice_id };
        return { slice_id, mir };
    }

    NODISCARD std::vector<int32_t> push_target_slices(int32_t slice_id) const;

    Pawn_Slice_Manager(const Pawn_Slice_Manager&) = delete;
    Pawn_Slice_Manager& operator=(const Pawn_Slice_Manager&) = delete;

private:
    struct Topo_Batch_Builder
    {
        const Pawn_Slice_Manager* psm;
        NODISCARD std::vector<std::vector<int32_t>> operator()() const;
    };

    NODISCARD std::vector<std::vector<int32_t>> build_pair_topo_batches() const;

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

    Lazy_Cached_Value<Topo_Batch_Builder> m_pair_topo_batches;
};
