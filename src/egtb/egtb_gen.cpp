#include "chess/chess.h"
#include "chess/position.h"

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"
#include "egtb/king_slice_manager.h"
#include "egtb/pawn_slice_manager.h"
#include "egtb/index_permutation_plan.h"

#include "util/defines.h"
#include "util/math.h"

#include "lz4/lz4.h"

#include <atomic>
#include <climits>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<Interrupt_Request> g_interrupt_request{ Interrupt_Request::NONE };
static_assert(std::atomic<Interrupt_Request>::is_always_lock_free,
              "the interrupt state is written from a signal handler");
}  // namespace

void egtb_request_interrupt(Interrupt_Request req) noexcept
{
	g_interrupt_request.store(req, std::memory_order_relaxed);
}

bool egtb_is_interrupt_requested(bool hard) noexcept
{
	const Interrupt_Request req = g_interrupt_request.load(std::memory_order_relaxed);
	if (hard) return req == Interrupt_Request::HARD;
	return req != Interrupt_Request::NONE;
}

std::array<Piece_Group::Placement, PIECE_CLASS_NB>
placements_from_position(const Piece_Config_For_Gen& epsi, const Position& pos)
{
	std::array<Piece_Group::Placement, PIECE_CLASS_NB> out{};

	out[WHITE_KINGS].add(pos.king_square(WHITE));
	out[BLACK_KINGS].add(pos.king_square(BLACK));

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		const Piece pc = epsi.group(c).piece();
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}

	const bool has_pair = epsi.pair_group() != nullptr;
	for (Piece_Class c : { WHITE_PAWNS, BLACK_PAWNS })
	{
		if (!epsi.is_populated(c) && !has_pair) continue;
		const Piece pc = (c == WHITE_PAWNS) ? WHITE_PAWN : BLACK_PAWN;
		Bitboard b = pos.piece_bb(pc);
		while (b)
			out[c].add(b.pop_first_square());
	}
	return out;
}

Board_Index canonical_board_index(
	const Piece_Config_For_Gen& epsi,
	std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements,
	Const_Span<Square> white_castling_rooks,
	Const_Span<Square> black_castling_rooks)
{
	const bool has_castling = epsi.has_castling();
	if (!has_castling)
		canonicalize_placements(inout_param(placements), epsi);
	const Square wk = placements[WHITE_KINGS][0];
	const Square bk = placements[BLACK_KINGS][0];
	const int32_t king_slice_id =
		has_castling
			? epsi.king_slice_manager().castling_slice_of(
				wk, bk, white_castling_rooks, black_castling_rooks)
			: epsi.king_slice_manager().lookup(wk, bk).slice_id;
	if (king_slice_id == SLICE_NONE)
		return BOARD_INDEX_NONE;

	Decomposed_Board_Index dix;
	dix.king_slice_id = king_slice_id;

	if (epsi.pawn_slice_manager().has_pawns())
	{
		const auto& w_pl = placements[WHITE_PAWNS];
		const auto& b_pl = placements[BLACK_PAWNS];

		Square pair_w = SQ_END, pair_b = SQ_END;
		Square free_w[Piece_Group::MAX_PIECE_GROUP_SIZE];
		Square free_b[Piece_Group::MAX_PIECE_GROUP_SIZE];
		size_t nfw = 0, nfb = 0;

		if (epsi.pair_group())
		{
			Pair_Group::canonical_pair(
				Const_Span<Square>(w_pl.begin(), w_pl.size()),
				Const_Span<Square>(b_pl.begin(), b_pl.size()), pair_w, pair_b);
			for (size_t i = 0; i < w_pl.size(); ++i)
				if (w_pl[i] != pair_w) free_w[nfw++] = w_pl[i];
			for (size_t i = 0; i < b_pl.size(); ++i)
				if (b_pl[i] != pair_b) free_b[nfb++] = b_pl[i];
		}
		else
		{
			for (size_t i = 0; i < w_pl.size(); ++i) free_w[nfw++] = w_pl[i];
			for (size_t i = 0; i < b_pl.size(); ++i) free_b[nfb++] = b_pl[i];
		}

		dix.pawn_slice_id = epsi.pawn_slice_manager().lookup_from_squares(
			pair_w, pair_b,
			Const_Span<Square>(free_w, nfw),
			Const_Span<Square>(free_b, nfb));
	}
	else
	{
		dix.pawn_slice_id = 0;
	}

	for (size_t i = 0; i < epsi.num_populated_classes(); ++i)
	{
		const Piece_Class c = epsi.populated_classes()[i];
		dix.within[c] = epsi.group(c).compound_index(placements[c]);
	}
	return epsi.compose_board_index(dix);
}

Board_Index EGTB_Generator::next_quiet_index_fallback(
	const Position_For_Gen& pos_gen, Piece mover, Square from, Square to) const
{
	auto placements = pos_gen.placements_unchecked();
	placements[piece_class(mover)].move_square(from, to);
	const Const_Span<Square> none(nullptr, size_t(0));
	const int32_t king_slice_id = pos_gen.index().king_slice_id;
	return canonical_board_index(
		m_epsi, placements,
		m_epsi.has_castling() ? m_epsi.castling_rook_squares(king_slice_id, WHITE) : none,
		m_epsi.has_castling() ? m_epsi.castling_rook_squares(king_slice_id, BLACK) : none);
}

// Squares of one side's castling rooks, ascending by file.
size_t castling_rook_squares(const Position& pos, Color c,
                             Square out[Castling_Group::MAX_RIGHTS])
{
	size_t n = 0;
	for (const bool h_side : { false, true })
		if (pos.can_castle(c, h_side))
			out[n++] = pos.castling_rook_square(c, h_side);
	if (n == 2 && out[1] < out[0])
		std::swap(out[0], out[1]);
	return n;
}

Board_Index board_index_of_position(const Piece_Config_For_Gen& epsi, const Position& pos)
{
	auto p = placements_from_position(epsi, pos);
	Square rooks[COLOR_NB][Castling_Group::MAX_RIGHTS];
	size_t num_rooks[COLOR_NB] = { 0, 0 };
	if (epsi.has_castling())
	{
		for (const Color c : { WHITE, BLACK })
		{
			num_rooks[c] = castling_rook_squares(pos, c, rooks[c]);
			if (num_rooks[c] != epsi.castling_rights(c))
				return BOARD_INDEX_NONE;
			if (num_rooks[c] == 0) continue;
			const Piece_Class rook_class = make_piece_class(c, ROOKS);
			if (epsi.is_populated(rook_class))
				for (size_t i = 0; i < num_rooks[c]; ++i)
					p[rook_class].remove_square(rooks[c][i]);
			else
				p[rook_class].clear();
		}
	}
	return canonical_board_index(epsi, p,
	                             Const_Span<Square>(rooks[WHITE], num_rooks[WHITE]),
	                             Const_Span<Square>(rooks[BLACK], num_rooks[BLACK]));
}

static Piece_Config make_cap_promo_sub(const Piece_Config& ps,
                                       size_t cap_idx, size_t pawn_idx,
                                       Piece promo_piece, bool break_pair)
{
	ASSERT(cap_idx != pawn_idx);
	std::vector<Piece> pieces;
	pieces.reserve(ps.num_pieces() + 1);
	for (size_t k = 0; k < ps.num_pieces(); ++k)
	{
		if (k == cap_idx) continue;
		if (k == pawn_idx) pieces.push_back(promo_piece);
		else               pieces.push_back(ps.pieces()[k]);
	}
	if (break_pair)
	{
		pieces.push_back(WHITE_PAWN);
		pieces.push_back(BLACK_PAWN);
	}
	return Piece_Config(Const_Span<Piece>(pieces.data(), pieces.size()));
}

static Material_Key material_key_of_pieces(Const_Span<Piece> pieces)
{
	Material_Key k;
	for (Piece p : pieces) k.add_piece(p);
	return k;
}

EGTB_Generator::EGTB_Generator(const Piece_Config& ps) :
	m_epsi(ps),
	m_is_symmetric(false)
{
	const auto [mat_key, mir_key] = m_epsi.material_keys();
	m_is_symmetric = (mat_key == mir_key);

	auto resolve_sub = [&](Color mover, Const_Span<Piece> literal_post_pieces,
	                       const Piece_Config& sub_ps,
	                       const Piece_Config::Castling_Rights_Counts& literal_rights =
	                           Piece_Config::NO_CASTLE_RIGHTS) -> Sub_Entry {
		auto [it, _] = m_sub_epsi_store.try_emplace(sub_ps.base_material_key(), sub_ps);
		const Piece_Config_For_Gen* sub_epsi = &it->second;
		Material_Key literal_key = material_key_of_pieces(literal_post_pieces);
		if (sub_ps.has_opposing_pair())
			literal_key.add_pair();
		if (literal_rights[WHITE] + literal_rights[BLACK] > 0)
			literal_key.add_castling(literal_rights[WHITE], literal_rights[BLACK]);
		bool mirr = (literal_key != sub_ps.base_material_key());
		ASSERT(mirr ? (literal_key == sub_ps.material_keys().second)
		            : (literal_key == sub_ps.base_material_key()));
		Color read_color = color_maybe_opp(color_opp(mover), mirr);

		const auto [child_base, child_mirror] = sub_ps.material_keys();
		if (child_base == child_mirror && read_color == BLACK)
		{
			mirr = !mirr;
			read_color = WHITE;
		}

		return Sub_Entry{ sub_epsi, mirr, read_color };
	};

	auto register_move = [&](Color mover, Piece captured, Piece_Type promo,
	                         Const_Span<Piece> literal_post_pieces,
	                         const Piece_Config& sub_ps) {
		const Sub_Entry e =
			resolve_sub(mover, literal_post_pieces, sub_ps, ps.castling_rights());
		m_sub_epsi_by_move[mover][captured][promo] = e.epsi;
		m_sub_mirror_by_move[mover][captured][promo] = e.mirror;
		m_sub_read_color_by_move[mover][captured][promo] = e.read_color;
	};

	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		if (!can_remove_piece(ps, i)) continue;
		const Piece victim = ps.pieces()[i];
		std::vector<Piece> post;
		post.reserve(ps.num_pieces() + 1);
		for (size_t k = 0; k < ps.num_pieces(); ++k)
			if (k != i) post.push_back(ps.pieces()[k]);
		const Piece_Config sub = ps.has_opposing_pair()
			? pair_broken_by_capture(ps, i)
			: with_removed_piece(ps, i);
		if (ps.has_opposing_pair())
		{
			post.push_back(WHITE_PAWN);
			post.push_back(BLACK_PAWN);
		}
		register_move(color_opp(piece_color(victim)), victim, PIECE_TYPE_NONE,
		              Const_Span<Piece>(post.data(), post.size()),
		              sub);
	}

	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color mover = piece_color(pawn);
		for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			const Piece promoted = piece_make(mover, promo);
			std::vector<Piece> post;
			post.reserve(ps.num_pieces());
			for (size_t k = 0; k < ps.num_pieces(); ++k)
				post.push_back(k == i ? promoted : ps.pieces()[k]);
			register_move(mover, PIECE_NONE, promo,
			              Const_Span<Piece>(post.data(), post.size()),
			              with_replaced_piece(ps, i, promoted));
		}
	}

	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color mover = piece_color(pawn);
		for (size_t j = 0; j < ps.num_pieces(); ++j)
		{
			if (i == j) continue;
			const Piece victim = ps.pieces()[j];
			if (piece_color(victim) == mover) continue;
			if (piece_type(victim) == KING) continue;
			for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			{
				const Piece promoted = piece_make(mover, promo);
				std::vector<Piece> post;
				post.reserve(ps.num_pieces() + 1);
				for (size_t k = 0; k < ps.num_pieces(); ++k)
				{
					if (k == j) continue;
					post.push_back(k == i ? promoted : ps.pieces()[k]);
				}
				const bool break_pair = ps.has_opposing_pair();
				if (break_pair)
				{
					post.push_back(WHITE_PAWN);
					post.push_back(BLACK_PAWN);
				}
				register_move(mover, victim, promo,
				              Const_Span<Piece>(post.data(), post.size()),
				              make_cap_promo_sub(ps, /*cap_idx=*/ j, /*pawn_idx=*/ i,
				                                 promoted, break_pair));
			}
		}
	}

	if (ps.has_opposing_pair())
	{
		for (const Color pc : { WHITE, BLACK })
		{
			const Color mover = color_opp(pc);      // the capturing side
			const Color survivor = color_opp(pc);   // the other pair member
			std::vector<Piece> post(ps.pieces().begin(), ps.pieces().end());
			post.push_back(piece_make(survivor, PAWN));
			m_pair_broken_survivor[pc] =
				resolve_sub(mover, Const_Span<Piece>(post.data(), post.size()),
				            pair_broken_survivor(ps, survivor), ps.castling_rights());
		}
	}

	for (const Color cc : { WHITE, BLACK })
	{
		const size_t held = ps.castling_rights(cc);
		if (held == 0) continue;

		for (size_t dropped = 1; dropped <= held; ++dropped)
		{
			Piece_Config::Castling_Rights_Counts left = ps.castling_rights();
			left[cc] -= dropped;

			std::vector<Piece> post(ps.pieces().begin(), ps.pieces().end());
			for (size_t i = 0; i < dropped; ++i)
				post.push_back(piece_make(cc, ROOK));

			m_rights_dropped[cc][dropped] = resolve_sub(
				cc, Const_Span<Piece>(post.data(), post.size()),
				rights_dropped(ps, cc, dropped), left);
		}
	}

	if (ps.has_castling())
		register_castling_zeroing_moves(ps, resolve_sub);
}

std::vector<EGTB_Generator::Castling_Zeroing_Move>
EGTB_Generator::enumerate_castling_zeroing_moves(const Piece_Config& ps)
{
	std::vector<Castling_Zeroing_Move> out;
	if (!ps.has_castling()) return out;

	struct Victim { Piece piece; bool held_right; size_t free_idx; bool pair_member; };
	std::vector<Victim> victims;
	for (size_t i = 0; i < ps.num_pieces(); ++i)
		if (can_remove_piece(ps, i))
			victims.push_back({ ps.pieces()[i], false, i, false });
	for (const Color v : { WHITE, BLACK })
		if (ps.castling_rights(v) > 0)
			victims.push_back({ piece_make(v, ROOK), true, ps.num_pieces(), false });
	if (ps.has_opposing_pair())
		for (const Color v : { WHITE, BLACK })
			victims.push_back({ piece_make(v, PAWN), false, ps.num_pieces(), true });

	for (const Victim& victim : victims)
	{
		const Color mover = color_opp(piece_color(victim.piece));

		bool mover_has_pawn = false;
		for (const Piece p : ps.pieces())
			if (p == piece_make(mover, PAWN)) { mover_has_pawn = true; break; }

		for (size_t dropped = 0; dropped <= ps.castling_rights(mover); ++dropped)
		{
			Piece_Config::Castling_Rights_Counts left = ps.castling_rights();
			std::vector<Piece> men;
			men.reserve(ps.num_pieces() + Castling_Group::MAX_RIGHTS + 2);

			for (size_t k = 0; k < ps.num_pieces(); ++k)
				if (victim.held_right || victim.pair_member || k != victim.free_idx)
					men.push_back(ps.pieces()[k]);

			if (victim.held_right)
				left[piece_color(victim.piece)] -= 1;

			if (ps.has_opposing_pair())
			{
				if (victim.pair_member)
					men.push_back(piece_make(color_opp(piece_color(victim.piece)), PAWN));
				else
				{
					men.push_back(WHITE_PAWN);
					men.push_back(BLACK_PAWN);
				}
			}

			left[mover] -= dropped;
			for (size_t k = 0; k < dropped; ++k)
				men.push_back(piece_make(mover, ROOK));

			for (const Piece_Type promo : { PIECE_TYPE_NONE, QUEEN, ROOK, BISHOP, KNIGHT })
			{
				if (promo != PIECE_TYPE_NONE
				    && (dropped != 0 || !mover_has_pawn || victim.pair_member))
					continue;

				std::vector<Piece> post = men;
				if (promo != PIECE_TYPE_NONE)
					for (Piece& p : post)
						if (p == piece_make(mover, PAWN))
						{
							p = piece_make(mover, promo);
							break;
						}

				Piece_Config child(Const_Span<Piece>(post.data(), post.size()), left);
				out.push_back(Castling_Zeroing_Move{
					std::move(child), std::move(post), left, mover, dropped,
					victim.piece, victim.held_right, victim.pair_member, promo });
			}
		}
	}
	return out;
}

template <typename Resolve>
void EGTB_Generator::register_castling_zeroing_moves(const Piece_Config& ps, Resolve&& resolve_sub)
{
	for (const Castling_Zeroing_Move& m : enumerate_castling_zeroing_moves(ps))
	{
		const Sub_Entry e =
			resolve_sub(m.mover,
			            Const_Span<Piece>(m.literal_men.data(), m.literal_men.size()),
			            m.child, m.literal_rights);
		if (m.victim_is_pair_member)
			m_castling_pair_survivor[castling_drop_slot(m.mover, m.dropped)]
			                      [piece_color(m.victim)] = e;
		else
			m_castling_zeroing[castling_drop_slot(m.mover, m.dropped)]
			                [m.victim][m.victim_held_right ? 1 : 0][m.promo] = e;
	}
}

std::map<Material_Key, Piece_Config> EGTB_Generator::enumerate_sub_materials(const Piece_Config& ps)
{
	std::map<Material_Key, Piece_Config> out;
	auto add = [&](Piece_Config sub) {
		out.try_emplace(sub.min_material_key(), std::move(sub));
	};
	for (const auto& [_p, sub] : sub_configs_by_capture(ps)) add(sub);
	if (ps.has_opposing_pair())
	{
		add(pair_broken_survivor(ps, WHITE));
		add(pair_broken_survivor(ps, BLACK));
	}
	for (const Castling_Zeroing_Move& m : enumerate_castling_zeroing_moves(ps))
		add(m.child);
	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color pc = piece_color(pawn);
		for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			add(with_replaced_piece(ps, i, piece_make(pc, promo)));
		for (size_t j = 0; j < ps.num_pieces(); ++j)
		{
			if (i == j) continue;
			const Piece v = ps.pieces()[j];
			if (piece_color(v) == pc) continue;
			if (piece_type(v) == KING) continue;
			std::vector<Piece> pieces;
			pieces.reserve(ps.num_pieces() + 1);
			for (size_t k = 0; k < ps.num_pieces(); ++k)
			{
				if (k == j) continue;
				pieces.push_back(k == i ? piece_make(pc, QUEEN) : ps.pieces()[k]);
			}
			if (ps.has_opposing_pair())
			{
				pieces.push_back(WHITE_PAWN);
				pieces.push_back(BLACK_PAWN);
			}
			for (Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			{
				pieces[i < j ? i : i - 1] = piece_make(pc, promo);
				add(Piece_Config(Const_Span<Piece>(pieces.data(), pieces.size())));
			}
		}
	}
	return out;
}

std::map<Material_Key, Piece_Config>
EGTB_Generator::enumerate_exit_materials(const Piece_Config& ps)
{
	std::map<Material_Key, Piece_Config> out;
	for (const Color c : { WHITE, BLACK })
		for (size_t dropped = 1; dropped <= ps.castling_rights(c); ++dropped)
		{
			Piece_Config twin = rights_dropped(ps, c, dropped);
			out.try_emplace(twin.min_material_key(), std::move(twin));
		}
	return out;
}

Shared_Board_Index_Iterator EGTB_Generator::make_slice_group_iterator(
	size_t group_id, size_t slices_per_group) const
{
	const size_t wss = m_epsi.within_slice_size();
	const size_t ntotal = m_epsi.num_slices();
	const size_t g_start_slice = group_id * slices_per_group;
	const size_t g_end_slice   = std::min(g_start_slice + slices_per_group, ntotal);
	return Shared_Board_Index_Iterator(
		static_cast<Board_Index>(g_start_slice * wss),
		static_cast<Board_Index>(g_end_slice   * wss),
		CHUNK_SIZE);
}

namespace {

// A fixed-universe set with constant-time reset and insertion-order iteration.
struct Group_Set
{
	std::vector<uint32_t> mark;
	std::vector<size_t> ids;
	uint32_t tick = 0;

	explicit Group_Set(size_t n) : mark(n, 0) { reset(); }
	// Zero means "never marked", so the first generation is 1.
	void reset() { ++tick; ids.clear(); }
	void insert(size_t g)
	{
		if (mark[g] == tick) return;
		mark[g] = tick;
		ids.push_back(g);
	}
	NODISCARD size_t count() const { return ids.size(); }
	NODISCARD bool contains(size_t g) const { return mark[g] == tick; }
};

}  // namespace

Working_Set_Estimate compute_working_set(const Piece_Config& ps)
{
	Piece_Config_For_Gen epsi(ps);
	Working_Set_Estimate w{};

	w.num_positions     = epsi.num_positions();
	// Both metrics store one uint16_t per color and cell.
	w.total_table_bytes = static_cast<size_t>(2) * w.num_positions * sizeof(uint16_t);

	const size_t within = epsi.within_slice_size();
	w.bytes_per_slice = within * sizeof(uint16_t);

	constexpr size_t MIN_GROUP_BYTES = 64ull * 1024ull * 1024ull;
	if (w.bytes_per_slice == 0 || w.bytes_per_slice >= MIN_GROUP_BYTES)
		w.slices_per_group = 1;
	else
		w.slices_per_group = (MIN_GROUP_BYTES + w.bytes_per_slice - 1) / w.bytes_per_slice;

	w.bytes_per_group = w.slices_per_group * w.bytes_per_slice;
	w.num_slices      = within == 0 ? 0 : (w.num_positions / within);
	w.num_groups      = (w.num_slices == 0)
		? 0
		: (w.num_slices + w.slices_per_group - 1) / w.slices_per_group;

	if (w.num_groups == 0) return w;

	const size_t nks = epsi.num_king_slices();
	const size_t spg = w.slices_per_group;
	const auto& psm = epsi.pawn_slice_manager();

	// Retro charges both colors; init also adds the push-target closure.
	Group_Set pair_me(w.num_groups), pair_push(w.num_groups);
	Group_Set batch_me_groups(w.num_groups), batch_push_groups(w.num_groups);
	Group_Set touched(w.num_groups), candidate_set(w.num_groups);

	auto add_pid_range = [&](int32_t p, Group_Set& dst) {
		const size_t base = static_cast<size_t>(p) * nks;
		if (base >= w.num_slices) return;
		const size_t first_g = base / spg;
		const size_t last_g  = (std::min(base + nks, w.num_slices) - 1) / spg;
		for (size_t g = first_g; g <= last_g; ++g) dst.insert(g);
	};
	auto union_size = [](const Group_Set& a, const Group_Set& b) {
		size_t both = 0;
		for (size_t g : b.ids) both += a.contains(g) ? 1 : 0;
		return a.count() + b.count() - both;
	};
	std::vector<int32_t> targets;

	Slice_Reach_Scratch scratch;
	std::vector<uint8_t> reach_groups(w.num_groups, 0), push_groups(w.num_groups, 0);
	std::vector<std::vector<int32_t>> targets_by_active;

	for (const auto& batch : psm.pair_topo_batches())
	{
		batch_me_groups.reset();
		batch_push_groups.reset();

		for (int32_t pair_sid : batch)
		{
			const auto members = psm.pair_members(pair_sid);
			pair_me.reset();
			pair_push.reset();
			for (int32_t pid : members)
			{
				add_pid_range(pid, pair_me);
				add_pid_range(pid, batch_me_groups);
				psm.push_target_slices(pid, targets);
				for (int32_t tpid : targets)
				{
					add_pid_range(tpid, pair_push);
					add_pid_range(tpid, batch_push_groups);
				}
			}
			const size_t pair_iter = 2 * pair_me.count();
			const size_t pair_init = 2 * union_size(pair_me, pair_push);
			update_max(w.peak_pair_iter_groups, pair_iter);
			update_max(w.peak_pair_init_groups, pair_init);
		}

		const size_t batch_iter = 2 * batch_me_groups.count();
		const size_t batch_init = 2 * union_size(batch_me_groups, batch_push_groups);
		update_max(w.peak_batch_iter_groups, batch_iter);
		update_max(w.peak_batch_init_groups, batch_init);

		if (batch.empty()) continue;
		const auto members = psm.pair_members(batch.front());
		const std::vector<int32_t> sample_members(members.begin(), members.end());

		targets_by_active.resize(sample_members.size());
		for (size_t i = 0; i < sample_members.size(); ++i)
			psm.push_target_slices(sample_members[i], targets_by_active[i]);

		touched.reset();
		candidate_set.reset();
		for (size_t i = 0; i < sample_members.size(); ++i)
		{
			add_pid_range(sample_members[i], touched);
			add_pid_range(sample_members[i], candidate_set);
			for (int32_t tpid : targets_by_active[i])
				add_pid_range(tpid, candidate_set);
		}
		const std::vector<size_t>& candidates = candidate_set.ids;

		for (size_t g : touched.ids)
		{
			const size_t g_start = g * spg;
			const size_t g_end   = std::min(g_start + spg, w.num_slices);

			for (size_t c : candidates) reach_groups[c] = push_groups[c] = 0;
			mark_king_neighbor_reach(reach_groups.data(), epsi, sample_members,
			                         g_start, g_end, spg, scratch);
			mark_push_target_reach(push_groups.data(), epsi, sample_members, targets_by_active,
			                       g_start, g_end, spg);

			size_t iter_count = 0, push_union = 0;
			for (size_t c : candidates)
			{
				iter_count += reach_groups[c];
				if (push_groups[c] || c == g) ++push_union;
			}
			const size_t iter_g = 1 + iter_count;
			const size_t init_g = 2 * push_union;
			update_max(w.peak_dispatch_iter_groups, iter_g);
			update_max(w.peak_dispatch_init_groups, init_g);
		}

	}

	return w;
}

void EGTB_Generator::set_active_fusion(const Pawn_Slice_Manager& psm,
                                       const std::vector<int32_t>& fusion)
{
	m_phase_pinned = false;
	m_active_pawn_slices.clear();
	for (int32_t pair_sid : fusion)
	{
		const auto m = psm.pair_members(pair_sid);
		m_active_pawn_slices.insert(m_active_pawn_slices.end(), m.begin(), m.end());
	}
	std::sort(m_active_pawn_slices.begin(), m_active_pawn_slices.end());
	m_active_pawn_slices.erase(
		std::unique(m_active_pawn_slices.begin(), m_active_pawn_slices.end()),
		m_active_pawn_slices.end());

	m_targets_by_active.resize(m_active_pawn_slices.size());
	for (size_t i = 0; i < m_active_pawn_slices.size(); ++i)
		psm.push_target_slices(m_active_pawn_slices[i], m_targets_by_active[i]);
}

void mark_king_neighbor_reach(uint8_t* need, const Piece_Config_For_Gen& epsi,
                              const std::vector<int32_t>& active_pids,
                              size_t g_start, size_t g_end, size_t spg,
                              Slice_Reach_Scratch& scratch)
{
	const size_t nks = epsi.num_king_slices();
	const auto& ksm = epsi.king_slice_manager();

	scratch.kid_seen.assign(nks, 0);
	scratch.kid_expanded.assign(nks, 0);
	scratch.kids.clear();
	auto add_kid = [&](int32_t k) {
		uint8_t& seen = scratch.kid_seen[static_cast<size_t>(k)];
		if (seen) return;
		seen = 1;
		scratch.kids.push_back(k);
	};
	for (int32_t pid : active_pids)
	{
		const size_t pid_base = static_cast<size_t>(pid) * nks;
		const size_t s_lo = std::max(g_start, pid_base);
		const size_t s_hi = std::min(g_end,   pid_base + nks);
		if (s_lo >= s_hi) continue;
		for (size_t s = s_lo; s < s_hi; ++s)
		{
			const int32_t kid = static_cast<int32_t>(s - pid_base);
			if (scratch.kid_expanded[static_cast<size_t>(kid)]) continue;
			scratch.kid_expanded[static_cast<size_t>(kid)] = 1;
			add_kid(kid);
			for (int32_t k : ksm.neighbors(kid)) add_kid(k);
		}
	}
	// File mirroring can send a king move to mirror(pid).
	for (int32_t pp : active_pids)
	{
		const size_t pp_base = static_cast<size_t>(pp) * nks;
		for (int32_t k : scratch.kids)
			need[(pp_base + static_cast<size_t>(k)) / spg] = 1;
	}
}

void mark_push_target_reach(uint8_t* need, const Piece_Config_For_Gen& epsi,
                            const std::vector<int32_t>& active_pids,
                            const std::vector<std::vector<int32_t>>& targets_by_active,
                            size_t g_start, size_t g_end, size_t spg)
{
	if (!epsi.pawn_slice_manager().has_pawns()) return;
	const size_t nks = epsi.num_king_slices();

	for (size_t i = 0; i < active_pids.size(); ++i)
	{
		const int32_t pid = active_pids[i];
		const auto& targets = targets_by_active[i];
		if (targets.empty()) continue;
		const size_t pid_base = static_cast<size_t>(pid) * nks;
		const size_t s_lo = std::max(g_start, pid_base);
		const size_t s_hi = std::min(g_end,   pid_base + nks);
		if (s_lo >= s_hi) continue;
		for (size_t s = s_lo; s < s_hi; ++s)
		{
			const size_t kid = s - pid_base;
			for (int32_t tpid : targets)
				need[(static_cast<size_t>(tpid) * nks + kid) / spg] = 1;
		}
	}
}

template <typename EntryT, typename... OtherEntryTs>
void EGTB_Generator::refresh_active_metadata(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl)
{
	const size_t nps = m_epsi.num_pawn_slices();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = tbl.slices_per_group();
	const size_t ngroups = tbl.num_groups();

	if (m_pid_in_pair.size() != nps)
		m_pid_in_pair.assign(nps, 0);
	else
		for (int32_t pid : m_pid_in_pair_marked)
			m_pid_in_pair[static_cast<size_t>(pid)] = 0;
	m_pid_in_pair_marked = m_active_pawn_slices;
	for (int32_t pid : m_active_pawn_slices)
		m_pid_in_pair[static_cast<size_t>(pid)] = 1;

	m_all_groups.assign(ngroups, 1);

	m_fusion_groups.assign(ngroups, 0);
	for (int32_t pid : m_active_pawn_slices)
	{
		const size_t base = static_cast<size_t>(pid) * nks;
		for (size_t k = 0; k < nks; ++k)
			m_fusion_groups[(base + k) / spg] = 1;
	}

	m_pair_group_ids.clear();
	m_pair_group_ids.reserve(ngroups);
	for (size_t g = 0; g < ngroups; ++g)
		if (m_fusion_groups[g]) m_pair_group_ids.push_back(g);
	m_fusion_group_count = m_pair_group_ids.size();

	m_fusion_init_groups = m_fusion_groups;
	for (const auto& targets : m_targets_by_active)
		for (int32_t tpid : targets)
		{
			const size_t base = static_cast<size_t>(tpid) * nks;
			for (size_t k = 0; k < nks; ++k)
				m_fusion_init_groups[(base + k) / spg] = 1;
		}
	m_fusion_init_group_count = 0;
	for (size_t g = 0; g < ngroups; ++g)
		m_fusion_init_group_count += m_fusion_init_groups[g];
}

template <typename EntryT, typename... OtherEntryTs>
std::vector<std::vector<int32_t>>
EGTB_Generator::compute_fusion_groups(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl,
                                      const std::vector<int32_t>& batch,
                                      size_t own_layers, size_t target_layers) const
{
	if (batch.empty()) return {};
	if (m_paging_budget_bytes == 0) return { batch };

	const auto& psm = m_epsi.pawn_slice_manager();
	const size_t nks = m_epsi.num_king_slices();
	const size_t spg = tbl.slices_per_group();
	// One layer of one group for both colors is the unit used below.
	const size_t bytes_per_group =
		spg * m_epsi.within_slice_size() * sizeof(EntryT) * COLOR_NB;
	const size_t budget_slots =
		std::max<size_t>(1, m_paging_budget_bytes / bytes_per_group);

	const size_t ngroups = tbl.num_groups();
	Group_Set pair_own(ngroups), pair_target(ngroups);
	Group_Set covered_own(ngroups), covered_target(ngroups);
	std::vector<int32_t> targets;

	auto add_pid_range = [&](Group_Set& dst, int32_t pid) {
		const size_t base = static_cast<size_t>(pid) * nks;
		const size_t first_g = base / spg;
		const size_t last_g  = (base + nks - 1) / spg;
		for (size_t gid = first_g; gid <= last_g; ++gid) dst.insert(gid);
	};
	auto pair_groups = [&](int32_t pair_sid) {
		pair_own.reset();
		pair_target.reset();
		for (int32_t pid : psm.pair_members(pair_sid))
		{
			add_pid_range(pair_own, pid);
			psm.push_target_slices(pid, targets);
			for (int32_t tpid : targets)
				add_pid_range(pair_target, tpid);
		}
	};

	std::vector<std::vector<int32_t>> fusions;
	fusions.emplace_back();

	size_t both = 0;
	auto slots = [&](size_t own_extra, size_t target_extra, size_t promoted) {
		return (covered_own.count() + own_extra) * own_layers
		     + (covered_target.count() - both - promoted + target_extra) * target_layers;
	};

	for (int32_t pair_sid : batch)
	{
		pair_groups(pair_sid);
		size_t added_own = 0, added_target = 0, promoted = 0;
		for (size_t g : pair_own.ids)
		{
			if (covered_own.contains(g)) continue;
			++added_own;
			if (covered_target.contains(g)) ++promoted;
		}
		for (size_t g : pair_target.ids)
			if (!covered_target.contains(g) && !covered_own.contains(g)
			    && !pair_own.contains(g)) ++added_target;

		if (!fusions.back().empty()
		    && slots(added_own, added_target, promoted) > budget_slots)
		{
			fusions.emplace_back();
			covered_own.reset();
			covered_target.reset();
			both = 0;
		}
		for (size_t g : pair_own.ids)
		{
			if (covered_own.contains(g)) continue;
			covered_own.insert(g);
			if (covered_target.contains(g)) ++both;
		}
		for (size_t g : pair_target.ids)
			if (!covered_own.contains(g)) covered_target.insert(g);
		fusions.back().push_back(pair_sid);
	}
	if (fusions.back().empty()) fusions.pop_back();
	return fusions;
}

template <typename EntryT, typename... OtherEntryTs>
void EGTB_Generator::apply_working_set(
	In_Out_Param<Thread_Pool> thread_pool,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
	const std::vector<uint8_t>& needed_w,
	const std::vector<uint8_t>& needed_b)
{
	using Table = Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>;
	struct Group_Ref { Table* tbl; size_t group_id; };

	Table* tbls[COLOR_NB] = { w_tbl, b_tbl };
	const std::vector<uint8_t>* needed[COLOR_NB] = { &needed_w, &needed_b };

	const size_t ng = w_tbl->num_groups();
	const size_t num_layers = needed_w.size() / ng;

	const size_t bytes_per_group =
		w_tbl->within_slice_size() * w_tbl->slices_per_group() * sizeof(EntryT);

	++m_paging_tick;

	std::vector<Group_Ref> loads;
	size_t live_groups = 0;
	for (Color c : { WHITE, BLACK })
	{
		const uint8_t* need = needed[c]->data();
		for (size_t layer = 0; layer < num_layers; ++layer)
		{
			Table& tbl = tbls[c][layer];
			ASSERT(tbl.num_groups() == ng);
			const uint8_t* need_layer = need + layer * ng;
			for (size_t group = 0; group < ng; ++group)
			{
				const bool resident = tbl.is_group_resident(group);
				if (need_layer[group])
					tbl.set_last_used(group, m_paging_tick);
				if (need_layer[group] && !resident)
					loads.push_back({ &tbl, group });
				if (resident || need_layer[group])
					++live_groups;
			}
		}
	}

	std::vector<Group_Ref> evicts;
	if (m_paging_budget_bytes > 0)
	{
		size_t future_bytes = live_groups * bytes_per_group;
		if (future_bytes > m_paging_budget_bytes)
		{
			struct Candidate { Table* tbl; size_t group; bool dirty; bool exempt; };
			std::vector<Candidate> victims;
			for (Color c : { WHITE, BLACK })
			{
				const uint8_t* need = needed[c]->data();
				for (size_t layer = 0; layer < num_layers; ++layer)
				{
					Table& tbl = tbls[c][layer];
					const uint8_t* need_layer = need + layer * ng;
					const bool exempt = (num_layers > 1 && layer == 0);
					for (size_t group = 0; group < ng; ++group)
						if (!need_layer[group] && tbl.is_group_resident(group))
							victims.push_back({ &tbl, group, tbl.is_group_dirty(group), exempt });
				}
			}
			std::sort(victims.begin(), victims.end(),
				[](const Candidate& lhs, const Candidate& rhs) {
					if (lhs.exempt != rhs.exempt)
						return !lhs.exempt;
					if (lhs.dirty != rhs.dirty)
						return !lhs.dirty;
					if (lhs.dirty)
						return lhs.tbl->last_used(lhs.group) > rhs.tbl->last_used(rhs.group);
					return lhs.tbl->last_used(lhs.group) < rhs.tbl->last_used(rhs.group);
				});
			for (const Candidate& victim : victims)
			{
				if (future_bytes <= m_paging_budget_bytes) break;
				evicts.push_back({ victim.tbl, victim.group });
				future_bytes -= bytes_per_group;
			}
		}
	}

	if (evicts.empty() && loads.empty()) return;

	const size_t n = std::max(evicts.size(), loads.size());
	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_all_threads([&](size_t) {
		for (;;)
		{
			const size_t idx = next.fetch_add(1);
			if (idx >= n) return;
			if (idx < evicts.size())
				evicts[idx].tbl->evict_group(evicts[idx].group_id);
			if (idx < loads.size())
				loads[idx].tbl->load_group(loads[idx].group_id);
		}
	});
}

template <typename EntryT, typename... OtherEntryTs>
bool EGTB_Generator::try_pin_phase(
	In_Out_Param<Thread_Pool> thread_pool,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
	const std::vector<uint8_t>& needed_w,
	const std::vector<uint8_t>& needed_b,
	size_t group_charge)
{
	const size_t bytes_per_group =
		w_tbl->within_slice_size() * w_tbl->slices_per_group() * sizeof(EntryT);
	if (m_paging_budget_bytes != 0 && bytes_per_group != 0
	    && group_charge > m_paging_budget_bytes / bytes_per_group)
		return false;

	apply_working_set(thread_pool, w_tbl, b_tbl, needed_w, needed_b);
	return true;
}

template <typename EntryT, typename... OtherEntryTs>
Block_Source make_entry_block_source(
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& src,
	Save_Group_Cache<EntryT, OtherEntryTs...>& cache,
	Color color,
	Index_Permutation_Plan perm_plan,
	size_t block_size,
	size_t entry_bytes)
{
	constexpr size_t kEntry = sizeof(EntryT);
	ASSERT(block_size % entry_bytes == 0);
	const size_t source_block_bytes = block_size * kEntry / entry_bytes;
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t total_entries = src.num_slices() * within;
	const size_t source_total_bytes = total_entries * kEntry;
	const size_t output_total_bytes = total_entries * entry_bytes;
	return Block_Source{
		output_total_bytes,
		[&src, &cache, color, within, spg, source_block_bytes, source_total_bytes, perm_plan](size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t> {
			const size_t block_off = block_id * source_block_bytes;
			const size_t this_block = std::min(source_block_bytes, source_total_bytes - block_off);
			ASSERT(scratch.size() >= this_block);
			ASSERT(block_off % kEntry == 0);
			ASSERT(this_block % kEntry == 0);

			const size_t storage_entry_off = block_off / kEntry;
			const size_t entry_cnt = this_block / kEntry;

			const size_t first_g = (storage_entry_off / within) / spg;
			const size_t last_g  = (entry_cnt == 0 ? first_g
			                                       : ((storage_entry_off + entry_cnt - 1) / within) / spg);
			Pinned_Group_Range<EntryT, OtherEntryTs...> pin(cache, color, first_g, last_g);

			for (size_t done = 0; done < entry_cnt; ++done)
			{
				const size_t storage_idx = storage_entry_off + done;
				const size_t logical_idx = storage_index_to_logical_index(perm_plan, storage_idx);
				const auto e = src.template view_at<EntryT>(static_cast<Board_Index>(logical_idx));
				std::memcpy(scratch.data() + done * kEntry, &e, kEntry);
			}

			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

template void EGTB_Generator::refresh_active_metadata<DTZ_Final_Entry, DTZ_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>&);
template void EGTB_Generator::refresh_active_metadata<DTC_Final_Entry, DTC_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&);
template void EGTB_Generator::refresh_active_metadata<DTM_Final_Entry, DTM_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&);
template void EGTB_Generator::refresh_active_metadata<DTM50_Final_Entry, DTM50_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&);

template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTZ_Final_Entry, DTZ_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, size_t) const;
template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTC_Final_Entry, DTC_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, size_t) const;
template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTM_Final_Entry, DTM_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, size_t) const;
template std::vector<std::vector<int32_t>> EGTB_Generator::compute_fusion_groups<DTM50_Final_Entry, DTM50_Intermediate_Entry>(const Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, const std::vector<int32_t>&, size_t, size_t) const;

template void EGTB_Generator::apply_working_set<DTZ_Final_Entry, DTZ_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);
template void EGTB_Generator::apply_working_set<DTC_Final_Entry, DTC_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);
template void EGTB_Generator::apply_working_set<DTM_Final_Entry, DTM_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);
template void EGTB_Generator::apply_working_set<DTM50_Final_Entry, DTM50_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&);

template bool EGTB_Generator::try_pin_phase<DTZ_Final_Entry, DTZ_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&, size_t);
template bool EGTB_Generator::try_pin_phase<DTC_Final_Entry, DTC_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&, size_t);
template bool EGTB_Generator::try_pin_phase<DTM_Final_Entry, DTM_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&, size_t);
template bool EGTB_Generator::try_pin_phase<DTM50_Final_Entry, DTM50_Intermediate_Entry>(In_Out_Param<Thread_Pool>, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>*, const std::vector<uint8_t>&, const std::vector<uint8_t>&, size_t);

template Block_Source make_entry_block_source<DTZ_Final_Entry, DTZ_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTZ_Final_Entry, DTZ_Intermediate_Entry>&, Save_Group_Cache<DTZ_Final_Entry, DTZ_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);
template Block_Source make_entry_block_source<DTC_Final_Entry, DTC_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&, Save_Group_Cache<DTC_Final_Entry, DTC_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);
template Block_Source make_entry_block_source<DTM_Final_Entry, DTM_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&, Save_Group_Cache<DTM_Final_Entry, DTM_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);
template Block_Source make_entry_block_source<DTM50_Final_Entry, DTM50_Intermediate_Entry>(Sliced_EGTB_File_For_Gen<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, Save_Group_Cache<DTM50_Final_Entry, DTM50_Intermediate_Entry>&, Color, Index_Permutation_Plan, size_t, size_t);

namespace {

void decompress_bytes_into(const uint8_t* src, size_t src_size,
                           uint8_t* dst, size_t want)
{
	const int ret = LZ4_decompress_safe(
		reinterpret_cast<const char*>(src),
		reinterpret_cast<char*>(dst),
		static_cast<int>(src_size),
		static_cast<int>(want));
	if (ret <= 0 || static_cast<size_t>(ret) != want)
		print_and_abort("Slice LZ4 decompress failed\n");
}

struct Spill_Header
{
	uint64_t magic;
	uint64_t uncompressed_size;
	uint64_t compressed_size;
};
static_assert(sizeof(Spill_Header) == 24);

constexpr size_t SPILL_CHUNK_BYTES = 4ull * 1024ull * 1024ull;
static_assert(SPILL_CHUNK_BYTES <= static_cast<size_t>(LZ4_MAX_INPUT_SIZE));

size_t spill_scratch_bytes(size_t bytes)
{
	const size_t chunk = bytes < SPILL_CHUNK_BYTES ? bytes : SPILL_CHUNK_BYTES;
	return static_cast<size_t>(LZ4_compressBound(narrowing_static_cast<int>(chunk)));
}

}  // namespace

void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic)
{
	std::ofstream fp(path, std::ios::binary | std::ios::trunc);
	if (!fp) print_and_abort("Could not open spill file for write: %s\n", path.string().c_str());

	std::vector<uint8_t> out;
	for (size_t off = 0; off < bytes; off += SPILL_CHUNK_BYTES)
	{
		const size_t remain = bytes - off;
		const int src_size = narrowing_static_cast<int>(remain < SPILL_CHUNK_BYTES ? remain : SPILL_CHUNK_BYTES);
		out.resize(static_cast<size_t>(LZ4_compressBound(src_size)));
		const int n = LZ4_compress_default(
			reinterpret_cast<const char*>(data + off),
			reinterpret_cast<char*>(out.data()),
			src_size,
			narrowing_static_cast<int>(out.size()));
		if (n <= 0) print_and_abort("LZ4 compress failed for spill file: %s\n", path.string().c_str());

		Spill_Header hdr{};
		hdr.magic = magic;
		hdr.uncompressed_size = static_cast<uint64_t>(src_size);
		hdr.compressed_size = static_cast<uint64_t>(n);

		fp.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
		fp.write(reinterpret_cast<const char*>(out.data()), n);
		if (!fp) print_and_abort("Write error on spill file: %s\n", path.string().c_str());
	}

	fp.close();
	if (!fp) print_and_abort("Close error on spill file: %s\n", path.string().c_str());
}

void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic)
{
	std::ifstream fp(path, std::ios::binary);
	if (!fp) print_and_abort("Could not open spill file for read: %s\n", path.string().c_str());

	std::vector<uint8_t> buf;
	buf.reserve(spill_scratch_bytes(bytes));
	size_t off = 0;
	while (off < bytes)
	{
		Spill_Header hdr{};
		fp.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
		if (!fp) print_and_abort("Spill file truncated header: %s\n", path.string().c_str());
		if (hdr.magic != expected_magic)
			print_and_abort("Spill file magic mismatch: %s\n", path.string().c_str());
		if (hdr.uncompressed_size == 0 || hdr.uncompressed_size > bytes - off
			|| hdr.uncompressed_size > static_cast<uint64_t>(LZ4_MAX_INPUT_SIZE))
			print_and_abort("Spill file size mismatch: %s\n", path.string().c_str());
		buf.resize(hdr.compressed_size);
		fp.read(reinterpret_cast<char*>(buf.data()), buf.size());
		if (!fp) print_and_abort("Spill file truncated payload: %s\n", path.string().c_str());

		decompress_bytes_into(buf.data(), buf.size(), data + off, hdr.uncompressed_size);
		off += hdr.uncompressed_size;
	}
}
