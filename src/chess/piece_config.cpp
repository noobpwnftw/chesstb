#include "piece_config.h"

#include "chess.h"

#include "util/defines.h"

#include <algorithm>
#include <map>

void Piece_Config::sort_pieces(Span<Piece> pieces)
{
	size_t score[COLOR_NB] = { 0, 0 };
	for (const Piece p : pieces)
		score[piece_color(p)] += PIECE_STRENGTH_FOR_SIDE_ORDER[p];

	if (score[BLACK] > score[WHITE])
		for (Piece& p : pieces)
			p = piece_opp_color(p);

	std::sort(
		pieces.begin(),
		pieces.end(),
		[](Piece a, Piece b) {
			return PIECE_ORDER[a] < PIECE_ORDER[b];
		}
	);
}

void Piece_Config::add_sub_configs_to(Unique_Piece_Configs& pss) const
{
	for (size_t i = 0; i < num_pieces(); ++i)
	{
		if (!can_remove_piece(i))
			continue;

		pss.add_unique(with_removed_piece(i));
	}
}

void Piece_Config::add_closure_in_dependency_order_to(Unique_Piece_Configs& pss, bool assume_contains_closures) const
{
	if (assume_contains_closures && pss.contains(*this))
		return;

	// Capture sub-configs (one piece removed).
	for (size_t i = 0; i < num_pieces(); ++i)
	{
		if (!can_remove_piece(i))
			continue;

		const auto ps = with_removed_piece(i);
		ps.add_closure_in_dependency_order_to(pss, assume_contains_closures);
		pss.add_unique(ps);
	}

	// Promotion sub-configs (pawn replaced by Q/R/B/N). These are dependencies
	// because a pawn-bearing TB's promotion moves land in these tables.
	// Note: the result may have MORE strength than the parent (Q replaces P),
	// but it's still a forward-dependency for retrograde — must exist first.
	for (size_t i = 0; i < num_pieces(); ++i)
	{
		const Piece pc = m_pieces[i];
		if (piece_type(pc) != PAWN)
			continue;
		const Color c = piece_color(pc);
		for (Piece_Type pt : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			const auto ps = with_replaced_piece(i, piece_make(c, pt));
			ps.add_closure_in_dependency_order_to(pss, assume_contains_closures);
			pss.add_unique(ps);
		}
	}

	pss.add_unique(*this);
}

std::map<std::pair<Piece, Piece_Type>, Piece_Config> Piece_Config::promotion_sub_configs() const
{
	std::map<std::pair<Piece, Piece_Type>, Piece_Config> res;
	for (size_t i = 0; i < num_pieces(); ++i)
	{
		const Piece pc = m_pieces[i];
		if (piece_type(pc) != PAWN)
			continue;
		const Color c = piece_color(pc);
		for (Piece_Type pt : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			res.try_emplace(std::make_pair(pc, pt), with_replaced_piece(i, piece_make(c, pt)));
		}
	}
	return res;
}

Unique_Piece_Configs Piece_Config::sub_configs() const
{
	Unique_Piece_Configs sub;
	add_sub_configs_to(sub);
	return sub;
}

std::map<Piece, Piece_Config> Piece_Config::sub_configs_by_capture() const
{
	std::map<Piece, Piece_Config> res;

	for (size_t i = 0; i < num_pieces(); ++i)
	{
		if (!can_remove_piece(i))
			continue;

		res.try_emplace(m_pieces[i], with_removed_piece(i));
	}

	return res;
}

Unique_Piece_Configs Piece_Config::closure() const
{
	Unique_Piece_Configs sub;
	add_closure_in_dependency_order_to(sub);
	return sub;
}
