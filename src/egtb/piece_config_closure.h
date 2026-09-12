#pragma once

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/defines.h"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

// Sub-configs and the dependency closure over them: each function derives a
// different material from an existing one (capture, promotion, dropped
// castling right, broken pair). Generation-only.
//
// Includes neither indexing layer; safe alongside probe/position_index.h.

// Insertion-ordered set of Piece_Configs (uniqueness by material key).
struct Unique_Piece_Configs
{
	using Container_Type = std::vector<Piece_Config>;
	using iterator = typename Container_Type::iterator;
	using const_iterator = typename Container_Type::const_iterator;
	using reverse_iterator = typename Container_Type::reverse_iterator;
	using const_reverse_iterator = typename Container_Type::const_reverse_iterator;

	NODISCARD const Piece_Config& operator[](size_t idx) const { return m_piece_sets[idx]; }

	void clear()
	{
		m_mat_keys.clear();
		m_piece_sets.clear();
	}

	NODISCARD bool contains(const Piece_Config& ps) const
	{
		return m_mat_keys.find(ps.base_material_key()) != m_mat_keys.end();
	}

	void add_unique(Piece_Config ps)
	{
		if (!contains(ps))
		{
			m_mat_keys.insert(ps.base_material_key());
			m_piece_sets.emplace_back(std::move(ps));
		}
	}

	void add_unique(const Unique_Piece_Configs& pss)
	{
		for (const auto& ps : pss)
			add_unique(ps);
	}

	// Transitive closure, ordered so each config lands after its dependencies.
	// `assume_contains_closures`: treat a present config as closed, skipping its
	// subtree; valid only if every entry came through this call.
	void add_closure_in_dependency_order(const Piece_Config& ps,
	                                     bool assume_contains_closures = false);

	void remove(const Piece_Config& ps)
	{
		auto iter = std::find(m_piece_sets.begin(), m_piece_sets.end(), ps);
		if (iter != m_piece_sets.end())
		{
			m_mat_keys.erase(ps.base_material_key());
			m_piece_sets.erase(iter);
		}
	}

	template <typename FuncT>
	void remove_if(FuncT&& f)
	{
		auto new_end = std::remove_if(m_piece_sets.begin(), m_piece_sets.end(), std::forward<FuncT>(f));
		for (auto it = new_end; it != m_piece_sets.end(); ++it)
			m_mat_keys.erase(it->base_material_key());
		m_piece_sets.erase(new_end, m_piece_sets.end());
	}

	NODISCARD size_t size() const  { return m_piece_sets.size(); }
	NODISCARD bool   empty() const { return m_piece_sets.empty(); }

	NODISCARD const_iterator begin()  const { return m_piece_sets.begin(); }
	NODISCARD const_iterator cbegin() const { return m_piece_sets.cbegin(); }
	NODISCARD const_iterator end()    const { return m_piece_sets.end(); }
	NODISCARD const_iterator cend()   const { return m_piece_sets.cend(); }

	NODISCARD const_reverse_iterator rbegin()  const { return m_piece_sets.rbegin(); }
	NODISCARD const_reverse_iterator crbegin() const { return m_piece_sets.crbegin(); }
	NODISCARD const_reverse_iterator rend()    const { return m_piece_sets.rend(); }
	NODISCARD const_reverse_iterator crend()   const { return m_piece_sets.crend(); }

private:
	Container_Type m_piece_sets;
	std::set<Material_Key> m_mat_keys;
};

NODISCARD INLINE bool can_remove_piece(const Piece_Config& ps, size_t idx)
{
	return idx < ps.num_pieces() && piece_type(ps.pieces()[idx]) != KING;
}

NODISCARD INLINE Piece_Config with_removed_piece(const Piece_Config& ps, size_t idx)
{
	ASSERT(can_remove_piece(ps, idx));

	const auto pcs = ps.pieces();
	Piece pcs_cpy[MAX_MAN];
	std::memcpy(pcs_cpy, pcs.data(), idx * sizeof(Piece));
	std::memcpy(pcs_cpy + idx, pcs.data() + idx + 1, (pcs.size() - idx - 1) * sizeof(Piece));
	Piece_Config res(Span(pcs_cpy, pcs.size() - 1), ps.castling_rights());
	if (ps.has_opposing_pair())
		res.mark_opposing_pair();
	return res;
}

NODISCARD INLINE Piece_Config with_replaced_piece(const Piece_Config& ps, size_t idx, Piece replacement)
{
	ASSERT(can_remove_piece(ps, idx));

	const auto pcs = ps.pieces();
	Piece pcs_cpy[MAX_MAN];
	std::memcpy(pcs_cpy, pcs.data(), pcs.size() * sizeof(Piece));
	pcs_cpy[idx] = replacement;
	Piece_Config res(Span(pcs_cpy, pcs.size()), ps.castling_rights());
	if (ps.has_opposing_pair())
		res.mark_opposing_pair();
	return res;
}

// Rights-dropping sub-config: a king move or castle gives up that side's
// rights at once ("rr -> RR"), a rook move only its own ("rr -> Rr"). Every
// man stays on the board, so the twin is the same size.
NODISCARD INLINE Piece_Config rights_dropped(const Piece_Config& ps, Color c, size_t num_dropped)
{
	ASSERT(num_dropped > 0 && num_dropped <= ps.castling_rights(c));

	const auto pcs = ps.pieces();
	Piece pcs_cpy[MAX_MAN];
	std::memcpy(pcs_cpy, pcs.data(), pcs.size() * sizeof(Piece));
	size_t n = pcs.size();
	for (size_t i = 0; i < num_dropped; ++i)
		pcs_cpy[n++] = piece_make(c, ROOK);

	Piece_Config::Castling_Rights_Counts left = ps.castling_rights();
	left[c] -= num_dropped;
	Piece_Config res(Span(pcs_cpy, n), left);
	if (ps.has_opposing_pair())
		res.mark_opposing_pair();
	return res;
}

// Castling-rook-capture sub-config "r -> (nothing)": a rook holding a right
// is taken. It leaves as a man, so the child is one lighter and holds one
// right fewer.
NODISCARD INLINE Piece_Config castling_rook_captured(const Piece_Config& ps, Color c)
{
	ASSERT(ps.castling_rights(c) > 0);

	Piece_Config::Castling_Rights_Counts left = ps.castling_rights();
	left[c] -= 1;
	Piece_Config res(ps.pieces(), left);
	if (ps.has_opposing_pair())
		res.mark_opposing_pair();
	return res;
}

// Pair-breaking sub-config "p -> PP": an opposing-pair member captures the free
// piece at `capture_idx` (an ordinary diagonal capture, or an en-passant
// capture of a free pawn). Both former pair pawns survive as free pawns and
// the captured piece is gone; the result no longer carries the pair.
NODISCARD INLINE Piece_Config pair_broken_by_capture(const Piece_Config& ps, size_t capture_idx)
{
	ASSERT(ps.has_opposing_pair());
	ASSERT(can_remove_piece(ps, capture_idx));

	const auto pcs = ps.pieces();
	Piece pcs_cpy[MAX_MAN];
	size_t n = 0;
	for (size_t i = 0; i < pcs.size(); ++i)
		if (i != capture_idx)
			pcs_cpy[n++] = pcs[i];
	pcs_cpy[n++] = WHITE_PAWN;
	pcs_cpy[n++] = BLACK_PAWN;
	return Piece_Config(Span(pcs_cpy, n), ps.castling_rights());
}

// Pair-breaking sub-config "p -> P": one opposing-pair member is captured by an
// enemy piece. The surviving member (of color `survivor`) becomes a free
// pawn; the result no longer carries the pair.
NODISCARD INLINE Piece_Config pair_broken_survivor(const Piece_Config& ps, Color survivor)
{
	ASSERT(ps.has_opposing_pair());

	const auto pcs = ps.pieces();
	Piece pcs_cpy[MAX_MAN];
	std::memcpy(pcs_cpy, pcs.data(), pcs.size() * sizeof(Piece));
	pcs_cpy[pcs.size()] = piece_make(survivor, PAWN);
	return Piece_Config(Span(pcs_cpy, pcs.size() + 1), ps.castling_rights());
}

NODISCARD INLINE std::map<Piece, Piece_Config> sub_configs_by_capture(const Piece_Config& ps)
{
	std::map<Piece, Piece_Config> res;

	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		if (!can_remove_piece(ps, i))
			continue;

		res.try_emplace(ps.pieces()[i], ps.has_opposing_pair() ? pair_broken_by_capture(ps, i)
		                                                       : with_removed_piece(ps, i));
	}

	return res;
}

INLINE void Unique_Piece_Configs::add_closure_in_dependency_order(const Piece_Config& ps,
                                                          bool assume_contains_closures)
{
	if (assume_contains_closures && contains(ps))
		return;

	// Capture sub-configs (one piece removed).
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		if (!can_remove_piece(ps, i))
			continue;

		const auto sub = ps.has_opposing_pair() ? pair_broken_by_capture(ps, i)
		                                       : with_removed_piece(ps, i);
		add_closure_in_dependency_order(sub, assume_contains_closures);
		add_unique(sub);
	}

	// p -> P: pair member captured; the survivor becomes a free pawn.
	if (ps.has_opposing_pair())
	{
		for (const Color survivor : { WHITE, BLACK })
		{
			const auto broken = pair_broken_survivor(ps, survivor);
			add_closure_in_dependency_order(broken, assume_contains_closures);
			add_unique(broken);
		}
	}

	// Dropping rights keeps every man, so the twin is the same size as this
	// table. Still a strict dependency: rights only ever decrease.
	if (ps.has_castling())
	{
		std::vector<Piece_Config> subs;
		for (const Color c : { WHITE, BLACK })
		{
			for (size_t dropped = 1; dropped <= ps.castling_rights(c); ++dropped)
				subs.push_back(rights_dropped(ps, c, dropped));
			if (ps.castling_rights(c) > 0)
				subs.push_back(castling_rook_captured(ps, c));
		}
		for (const auto& sub : subs)
		{
			add_closure_in_dependency_order(sub, assume_contains_closures);
			add_unique(sub);
		}
	}

	// Promotion sub-configs (pawn replaced by Q/R/B/N). These are dependencies
	// because a pawn-bearing TB's promotion moves land in these tables.
	// Note: the result may have MORE strength than the parent (Q replaces P),
	// but it's still a forward-dependency for retrograde — must exist first.
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pc = ps.pieces()[i];
		if (piece_type(pc) != PAWN)
			continue;
		const Color c = piece_color(pc);
		for (Piece_Type pt : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			const auto sub = with_replaced_piece(ps, i, piece_make(c, pt));
			add_closure_in_dependency_order(sub, assume_contains_closures);
			add_unique(sub);
		}
	}

	add_unique(ps);
}
