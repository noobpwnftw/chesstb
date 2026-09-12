#include "piece_config.h"

#include "chess.h"

#include "util/defines.h"

#include <algorithm>

bool Piece_Config::sort_pieces(Span<Piece> pieces,
                              const std::array<size_t, COLOR_NB>& castling_rights)
{
	size_t score[COLOR_NB] = { 0, 0 };
	for (const Piece p : pieces)
		score[piece_color(p)] += PIECE_STRENGTH_FOR_SIDE_ORDER[p];
	for (const Color c : { WHITE, BLACK })
		score[c] += castling_rights[c] * PIECE_STRENGTH_FOR_SIDE_ORDER[piece_make(c, ROOK)];

	bool do_swap = score[BLACK] > score[WHITE];
	if (score[BLACK] == score[WHITE])
	{
		std::array<int8_t, PIECE_TYPE_NB> white_key{}, black_key{};
		for (const Piece p : pieces)
			(piece_color(p) == WHITE ? white_key : black_key)[piece_type(p)]++;
		white_key[ROOK] += static_cast<int8_t>(castling_rights[WHITE]);
		black_key[ROOK] += static_cast<int8_t>(castling_rights[BLACK]);
		if (white_key != black_key)
			do_swap = black_key > white_key;
		else
			// Same men on both sides, so the only thing left to order by is which
			// side's rook kept its right. Fewer white rights is the canonical
			// orientation, matching Material_Key's castle code -- KRKr, not KrKR.
			do_swap = castling_rights[WHITE] > castling_rights[BLACK];
	}

	if (do_swap)
		for (Piece& p : pieces)
			p = piece_opp_color(p);

	std::sort(
		pieces.begin(),
		pieces.end(),
		[](Piece a, Piece b) {
			return PIECE_ORDER[a] < PIECE_ORDER[b];
		}
	);

	return do_swap;
}
