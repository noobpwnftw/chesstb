#include "egtb/piece_config_for_gen.h"
#include "egtb/slice_manager.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/defines.h"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

INLINE Square diag_flip(Square s)
{
	return sq_make(static_cast<Rank>(sq_file(s)), static_cast<File>(sq_rank(s)));
}

}  // namespace

namespace {

// One King_Slice_Manager per symmetry group, lazily built.
const King_Slice_Manager& slice_mgr_for(Symmetry_Group sym)
{
	static std::mutex mu;
	static std::map<Symmetry_Group, std::unique_ptr<King_Slice_Manager>> cache;
	std::lock_guard<std::mutex> lock(mu);
	auto it = cache.find(sym);
	if (it == cache.end())
	{
		auto p = std::make_unique<King_Slice_Manager>(sym);
		auto* raw = p.get();
		cache[sym] = std::move(p);
		return *raw;
	}
	return *it->second;
}

}  // namespace

const King_Slice_Manager& get_slice_manager(Symmetry_Group sym)
{
	return slice_mgr_for(sym);
}

bool Piece_Config_For_Gen::try_init()
{
	const auto counts = piece_counts();
	m_symmetry = pick_symmetry(*this);
	m_king_slice_manager = &get_slice_manager(m_symmetry);

	m_both_sides_have_free_attackers =
		   has_any_free_attackers(WHITE)
		&& has_any_free_attackers(BLACK);

	for (size_t i = 0; i < PIECE_CLASS_NB; ++i)
	{
		m_groups_owned[i].reset();
		m_groups[i] = nullptr;
	}

	auto make_group = [&](Piece_Class pclass, Piece pc, size_t count, Const_Span<Square> legals) {
		m_groups_owned[pclass] = std::make_unique<Piece_Group>(pc, count, legals);
		m_groups[pclass] = m_groups_owned[pclass].get();
	};

	for (Color c : { WHITE, BLACK })
	{
		for (Piece_Type pt : { QUEEN, ROOK, BISHOP, KNIGHT, PAWN })
		{
			const Piece pc = piece_make(c, pt);
			const size_t n = counts[pc];
			if (n == 0) continue;
			const Piece_Type_Class ptcl =
				pt == QUEEN  ? QUEENS  :
				pt == ROOK   ? ROOKS   :
				pt == BISHOP ? BISHOPS :
				pt == KNIGHT ? KNIGHTS :
				                PAWNS;
			const Piece_Class pcl = make_piece_class(c, ptcl);
			auto sq = default_legal_squares(pc);
			make_group(pcl, pc, n, Const_Span<Square>(sq));
		}
	}

	const bool has_pawns = counts[WHITE_PAWN] > 0 || counts[BLACK_PAWN] > 0;
	if (has_pawns)
	{
		m_pawn_slice_manager = std::make_unique<Pawn_Slice_Manager>(
			*this,
			m_groups[WHITE_PAWNS],
			m_groups[BLACK_PAWNS]);
	}
	else
	{
		m_pawn_slice_manager = std::make_unique<Pawn_Slice_Manager>(Pawn_Slice_Manager::empty());
	}
	m_num_pawn_slices = m_pawn_slice_manager->num_slices();

	m_num_populated_classes = 0;
	size_t w = 1;
	for (Piece_Class i = PIECE_CLASS_START; i < PIECE_CLASS_END; ++i)
	{
		if (m_groups[i] == nullptr) continue;
		if (i == WHITE_PAWNS || i == BLACK_PAWNS) continue;
		m_populated_classes[m_num_populated_classes++] = i;
		m_weights[i] = w;
		if (w > 1)
			m_weights_div[i] = Divider<uint64_t>(w);
		m_num_positions_by_group[i] = m_groups[i]->table_size();
		const size_t next_w = w * m_num_positions_by_group[i];
		if (m_num_positions_by_group[i] != 0 && next_w / m_num_positions_by_group[i] != w)
			return false;
		if (next_w > MAX_NUM_POSITIONS)
			return false;
		w = next_w;
	}
	m_within_slice_size = w;
	if (m_within_slice_size > 1)
		m_within_slice_size_div = Divider<uint64_t>(m_within_slice_size);

	m_num_king_slices = m_king_slice_manager->num_slices;
	m_pawn_slice_stride = m_num_king_slices * m_within_slice_size;
	if (m_within_slice_size != 0 && m_pawn_slice_stride / m_within_slice_size != m_num_king_slices)
		return false;
	if (m_pawn_slice_stride > 1)
		m_pawn_slice_stride_div = Divider<uint64_t>(m_pawn_slice_stride);

	const size_t total = m_num_pawn_slices * m_pawn_slice_stride;
	if (m_pawn_slice_stride != 0 && total / m_pawn_slice_stride != m_num_pawn_slices)
		return false;
	if (total > MAX_NUM_POSITIONS)
		return false;
	m_num_positions = total;
	return true;
}

void Piece_Config_For_Gen::prepare_orbit_weight_table()
{
	// Per-within orbit weight: only needed for DIHEDRAL_8 stabilized slices.
	// Encoding per cell: 0 = phantom (non-canonical half of diag pair, never
	// written), 4 = diag-fixed (orbit size 4), 8 = canonical, not fixed.
	if (m_symmetry != Symmetry_Group::DIHEDRAL_8) return;
	if (m_within_slice_size == 0) return;
	if (!m_orbit_weight_within.empty()) return;

	// Per-class diag-image lookup, freed after the within table is built.
	std::vector<std::vector<Piece_Group::Placement_Index>> diag_image(PIECE_CLASS_NB);
	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Class c = m_populated_classes[i];
		const Piece_Group& g = group(c);
		const size_t n = g.table_size();
		diag_image[c].resize(n);
		for (size_t idx = 0; idx < n; ++idx)
		{
			const auto& p = g.squares(static_cast<Piece_Group::Placement_Index>(idx));
			const auto flipped = p.with_transformed_squares(diag_flip);
			diag_image[c][idx] = g.compound_index(flipped);
		}
	}

	m_orbit_weight_within.assign(m_within_slice_size, 0);
	Decomposed_Board_Index d{};
	for (size_t w = 0; w < m_within_slice_size; ++w)
	{
		size_t w_alt = 0;
		for (size_t i = 0; i < m_num_populated_classes; ++i)
		{
			const Piece_Class c = m_populated_classes[i];
			w_alt += m_weights[c] * static_cast<size_t>(diag_image[c][d.within[c]]);
		}
		m_orbit_weight_within[w] =
			(w_alt > w) ? uint8_t{8} :
			(w_alt == w) ? uint8_t{4} : uint8_t{0};

		// Advance d.within in board-index order without touching king_slice.
		for (size_t i = 0; i < m_num_populated_classes; ++i)
		{
			const Piece_Class ix = m_populated_classes[i];
			d.within[ix] = static_cast<Piece_Group::Placement_Index>(d.within[ix] + 1);
			if (d.within[ix] < m_num_positions_by_group[ix]) break;
			d.within[ix] = Piece_Group::Placement_Index(0);
		}
	}
}

uint8_t Piece_Config_For_Gen::orbit_weight(const Decomposed_Board_Index& d) const
{
	const auto& sm = *m_king_slice_manager;
	ASSERT(d.king_slice_id >= 0
	       && static_cast<size_t>(d.king_slice_id) < sm.kings_of_slice.size());
	const auto [wk, bk] = sm.kings_of_slice[d.king_slice_id];

	// Storage-overlap pruning: any cell where a non-king piece sits on a king
	// square, or two non-king pieces share a square, is stored as ILLEGAL but
	// is not a real chess position. Treat as weight 0 so counters stay clean.
	Bitboard occupied = square_bb(wk) | square_bb(bk);

	if (m_pawn_slice_manager->has_pawns())
	{
		const auto pd = m_pawn_slice_manager->decompose(d.pawn_slice_id);
		for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
		{
			if (!is_populated(c)) continue;
			const auto idx = (c == WHITE_PAWNS) ? pd.white_idx : pd.black_idx;
			const auto& p = group(c).squares(idx);
			for (size_t k = 0; k < p.size(); ++k)
			{
				const Bitboard b = square_bb(p[k]);
				if (occupied & b) return 0;
				occupied |= b;
			}
		}
	}

	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Class c = m_populated_classes[i];
		const auto& p = group(c).squares(d.within[c]);
		for (size_t k = 0; k < p.size(); ++k)
		{
			const Bitboard b = square_bb(p[k]);
			if (occupied & b) return 0;
			occupied |= b;
		}
	}

	// Structural weight: D2 trivial, D8 needs phantom/diag-fixed lookup.
	if (m_symmetry == Symmetry_Group::FILE_MIRROR) return 2;

	ASSERT(m_symmetry == Symmetry_Group::DIHEDRAL_8);
	ASSERT(static_cast<size_t>(d.king_slice_id) < sm.slice_has_stabilizer.size());
	if (!sm.slice_has_stabilizer[d.king_slice_id]) return 8;

	size_t w = 0;
	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Class c = m_populated_classes[i];
		w += m_weights[c] * static_cast<size_t>(d.within[c]);
	}
	return m_orbit_weight_within[w];
}

std::optional<size_t> Piece_Config_For_Gen::num_positions_safe(const Piece_Config& ps)
{
	Piece_Config_For_Gen epsi(ps, std::nothrow);
	if (epsi.m_initialized_ok)
		return epsi.m_num_positions;
	return std::nullopt;
}

Piece_Config_For_Gen::Piece_Config_For_Gen(const Piece_Config& ps) :
	Piece_Config(ps)
{
	if (!try_init())
		throw std::runtime_error("Piece_Config_For_Gen: configuration too large to materialize");
	m_initialized_ok = true;
}

Piece_Config_For_Gen::Piece_Config_For_Gen(const Piece_Config& ps, std::nothrow_t) :
	Piece_Config(ps)
{
	m_initialized_ok = try_init();
}

template <bool ASSUME_LEGAL>
bool Piece_Config_For_Gen::fill_board(
	const Decomposed_Board_Index& index, Out_Param<Position> board) const
{
	board->clear();

	const King_Slice_Manager& sm = slice_manager();
	if constexpr (!ASSUME_LEGAL)
	{
		if (index.king_slice_id < 0
		    || static_cast<size_t>(index.king_slice_id) >= sm.num_slices)
			return false;
	}
	const auto [wk, bk] = sm.kings_of_slice[index.king_slice_id];
	board->put_piece(WHITE_KING, wk);
	board->put_piece(BLACK_KING, bk);

	const Pawn_Slice_Manager& psm = pawn_slice_manager();
	if (psm.has_pawns())
	{
		const auto pd = psm.decompose(index.pawn_slice_id);
		auto place_pawns = [&](Piece_Class c, Piece_Group::Placement_Index idx) -> bool {
			if (!is_populated(c)) return true;
			const Piece_Group& g = group(c);
			const auto& placement = g.squares(idx);
			const Piece pc = g.piece();
			for (size_t k = 0; k < placement.size(); ++k)
			{
				const Square sq = placement[k];
				if constexpr (!ASSUME_LEGAL)
					if (!board->is_empty(sq)) return false;
				board->put_piece(pc, sq);
			}
			return true;
		};
		if (!place_pawns(WHITE_PAWNS, pd.white_idx)) return false;
		if (!place_pawns(BLACK_PAWNS, pd.black_idx)) return false;
	}

	for (size_t i = 0; i < m_num_populated_classes; ++i)
	{
		const Piece_Class c = m_populated_classes[i];
		const Piece_Group& g = group(c);
		const auto& placement = g.squares(index.within[c]);
		const Piece pc = g.piece();
		for (size_t k = 0; k < placement.size(); ++k)
		{
			const Square sq = placement[k];
			if constexpr (!ASSUME_LEGAL)
				if (!board->is_empty(sq)) return false;
			board->put_piece(pc, sq);
		}
	}

	return true;
}

template bool Piece_Config_For_Gen::fill_board<false>(
	const Decomposed_Board_Index&, Out_Param<Position>) const;
template bool Piece_Config_For_Gen::fill_board<true>(
	const Decomposed_Board_Index&, Out_Param<Position>) const;
