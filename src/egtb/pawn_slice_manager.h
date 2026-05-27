#pragma once

#include "chess/chess.h"

#include "egtb/piece_group.h"

#include "util/defines.h"
#include "util/span.h"

#include <cstdint>
#include <vector>

struct Pawn_Slice_Manager
{
    Pawn_Slice_Manager();
    Pawn_Slice_Manager(const Piece_Group* white_pawns,
                       const Piece_Group* black_pawns);

    NODISCARD size_t num_slices() const { return m_num_slices; }
    NODISCARD bool   has_pawns()  const { return m_has_pawns; }

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

    NODISCARD int32_t mirror_slice_of(int32_t slice_id) const;

    NODISCARD const std::vector<std::vector<int32_t>>& pair_topo_batches() const
    {
        return m_pair_topo_batches;
    }

    NODISCARD std::vector<int32_t> pair_members(int32_t slice_id) const
    {
        const int32_t mir = mirror_slice_of(slice_id);
        if (mir == slice_id) return { slice_id };
        return { slice_id, mir };
    }

    NODISCARD std::vector<int32_t> push_target_slices(int32_t slice_id) const;

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

    std::vector<std::vector<int32_t>> m_pair_topo_batches;
};
