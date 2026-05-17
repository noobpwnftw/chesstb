#include "probe/probe.h"
#include "probe/entry.h"
#include "probe/position_index.h"
#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/filesystem.h"
#include "util/math.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {


bool find_in_dirs(const Piece_Config& ps, const char* ext,
                  const std::vector<std::filesystem::path>& dirs,
                  std::filesystem::path* out)
{
	const std::string name = ps.name() + ext;
	for (const auto& d : dirs)
	{
		auto p = path_join(d, name);
		if (std::filesystem::exists(p))
		{
			if (out) *out = std::move(p);
			return true;
		}
	}
	return false;
}

// Open-or-build behind a two-tier cache: a lock-free thread-local front (keyed
// by `epoch`) over the shared per-key map under `mu`. `make` runs only on a
// full miss and outside the write lock, returning the owning pointer to insert
// — which may be null (e.g. no table on disk). Concurrent builders race on
// try_emplace; the loser's freshly built object is discarded.
template <typename Map, typename Make>
auto cached_open(uint64_t epoch, std::shared_mutex& mu, Map& cache, uint32_t k, Make&& make)
	-> typename Map::mapped_type::element_type*
{
	using T = typename Map::mapped_type::element_type;
	thread_local TL_Cache<T> tl;
	T* hit;
	if (tl.lookup(epoch, k, hit)) return hit;
	{
		std::shared_lock rlk(mu);
		auto it = cache.find(k);
		if (it != cache.end())
		{
			tl.insert(epoch, k, it->second.get());
			return it->second.get();
		}
	}
	typename Map::mapped_type built = make();
	std::unique_lock wlk(mu);
	auto [it, inserted] = cache.try_emplace(k, std::move(built));
	T* raw = it->second.get();
	tl.insert(epoch, k, raw);
	return raw;
}

// Literal key detects whether canonicalization swapped colors.
struct Config_And_Literal_Key
{
	Piece_Config cfg;
	Material_Key literal_key;
};

Config_And_Literal_Key piece_config_and_literal_key_from_position(const Position& pos)
{
	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	Material_Key literal_key;
	for (Piece pc : ALL_PIECES)
	{
		const size_t cnt = pos.piece_bb(pc).num_set_bits();
		for (size_t i = 0; i < cnt; ++i)
		{
			pieces[n++] = pc;
			literal_key.add_piece(pc);
		}
	}
	return { Piece_Config(Const_Span<Piece>(pieces.data(), n)), literal_key };
}

Position mirror_for_canonical(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
	{
		const Piece p = pos.piece_at(sq);
		if (p != PIECE_NONE)
			swapped.put_piece(piece_opp_color(p), sq_rank_mirror(sq));
	}
	swapped.set_turn(color_opp(pos.turn()));
	return swapped;
}

constexpr int MAX_DERIVE_DEPTH = 16;

NODISCARD WDL_Entry invert_wdl(WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::WIN:          return WDL_Entry::LOSE;
		case WDL_Entry::CURSED_WIN:   return WDL_Entry::BLESSED_LOSS;
		case WDL_Entry::DRAW:         return WDL_Entry::DRAW;
		case WDL_Entry::BLESSED_LOSS: return WDL_Entry::CURSED_WIN;
		case WDL_Entry::LOSE:         return WDL_Entry::WIN;
		case WDL_Entry::ILLEGAL:      return WDL_Entry::ILLEGAL;
	}
	return WDL_Entry::ILLEGAL;
}

// Invert a quiet child's stored class to the mover's, across one quiet ply. A
// rule-edge marker tips one ply past the 50mr boundary: BOUNDARY_LOSS -> we win
// but only cursed, BOUNDARY_WIN -> we lose but only blessed. The five plain
// codes invert like invert_wdl.
NODISCARD WDL_Entry invert_stored(WDL_Stored s)
{
	switch (s)
	{
		case WDL_Stored::WIN:           return WDL_Entry::LOSE;
		case WDL_Stored::CURSED_WIN:    return WDL_Entry::BLESSED_LOSS;
		case WDL_Stored::DRAW:          return WDL_Entry::DRAW;
		case WDL_Stored::BLESSED_LOSS:  return WDL_Entry::CURSED_WIN;
		case WDL_Stored::LOSE:          return WDL_Entry::WIN;
		case WDL_Stored::BOUNDARY_LOSS: return WDL_Entry::CURSED_WIN;
		case WDL_Stored::BOUNDARY_WIN:  return WDL_Entry::BLESSED_LOSS;
		case WDL_Stored::ILLEGAL:       return WDL_Entry::ILLEGAL;
	}
	return WDL_Entry::ILLEGAL;
}

NODISCARD WDL_Entry fold_dtm_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN) return WDL_Entry::WIN;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::LOSE;
	return w;
}

// 5-class WDL → DTM50's 3-class: cursed/blessed are unreachable under 50MR.
NODISCARD WDL_Entry fold_dtm50_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN)   return WDL_Entry::DRAW;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::DRAW;
	return w;
}

NODISCARD bool move_is_zeroing(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN
	    || pos.piece_at(m.to()) != PIECE_NONE
	    || m.is_ep_capture();
}

NODISCARD Square ep_square_after_double_push(Move m)
{
	ASSERT(is_pawn_double_push(m));
	const int from_rank = static_cast<int>(sq_rank(m.from()));
	const int to_rank = static_cast<int>(sq_rank(m.to()));
	return sq_make(static_cast<Rank>((from_rank + to_rank) / 2), sq_file(m.from()));
}

NODISCARD bool move_is_pawn_double_push(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN && is_pawn_double_push(m);
}

NODISCARD int wdl_rank(WDL_Entry w)
{
	switch (w) {
		case WDL_Entry::WIN:          return 4;
		case WDL_Entry::CURSED_WIN:   return 3;
		case WDL_Entry::DRAW:         return 2;
		case WDL_Entry::BLESSED_LOSS: return 1;
		case WDL_Entry::LOSE:         return 0;
		case WDL_Entry::ILLEGAL:      return -1;
	}
	return -1;
}

NODISCARD bool prefer_new(WDL_Entry new_wdl, uint16_t new_dtc,
                          WDL_Entry old_wdl, uint16_t old_dtc)
{
	const int rn = wdl_rank(new_wdl), ro = wdl_rank(old_wdl);
	if (rn != ro) return rn > ro;
	const bool win_side = (new_wdl == WDL_Entry::WIN || new_wdl == WDL_Entry::CURSED_WIN);
	const bool loss_side = (new_wdl == WDL_Entry::LOSE || new_wdl == WDL_Entry::BLESSED_LOSS);
	if (win_side)  return new_dtc < old_dtc;
	if (loss_side) return new_dtc > old_dtc;
	return false;
}

bool is_symmetric_material(const Piece_Config& ps)
{
	const auto [mat_key, mir_key] = ps.material_keys();
	return mat_key == mir_key;
}

Position mirror_symmetric_stm(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
	{
		const Piece p = pos.piece_at(sq);
		if (p != PIECE_NONE)
			swapped.put_piece(piece_opp_color(p), sq_rank_mirror(sq));
	}
	swapped.set_turn(color_opp(pos.turn()));
	return swapped;
}

struct Child_Pos { Position pos; Piece_Config ps; Square ep; bool is_kk; bool is_zeroing; };

struct DTM50_Result
{
	WDL_Entry wdl = WDL_Entry::ILLEGAL;
	uint16_t dtm = 0;
};

void add_ep_moves(const Position& pos, Square ep_square, Move_List& ml)
{
	if (ep_square == SQ_END) return;
	const Color me = pos.turn();
	const Rank target_rank = (me == WHITE) ? RANK_6 : RANK_3;
	const Rank pawn_rank   = (me == WHITE) ? RANK_5 : RANK_4;
	if (sq_rank(ep_square) != target_rank) return;

	const File target_file = sq_file(ep_square);
	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(target_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square from = sq_make(pawn_rank, static_cast<File>(f));
		if (pos.piece_at(from) != piece_make(me, PAWN)) continue;

		const Square cap_sq = sq_make(pawn_rank, target_file);
		if (pos.piece_at(cap_sq) != piece_make(color_opp(me), PAWN)) continue;

		ml.add(Move::make_ep_capture(from, ep_square));
	}
}

NODISCARD bool has_legal_move(const Position& pos)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	for (size_t i = 0; i < ml.size(); ++i)
		if (pos.is_pseudo_legal_move_legal(ml[i], ctx)) return true;
	return false;
}

NODISCARD bool is_checkmate(const Position& pos)
{
	return pos.is_in_check(pos.turn()) && !has_legal_move(pos);
}

// Recover mate-in-1 the layered DTM50 decoder collapses to DRAW at hmc>0.
NODISCARD DTM50_Result recover_mate_at_hmc(const Position& pos, WDL_Entry wdl)
{
	if (wdl == WDL_Entry::WIN)
	{
		Position p = pos;
		Move_List ml;
		p.gen_pseudo_legal_moves(out_param(ml));
		const Position::Legality ctx = p.legality_context();
		for (size_t i = 0; i < ml.size(); ++i)
		{
			if (!p.is_pseudo_legal_move_legal(ml[i], ctx)) continue;
			Position child = p;
			(void)child.do_move(ml[i]);
			if (is_checkmate(child))
				return { WDL_Entry::WIN, 1 };
		}
		return { WDL_Entry::DRAW, 0 };
	}
	if (wdl == WDL_Entry::LOSE)
	{
		if (is_checkmate(pos))
			return { WDL_Entry::LOSE, 0 };
		return { WDL_Entry::DRAW, 0 };
	}
	return { WDL_Entry::DRAW, 0 };
}

struct Canonical_Root
{
	Piece_Config ps;
	Position pos;
	Square ep_square = SQ_END;
	bool mirrored = false;
};

}  // namespace

struct Probe_Tables::Impl
{
	std::atomic<uint64_t> epoch{ next_epoch() };
	std::vector<std::filesystem::path> wdl_dirs = { "./wdl/" };
	std::vector<std::filesystem::path> dtc_dirs = { "./dtc/" };
	std::vector<std::filesystem::path> dtm_dirs = { "./dtm/" };
	std::vector<std::filesystem::path> dtm50_dirs = { "./dtm50/" };

	// Keyed by material-key int to avoid per-probe string hashing.
	mutable std::shared_mutex                                 wdl_mu;
	std::unordered_map<uint32_t, std::unique_ptr<WDL_File>> wdl_cache;
	mutable std::shared_mutex                                 dtc_mu;
	std::unordered_map<uint32_t, std::unique_ptr<DTC_File>> dtc_cache;
	mutable std::shared_mutex                                 dtm_mu;
	std::unordered_map<uint32_t, std::unique_ptr<DTM_File>> dtm_cache;
	mutable std::shared_mutex                                 dtm50_mu;
	std::unordered_map<uint32_t, std::unique_ptr<DTM50_File>> dtm50_cache;
	mutable std::shared_mutex                                 epsi_mu;
	std::unordered_map<uint32_t, std::unique_ptr<Position_Index_Config>> epsi_cache;

	std::atomic<size_t> largest_pieces{0};

	// Builds the table for `ps` if a matching file exists in `dirs`, else
	// caches a null entry so the lookup isn't retried on every probe.
	template <typename File>
	NODISCARD File* open_table(
		std::shared_mutex& mu, std::unordered_map<uint32_t, std::unique_ptr<File>>& cache,
		const std::vector<std::filesystem::path>& dirs, const char* ext, const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().cache_key();
		return cached_open(epoch.load(std::memory_order_acquire), mu, cache, k, [&]() -> std::unique_ptr<File> {
			std::filesystem::path path;
			if (!find_in_dirs(ps, ext, dirs, &path)) return nullptr;
			auto f = std::make_unique<File>();
			f->load(ps, path);
			return f;
		});
	}

	NODISCARD const Position_Index_Config& get_epsi(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().cache_key();
		return *cached_open(epoch.load(std::memory_order_acquire), epsi_mu, epsi_cache, k,
			[&] { return std::make_unique<Position_Index_Config>(ps); });
	}

	NODISCARD WDL_File*   open_wdl  (const Piece_Config& ps) { return open_table(wdl_mu,   wdl_cache,   wdl_dirs,   WDL_EXT,   ps); }
	NODISCARD DTC_File*   open_dtc  (const Piece_Config& ps) { return open_table(dtc_mu,   dtc_cache,   dtc_dirs,   DTC_EXT,   ps); }
	NODISCARD DTM_File*   open_dtm  (const Piece_Config& ps) { return open_table(dtm_mu,   dtm_cache,   dtm_dirs,   DTM_EXT,   ps); }
	NODISCARD DTM50_File* open_dtm50(const Piece_Config& ps) { return open_table(dtm50_mu, dtm50_cache, dtm50_dirs, DTM50_EXT, ps); }

	// Whether `ps` is probeable on disk, to decide frozen-pair routing: prefer
	// the 'p' table for an opposing-pair position only if present.
	NODISCARD bool has_any_table(const Piece_Config& ps)
	{
		return open_wdl(ps) != nullptr;
	}

	NODISCARD Probe_Result probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth);
	NODISCARD WDL_Entry probe_wdl_impl(const Piece_Config& ps, const Position& pos, Square ep_square, int depth);
	// Frozen-pair routing bound to this Impl's table availability: the 'p'-table
	// root when `pos` has an opposing pair whose table exists, else nullopt. Lets
	// the root-move rankers steer children that keep the pair to the 'p' table.
	NODISCARD std::optional<Canonical_Root> route_pair(const Position& pos, Square ep_square);
	// Apply `m` to `parent` and resolve the child to probe: prefers the child's
	// frozen-pair table when on disk (a move that keeps the pair stays in a 'p'
	// material the board-derived config would miss), else its full material.
	// Owns the child ep so the routed/mirrored board and ep can't desync.
	NODISCARD Child_Pos make_child(const Position& parent, Move m);
	// The *_internal helpers take an already-opened table handle; they never re-open it.
	NODISCARD WDL_Entry probe_wdl_internal(WDL_File* w, const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD WDL_Stored read_wdl_stored(WDL_File* w, const Position& pos);
	NODISCARD std::optional<uint16_t> probe_dtc_internal(DTC_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD std::optional<uint16_t> probe_dtm_internal(DTM_File* m, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM50_Result probe_dtm50_internal(DTM50_File* m, const Piece_Config& ps, const Position& pos,
	                                            WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD WDL_Entry derive_wdl(const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> derive_dtc(const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> derive_dtm(const Position& pos, int depth);
	NODISCARD DTM50_Result derive_dtm50(const Position& pos, unsigned rule50, int depth);
	NODISCARD DTM50_Result derive_dtm50_flat(const Position& pos, int depth);


	void scan_paths();

	void invalidate_tables()
	{
		{ std::unique_lock lk(wdl_mu);   wdl_cache.clear();   }
		{ std::unique_lock lk(dtc_mu);   dtc_cache.clear();   }
		{ std::unique_lock lk(dtm_mu);   dtm_cache.clear();   }
		{ std::unique_lock lk(dtm50_mu); dtm50_cache.clear(); }
		epoch.store(next_epoch(), std::memory_order_release);
	}
};

// Raw on-disk code, markers intact. derive_wdl reads quiet children this way —
// they always land in the kept opposite-stm frame of the same material.
WDL_Stored Probe_Tables::Impl::read_wdl_stored(WDL_File* w, const Position& pos)
{
	if (!w) return WDL_Stored::ILLEGAL;

	return w->read(pos.turn(), pos);
}

// Semantic WDL for `pos`: read from the stored frame (markers folded), or
// reconstructed by derive_wdl when this stm's frame was dropped.
WDL_Entry Probe_Tables::Impl::probe_wdl_internal(WDL_File* w, const Piece_Config& ps, const Position& pos, int depth)
{
	if (!w) return WDL_Entry::ILLEGAL;

	const Color stm = pos.turn();
	if (w->is_dropped[stm])
	{
		if (!is_symmetric_material(ps))
			return derive_wdl(pos, depth);
		const Position mp = mirror_symmetric_stm(pos);
		return wdl_from_storage(w->read(mp.turn(), mp));
	}
	return wdl_from_storage(w->read(stm, pos));
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtc_internal(
	DTC_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	if (!d) return std::nullopt;

	const Color stm = pos.turn();
	if (d->is_dropped[stm])
	{
		if (!is_symmetric_material(ps))
			return derive_dtc(pos, depth);
		const Position mp = mirror_symmetric_stm(pos);
		return d->read(mp.turn(), mp, wdl);
	}
	return d->read(stm, pos, wdl);
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtm_internal(
	DTM_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	if (!d) return std::nullopt;

	const Color stm = pos.turn();
	if (d->is_dropped[stm])
	{
		if (!is_symmetric_material(ps))
			return derive_dtm(pos, depth);
		const Position mp = mirror_symmetric_stm(pos);
		return d->read(mp.turn(), mp, wdl);
	}
	return d->read(stm, pos, wdl);
}

DTM50_Result Probe_Tables::Impl::probe_dtm50_internal(
	DTM50_File* m, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	const bool flat = (rule50 == IGNORE_50MR);
	if (!flat && rule50 >= DTM50_HMC_COUNT)
		return { WDL_Entry::DRAW, 0 };
	if (!m) return {};

	const Color stm = pos.turn();
	DTM50_Result d;
	if (!m->is_dropped[stm])
		d = { flat ? wdl : fold_dtm50_wdl(wdl), m->read(stm, pos, wdl, rule50) };
	else if (!is_symmetric_material(ps))
		d = flat ? derive_dtm50_flat(pos, depth) : derive_dtm50(pos, rule50, depth);
	else
	{
		const Position mp = mirror_symmetric_stm(pos);
		d = { flat ? wdl : fold_dtm50_wdl(wdl), m->read(mp.turn(), mp, wdl, rule50) };
	}

	// See recover_mate_at_hmc.
	if (!flat && rule50 > 0 && d.dtm == 0 && (wdl == WDL_Entry::WIN || wdl == WDL_Entry::LOSE))
		return recover_mate_at_hmc(pos, wdl);
	return d;
}

// Reconstruct a dropped WDL frame by one-ply minimax over children. A quiet
// move keeps the child in this material's kept opposite-stm frame, read raw so
// invert_stored can tip a rule-edge marker for the +1 ply. A capture/pawn move
// resets the 50mr clock (no edge to cross) and may cross into a sub-tablebase
// whose frame is itself dropped, so it goes through probe_wdl_internal.
WDL_Entry Probe_Tables::Impl::derive_wdl(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best = WDL_Entry::LOSE;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry mw;
		if (c.is_kk)
		{
			mw = WDL_Entry::DRAW;
		}
		else if (c.is_zeroing)
		{
			const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			mw = invert_wdl(cw);
		}
		else
		{
			const WDL_Stored cs = read_wdl_stored(open_wdl(c.ps), c.pos);
			if (cs == WDL_Stored::ILLEGAL) continue;
			mw = invert_stored(cs);
		}

		if (wdl_rank(mw) > wdl_rank(best)) best = mw;
		have_candidate = true;
	}

	if (!any_legal) return ctx.in_check ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	if (!have_candidate) return WDL_Entry::ILLEGAL;
	return best;
}

std::optional<uint16_t> Probe_Tables::Impl::derive_dtc(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtc = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		uint16_t my_dtc;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			my_dtc = 1;
		}
		else if (c.is_zeroing)
		{
			cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			my_dtc = 1;
		}
		else
		{
			cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			const auto child_dtc = probe_dtc_internal(open_dtc(c.ps), c.ps, c.pos, cw, depth + 1);
			if (!child_dtc) continue;
			my_dtc = static_cast<uint16_t>(1u + *child_dtc);
		}

		WDL_Entry my_wdl = invert_wdl(cw);
		if (my_dtc > DTC_MAX_NON_CURSED_DTZ)
		{
			if (my_wdl == WDL_Entry::WIN)  my_wdl = WDL_Entry::CURSED_WIN;
			if (my_wdl == WDL_Entry::LOSE) my_wdl = WDL_Entry::BLESSED_LOSS;
		}

		if (!have_candidate || prefer_new(my_wdl, my_dtc, best_wdl, best_dtc))
		{
			best_wdl = my_wdl;
			best_dtc = my_dtc;
			have_candidate = true;
		}
	}

	if (!any_legal || best_wdl == WDL_Entry::DRAW)
		return 0;
	if (!have_candidate)
		return std::nullopt;
	return best_dtc;
}

std::optional<uint16_t> Probe_Tables::Impl::derive_dtm(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		uint16_t cd;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			cd = 0;
		}
		else
		{
			if (c.ep != SQ_END)
			{
				Probe_Result cr = probe_impl(c.ps, c.pos, IGNORE_50MR, c.ep, depth + 1);
				if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL || !cr.has_dtm)
					continue;
				cw = cr.wdl;
				cd = static_cast<uint16_t>(cr.dtm);
			}
			else
			{
				cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (cw == WDL_Entry::ILLEGAL) continue;
				const auto child_dtm = probe_dtm_internal(open_dtm(c.ps), c.ps, c.pos, cw, depth + 1);
				if (!child_dtm) continue;
				cd = *child_dtm;
			}
		}

		if (cw == WDL_Entry::CURSED_WIN)   cw = WDL_Entry::WIN;
		if (cw == WDL_Entry::BLESSED_LOSS) cw = WDL_Entry::LOSE;

		const WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtm = static_cast<uint16_t>(1u + cd);

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal)
		return 0;
	if (!have_candidate)
		return std::nullopt;

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return best_dtm;
	return 0;
}

// rule50-aware derive: per-child hmc (zeroing resets, quiet increments);
// once ≥100, the move is DRAW unless it mates.
DTM50_Result Probe_Tables::Impl::derive_dtm50(
	const Position& pos, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		const unsigned child_rule50 = c.is_zeroing ? 0u : (rule50 + 1u);

		DTM50_Result cd;
		if (c.is_kk)
		{
			cd = { WDL_Entry::DRAW, 0 };
		}
		else if (child_rule50 >= DTM50_HMC_COUNT)
		{
			// Quiet move past the 50MR window: DRAW unless the move is mate.
			cd = is_checkmate(c.pos)
				? DTM50_Result{ WDL_Entry::LOSE, 0 }
				: DTM50_Result{ WDL_Entry::DRAW, 0 };
		}
		else
		{
			if (c.ep != SQ_END)
			{
				Probe_Result cr = probe_impl(c.ps, c.pos, child_rule50, c.ep, depth + 1);
				if (cr.status != Probe_Result::Status::OK || !cr.has_dtm50)
					continue;
				cd = { cr.dtm50_wdl, static_cast<uint16_t>(cr.dtm50) };
			}
			else
			{
				const WDL_Entry cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (cw == WDL_Entry::ILLEGAL) continue;
				cd = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, cw, child_rule50, depth + 1);
				if (cd.wdl == WDL_Entry::ILLEGAL) continue;
			}
		}

		WDL_Entry cw = fold_dtm50_wdl(cd.wdl);  // cursed/blessed -> DRAW before inverting
		const WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.dtm));

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal)
		return ctx.in_check
			? DTM50_Result{ WDL_Entry::LOSE, 0 }
			: DTM50_Result{ WDL_Entry::DRAW, 0 };
	if (!have_candidate)
		return {};

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return { best_wdl, best_dtm };
	return { WDL_Entry::DRAW, 0 };
}

DTM50_Result Probe_Tables::Impl::derive_dtm50_flat(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);

		WDL_Entry cw;
		uint16_t  cd;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			cd = 0;
		}
		else if (c.ep != SQ_END)
		{
			Probe_Result cr = probe_impl(c.ps, c.pos, IGNORE_50MR, c.ep, depth + 1);
			if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL || !cr.has_dtm)
				continue;
			cw = cr.wdl;
			cd = static_cast<uint16_t>(cr.dtm);
		}
		else
		{
			cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			const DTM50_Result child = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, cw, IGNORE_50MR, depth + 1);
			if (child.wdl == WDL_Entry::ILLEGAL) continue;
			cw = child.wdl;
			cd = static_cast<uint16_t>(child.dtm);
		}

		if (cw == WDL_Entry::CURSED_WIN)   cw = WDL_Entry::WIN;
		if (cw == WDL_Entry::BLESSED_LOSS) cw = WDL_Entry::LOSE;

		const WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtm = static_cast<uint16_t>(1u + cd);

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal)
		return ctx.in_check
			? DTM50_Result{ WDL_Entry::LOSE, 0 }
			: DTM50_Result{ WDL_Entry::DRAW, 0 };
	if (!have_candidate)
		return {};

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return { best_wdl, best_dtm };
	return { WDL_Entry::DRAW, 0 };
}

// When `ep_square` is set, the ep-capture children are minimaxed against the
// no-ep result inline. Each such child is ep-free (probed with SQ_END), so the
// overlay bottoms out in one ply.
Probe_Result Probe_Tables::Impl::probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth)
{
	Probe_Result r;
	if (!pos.is_legal())
	{
		r.status = Probe_Result::Status::ILLEGAL_POS;
		return r;
	}

	WDL_File* w = open_wdl(ps);
	const bool rule50_drawn = rule50 != IGNORE_50MR && rule50 >= DTM50_HMC_COUNT;
	// DTC/DTM/DTM50 reads are all gated on `w`, so WDL absent (and not a rule50
	// auto-draw) means there is nothing to return.
	if (!w && !rule50_drawn) return r;

	r.status = Probe_Result::Status::OK;
	if (w)
	{
		r.wdl = probe_wdl_internal(w, ps, pos, depth);
		if (DTC_File* d = open_dtc(ps))
		{
			const auto dtc = probe_dtc_internal(d, ps, pos, r.wdl, depth);
			r.has_dtc = dtc.has_value();
			if (dtc) r.dtc = *dtc;
		}
		if (DTM50_File* m50 = open_dtm50(ps))
		{
			const DTM50_Result d50 = probe_dtm50_internal(m50, ps, pos, r.wdl, IGNORE_50MR, depth);
			r.dtm = d50.dtm;
			r.has_dtm = d50.wdl != WDL_Entry::ILLEGAL;
			if (rule50_drawn)
			{
				const bool mated = r.wdl == WDL_Entry::LOSE && is_checkmate(pos);
				r.dtm50_wdl = mated ? WDL_Entry::LOSE : WDL_Entry::DRAW;
				r.dtm50 = 0;
				r.has_dtm50 = true;
			}
			else if (rule50 != IGNORE_50MR)
			{
				const DTM50_Result d50r = probe_dtm50_internal(m50, ps, pos, r.wdl, rule50, depth);
				r.dtm50_wdl = d50r.wdl;
				r.dtm50 = d50r.dtm;
				r.has_dtm50 = d50r.wdl != WDL_Entry::ILLEGAL;
			}
		}
		else if (DTM_File* m = open_dtm(ps))
		{
			const auto dtm = probe_dtm_internal(m, ps, pos, r.wdl, depth);
			r.has_dtm = dtm.has_value();
			if (dtm) r.dtm = *dtm;
		}
	}

	if (ep_square == SQ_END)
		return r;

	Move_List eps;
	add_ep_moves(pos, ep_square, eps);
	if (eps.empty()) return r;

	Probe_Result best = r;
	WDL_Entry best_dtc_wdl = r.wdl;
	uint16_t  best_dtc     = r.has_dtc ? static_cast<uint16_t>(r.dtc) : 0;
	WDL_Entry best_dtm_wdl = fold_dtm_wdl(r.wdl);
	uint16_t  best_dtm     = r.has_dtm ? static_cast<uint16_t>(r.dtm) : 0;
	WDL_Entry best_dtm50_wdl = r.has_dtm50 ? r.dtm50_wdl : fold_dtm50_wdl(r.wdl);
	uint16_t  best_dtm50     = r.has_dtm50 ? static_cast<uint16_t>(r.dtm50) : 0;

	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!pos.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(pos, eps[i]);
		Probe_Result cr;
		if (child.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtc = best.has_dtc;
			cr.dtc = 0;
			cr.has_dtm = best.has_dtm;
			cr.dtm = 0;
			cr.has_dtm50 = best.has_dtm50;
			cr.dtm50_wdl = WDL_Entry::DRAW;
			cr.dtm50 = 0;
		}
		else
		{
			cr = probe_impl(child.ps, child.pos, 0, SQ_END, depth + 1);  // EP is zeroing
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			continue;

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		if (wdl_rank(my_wdl) > wdl_rank(best.wdl))
			best.wdl = my_wdl;

		if (best.has_dtc && cr.has_dtc)
		{
			if (prefer_new(my_wdl, 1, best_dtc_wdl, best_dtc))
			{
				best_dtc_wdl = my_wdl;
				best_dtc = 1;
				best.dtc = 1;
			}
		}

		if (best.has_dtm && cr.has_dtm)
		{
			const WDL_Entry my_dtm_wdl = fold_dtm_wdl(my_wdl);
			const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cr.dtm));
			if (prefer_new(my_dtm_wdl, my_dtm, best_dtm_wdl, best_dtm))
			{
				best_dtm_wdl = my_dtm_wdl;
				best_dtm = my_dtm;
				best.dtm = (my_dtm_wdl == WDL_Entry::WIN || my_dtm_wdl == WDL_Entry::LOSE)
					? my_dtm
					: 0;
			}
		}

		if (best.has_dtm50 && cr.has_dtm50)
		{
			const WDL_Entry my_dtm50_wdl = invert_wdl(cr.dtm50_wdl);
			const uint16_t my_dtm50 = static_cast<uint16_t>(1u + static_cast<uint16_t>(cr.dtm50));
			if (prefer_new(my_dtm50_wdl, my_dtm50, best_dtm50_wdl, best_dtm50))
			{
				best_dtm50_wdl = my_dtm50_wdl;
				best_dtm50 = my_dtm50;
				best.dtm50_wdl = my_dtm50_wdl;
				best.dtm50 = (my_dtm50_wdl == WDL_Entry::WIN || my_dtm50_wdl == WDL_Entry::LOSE)
					? my_dtm50
					: 0;
			}
		}
	}
	return best;
}

// WDL-only counterpart of probe_impl's en-passant overlay. Each ep-capture child
// is ep-free, so it goes through probe_wdl_internal, not back through this.
WDL_Entry Probe_Tables::Impl::probe_wdl_impl(const Piece_Config& ps, const Position& pos, Square ep_square, int depth)
{
	if (!pos.is_legal())
		return WDL_Entry::ILLEGAL;

	WDL_Entry best = probe_wdl_internal(open_wdl(ps), ps, pos, depth);
	if (best == WDL_Entry::ILLEGAL || ep_square == SQ_END)
		return best;

	Move_List eps;
	add_ep_moves(pos, ep_square, eps);
	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!pos.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(pos, eps[i]);
		const WDL_Entry cw = child.is_kk
			? WDL_Entry::DRAW
			: probe_wdl_internal(open_wdl(child.ps), child.ps, child.pos, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) continue;

		const WDL_Entry mine = invert_wdl(cw);
		if (wdl_rank(mine) > wdl_rank(best))
			best = mine;
	}
	return best;
}

namespace {

Canonical_Root canonical_root_from_position(const Position& input, Square ep_square)
{
	auto [ps, literal_key] = piece_config_and_literal_key_from_position(input);
	Position pos = input;
	if (literal_key != ps.base_material_key())
	{
		pos = mirror_for_canonical(input);
		if (ep_square != SQ_END)
			ep_square = sq_rank_mirror(ep_square);
		return { std::move(ps), std::move(pos), ep_square, true };
	}
	return { std::move(ps), std::move(pos), ep_square, false };
}

// Material key of `pos` for comparison against a (possibly pair-bearing)
// Piece_Config. When `ps` carries a frozen pair, the canonical opposing pair's
// two pawns are excluded from the key and the pair flag is set instead -- the
// board shows them as ordinary pawns. Returns false when `ps` expects a pair but
// `pos` has no opposing pair (the position is outside the pair table's domain).
bool pair_aware_literal_key(const Piece_Config& ps, const Position& pos,
                            Material_Key& out)
{
	Square pw = SQ_END, pb = SQ_END;
	if (ps.has_frozen_pair())
	{
		Square ws[16], bs[16];
		size_t nw = 0, nb = 0;
		Bitboard wb = pos.piece_bb(WHITE_PAWN);
		while (wb) ws[nw++] = wb.pop_first_square();
		Bitboard bb = pos.piece_bb(BLACK_PAWN);
		while (bb) bs[nb++] = bb.pop_first_square();
		if (!Pair_Group::find_canonical(Const_Span<Square>(ws, nw),
		                                Const_Span<Square>(bs, nb), pw, pb))
			return false;
	}

	Material_Key k;
	for (Piece pc : ALL_PIECES)
	{
		Bitboard b = pos.piece_bb(pc);
		while (b)
		{
			const Square s = b.pop_first_square();
			if (s == pw || s == pb) continue;  // pair members are keyed via the flag
			k.add_piece(pc);
		}
	}
	if (ps.has_frozen_pair())
		k.add_pair();
	out = k;
	return true;
}

// Frozen-pair material for `pos`: physical pieces minus the canonical opposing
// pair's two pawns, with the pair flag set. nullopt if there is no opposing pair.
// Used to prefer a 'p' table over the full table at probe time.
std::optional<Piece_Config> pair_config_from_position(const Position& pos)
{
	Square ws[16], bs[16];
	size_t nw = 0, nb = 0;
	Bitboard wb = pos.piece_bb(WHITE_PAWN);
	while (wb) ws[nw++] = wb.pop_first_square();
	Bitboard bb = pos.piece_bb(BLACK_PAWN);
	while (bb) bs[nb++] = bb.pop_first_square();

	Square pw, pb;
	if (!Pair_Group::find_canonical(Const_Span<Square>(ws, nw),
	                                Const_Span<Square>(bs, nb), pw, pb))
		return std::nullopt;

	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	for (Piece pc : ALL_PIECES)
	{
		Bitboard b = pos.piece_bb(pc);
		while (b)
		{
			const Square s = b.pop_first_square();
			if (s == pw || s == pb) continue;
			pieces[n++] = pc;
		}
	}
	Piece_Config ps(Const_Span<Piece>(pieces.data(), n));
	ps.mark_frozen_pair();
	return ps;
}

std::optional<Canonical_Root> canonical_root_from_config(
	const Piece_Config& ps, const Position& input, Square ep_square)
{
	Material_Key literal_key;
	if (!pair_aware_literal_key(ps, input, literal_key))
		return std::nullopt;
	const auto [base_key, mirror_key] = ps.material_keys();
	if (literal_key == base_key)
		return Canonical_Root{ ps, input, ep_square, false };
	if (literal_key != mirror_key)
		return std::nullopt;

	Position pos = mirror_for_canonical(input);
	if (ep_square != SQ_END)
		ep_square = sq_rank_mirror(ep_square);
	return Canonical_Root{ ps, std::move(pos), ep_square, true };
}

Probe_Result illegal_probe_result()
{
	Probe_Result r;
	r.status = Probe_Result::Status::ILLEGAL_POS;
	return r;
}

// Count material characters before the extension.
size_t count_pieces_from_filename(const std::string& fname)
{
	size_t n = 0;
	for (char c : fname)
	{
		if (c == '.') break;
		++n;
	}
	return n;
}

}  // namespace

void Probe_Tables::Impl::scan_paths()
{
	size_t lg = 0;
	auto scan_dir = [&](const std::filesystem::path& dir, const char* ext) {
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) return;
		const size_t ext_len = std::strlen(ext);
		for (auto& e : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			const std::string n = e.path().filename().string();
			if (n.size() < ext_len) continue;
			if (n.compare(n.size() - ext_len, ext_len, ext) != 0) continue;
			const size_t cnt = count_pieces_from_filename(n);
			if (cnt > lg) lg = cnt;
		}
	};
	for (const auto& d : wdl_dirs) scan_dir(d, WDL_EXT);
	for (const auto& d : dtc_dirs) scan_dir(d, DTC_EXT);
	for (const auto& d : dtm_dirs) scan_dir(d, DTM_EXT);
	for (const auto& d : dtm50_dirs) scan_dir(d, DTM50_EXT);
	largest_pieces.store(lg, std::memory_order_release);
}

Probe_Tables::Probe_Tables() : m_impl(std::make_unique<Impl>()) {}
Probe_Tables::~Probe_Tables() = default;
Probe_Tables::Probe_Tables(Probe_Tables&&) noexcept = default;
Probe_Tables& Probe_Tables::operator=(Probe_Tables&&) noexcept = default;

void Probe_Tables::add_wdl_path(std::filesystem::path dir) { m_impl->wdl_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtc_path(std::filesystem::path dir) { m_impl->dtc_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtm_path(std::filesystem::path dir) { m_impl->dtm_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtm50_path(std::filesystem::path dir) { m_impl->dtm50_dirs.emplace_back(std::move(dir)); }

bool Probe_Tables::init(const std::filesystem::path& dir)
{
	std::error_code ec;
	auto try_sub = [&](const char* sub) -> std::filesystem::path {
		auto p = dir / sub;
		return std::filesystem::is_directory(p, ec) ? p : dir;
	};
	m_impl->wdl_dirs.push_back(try_sub("wdl"));
	m_impl->dtc_dirs.push_back(try_sub("dtc"));
	m_impl->dtm_dirs.push_back(try_sub("dtm"));
	m_impl->dtm50_dirs.push_back(try_sub("dtm50"));
	m_impl->scan_paths();
	return m_impl->largest_pieces.load(std::memory_order_acquire) > 0;
}

size_t Probe_Tables::largest() const
{
	return m_impl->largest_pieces.load(std::memory_order_acquire);
}

void Probe_Tables::rescan() { m_impl->invalidate_tables(); m_impl->scan_paths(); }

// Frozen-pair routing shared by every probe entry point: if `pos` has an opposing
// pawn pair and that 'p' table is on disk, return its canonical root. The 'p'
// table covers a partial domain and a position has the same value in either
// table, so "prefer the pair table when present" is always safe -- even for the
// explicit-ps overloads and the root-move rankers, where the physical material is
// the fallback.
std::optional<Canonical_Root> Probe_Tables::Impl::route_pair(
	const Position& pos, Square ep_square)
{
	if (std::optional<Piece_Config> pair_ps = pair_config_from_position(pos))
		if (std::optional<Canonical_Root> r = canonical_root_from_config(*pair_ps, pos, ep_square))
			if (has_any_table(r->ps))
				return r;
	return std::nullopt;
}

Child_Pos Probe_Tables::Impl::make_child(const Position& parent, Move m)
{
	const bool zeroing = move_is_zeroing(parent, m);
	Position pos = parent;
	(void)pos.do_move(m);
	const Square raw_ep = move_is_pawn_double_push(parent, m)
		? ep_square_after_double_push(m) : SQ_END;

	// Prefer the child's frozen-pair table when one is on disk. A move that keeps
	// the pair (any non-capture, including a free-pawn push) stays in a 'p'
	// material, which the board-derived config below would miss -- it sees the
	// pair pawns as ordinary free pawns. route_pair re-indexes into the pair table
	// and mirrors ep to match; it returns nullopt for captures/promotions (which
	// cross into a non-pair sub-table) and when no 'p' table is on disk, so we
	// then fall back to the board's full physical material.
	if (std::optional<Canonical_Root> r = route_pair(pos, raw_ep))
	{
		const bool is_kk = (r->ps.num_pieces() <= 2);
		return Child_Pos{ std::move(r->pos), std::move(r->ps), r->ep_square, is_kk, zeroing };
	}

	auto [cps, lit] = piece_config_and_literal_key_from_position(pos);
	Square ep = raw_ep;
	if (lit != cps.base_material_key())
	{
		pos = mirror_for_canonical(pos);
		if (ep != SQ_END) ep = sq_rank_mirror(ep);
	}
	const bool is_kk = (cps.num_pieces() <= 2);
	return Child_Pos{ std::move(pos), std::move(cps), ep, is_kk, zeroing };
}

Probe_Result Probe_Tables::probe(const Position& pos, unsigned rule50)
{
	return probe(pos, SQ_END, rule50);
}

Probe_Result Probe_Tables::probe(const Position& pos, Square ep_square, unsigned rule50)
{
	const std::optional<Canonical_Root> paired = m_impl->route_pair(pos, ep_square);
	const Canonical_Root root = paired ? *paired : canonical_root_from_position(pos, ep_square);
	return m_impl->probe_impl(root.ps, root.pos, rule50, root.ep_square, 0);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, unsigned rule50)
{
	return probe(ps, pos, SQ_END, rule50);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	if (const std::optional<Canonical_Root> paired = m_impl->route_pair(pos, ep_square))
		return m_impl->probe_impl(paired->ps, paired->pos, rule50, paired->ep_square, 0);
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, ep_square);
	if (!root) return illegal_probe_result();
	return m_impl->probe_impl(root->ps, root->pos, rule50, root->ep_square, 0);
}

WDL_Entry Probe_Tables::probe_wdl(const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	const std::optional<Canonical_Root> paired = m_impl->route_pair(pos, ep_square);
	const Canonical_Root root = paired ? *paired : canonical_root_from_position(pos, ep_square);
	return m_impl->probe_wdl_impl(root.ps, root.pos, root.ep_square, 0);
}

WDL_Entry Probe_Tables::probe_wdl(
	const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	if (const std::optional<Canonical_Root> paired = m_impl->route_pair(pos, ep_square))
		return m_impl->probe_wdl_impl(paired->ps, paired->pos, paired->ep_square, 0);
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, ep_square);
	if (!root) return WDL_Entry::ILLEGAL;
	return m_impl->probe_wdl_impl(root->ps, root->pos, root->ep_square, 0);
}

namespace {

Move rank_mirror_move(Move m)
{
	if (m.is_ep_capture())
		return Move::make_ep_capture(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
	if (m.is_promotion())
		return Move::make_promotion(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()), m.promotion());
	return Move::make_quiet(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
}

// Positive values are wins for the side to move; negative values are losses.
int signed_dtz_of(const Probe_Result& r)
{
	if (!r.has_dtc) return 0;
	const int v = static_cast<int>(r.dtc);
	switch (r.wdl)
	{
		case WDL_Entry::WIN:          return  v;
		case WDL_Entry::CURSED_WIN:   return  v;
		case WDL_Entry::BLESSED_LOSS: return -v;
		case WDL_Entry::LOSE:         return -v;
		case WDL_Entry::DRAW:
		case WDL_Entry::ILLEGAL:
		default:                      return 0;
	}
}

// Fathom WdlToDtz mapping for zeroing moves.
int zeroing_signed_dtz(WDL_Entry my_wdl)
{
	switch (my_wdl)
	{
		case WDL_Entry::WIN:          return    1;
		case WDL_Entry::CURSED_WIN:   return  101;
		case WDL_Entry::DRAW:         return    0;
		case WDL_Entry::BLESSED_LOSS: return -101;
		case WDL_Entry::LOSE:         return   -1;
		case WDL_Entry::ILLEGAL:
		default:                      return    0;
	}
}

int fathom_dtz_rank(int v, unsigned cnt50, bool has_repeated)
{
	if (v > 0) return (static_cast<int>(v + cnt50) <= 99 && !has_repeated)
		? 1000 : 1000 - static_cast<int>(v + cnt50);
	if (v < 0) return (-v * 2 + static_cast<int>(cnt50) < 100)
		? -1000 : -1000 + static_cast<int>(-v + cnt50);
	return 0;
}

constexpr int TB_VALUE_PAWN    = 100;
constexpr int TB_VALUE_DRAW    =   0;
constexpr int TB_VALUE_MATE    = 32000;
constexpr int TB_MAX_MATE_PLY  = 255;

int fathom_dtz_score(int rank, int bound)
{
	if (rank >=  bound) return  TB_VALUE_MATE - TB_MAX_MATE_PLY - 1;
	if (rank >       0) return std::max( 3, rank - 800) * TB_VALUE_PAWN / 200;
	if (rank ==      0) return TB_VALUE_DRAW;
	if (rank >  -bound) return std::min(-3, rank + 800) * TB_VALUE_PAWN / 200;
	return -TB_VALUE_MATE + TB_MAX_MATE_PLY + 1;
}

}  // namespace

std::vector<Root_Move> Probe_Tables::probe_root_dtz(
	const Position& pos, Square ep_square,
	unsigned rule50, bool use_rule50, bool has_repeated)
{
	std::vector<Root_Move> out;
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Position& probe_pos = root.pos;

	Move_List ml;
	probe_pos.gen_pseudo_legal_moves(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);
	const Position::Legality ctx = probe_pos.legality_context();

	const int bound = use_rule50 ? 900 : 1;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m, ctx)) continue;

		Child_Pos c = m_impl->make_child(probe_pos, m);
		Probe_Result cr;
		if (c.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtc = true;
			cr.dtc = 0;
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, rule50, c.ep, 0);
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			return {};

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		int v;
		if (c.is_zeroing)
		{
			v = zeroing_signed_dtz(my_wdl);
		}
		else
		{
			if (!cr.has_dtc) return {};
			v = -signed_dtz_of(cr);
			if (v > 0) ++v;
			else if (v < 0) --v;
		}
		// Fathom reports mate-in-1 as 1, not the child-derived 2.
		if (v == 2 && c.pos.is_in_check())
		{
			Move_List cml;
			c.pos.gen_pseudo_legal_moves(out_param(cml));
			const Position::Legality cctx = c.pos.legality_context();
			bool any = false;
			for (size_t j = 0; j < cml.size(); ++j)
			{
				if (c.pos.is_pseudo_legal_move_legal(cml[j], cctx))
				{
					any = true;
					break;
				}
			}
			if (!any) v = 1;
		}

		const int rank = fathom_dtz_rank(v, rule50, has_repeated);
		const int score = fathom_dtz_score(rank, bound);
		out.push_back(Root_Move{root.mirrored ? rank_mirror_move(m) : m, my_wdl, v, rank, score});
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}

std::vector<Root_Move> Probe_Tables::probe_root_wdl(
	const Position& pos, Square ep_square, bool use_rule50)
{
	// Fathom WdlToRank/WdlToValue, indexed by v + 2 from side-to-move POV.
	static constexpr int WdlToRank[]  = { -1000, -899, 0, 899, 1000 };
	static constexpr int WdlToValue[] = {
		-TB_VALUE_MATE + TB_MAX_MATE_PLY + 1,
		TB_VALUE_DRAW - 2,
		TB_VALUE_DRAW,
		TB_VALUE_DRAW + 2,
		 TB_VALUE_MATE - TB_MAX_MATE_PLY - 1
	};

	auto wdl_to_v = [](WDL_Entry w) -> int {
		switch (w) {
			case WDL_Entry::WIN:          return  2;
			case WDL_Entry::CURSED_WIN:   return  1;
			case WDL_Entry::DRAW:         return  0;
			case WDL_Entry::BLESSED_LOSS: return -1;
			case WDL_Entry::LOSE:         return -2;
			default:                      return  0;
		}
	};

	std::vector<Root_Move> out;
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Position& probe_pos = root.pos;

	Move_List ml;
	probe_pos.gen_pseudo_legal_moves(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);
	const Position::Legality ctx = probe_pos.legality_context();

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m, ctx)) continue;

		Child_Pos c = m_impl->make_child(probe_pos, m);
		// Root WDL ranking only needs the child's WDL, so probe just that layer.
		const WDL_Entry cw = c.is_kk
			? WDL_Entry::DRAW
			: m_impl->probe_wdl_impl(c.ps, c.pos, c.ep, 0);
		if (cw == WDL_Entry::ILLEGAL)
			return {};

		const WDL_Entry my_wdl = invert_wdl(cw);
		int v = wdl_to_v(my_wdl);
		if (!use_rule50) v = v > 0 ? 2 : v < 0 ? -2 : 0;

		Root_Move r{root.mirrored ? rank_mirror_move(m) : m, my_wdl, 0, WdlToRank[v + 2], WdlToValue[v + 2]};
		out.push_back(r);
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}

std::vector<Root_Move> Probe_Tables::probe_root_dtm(
	const Position& pos, Square ep_square, unsigned rule50, bool use_rule50)
{
	std::vector<Root_Move> out;
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Position& probe_pos = root.pos;

	Move_List ml;
	probe_pos.gen_pseudo_legal_moves(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);
	const Position::Legality ctx = probe_pos.legality_context();

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m, ctx)) continue;

		Child_Pos c = m_impl->make_child(probe_pos, m);
		// Honoring 50MR, advance the clock into the child as probe_impl does (a
		// zeroing move opens a fresh window). Ignoring it, probe flat: probe_impl
		// then fills the flat dtm/wdl and skips the rule-true layer entirely.
		const unsigned child_rule50 = use_rule50 ? (c.is_zeroing ? 0u : rule50 + 1u)
		                                         : IGNORE_50MR;

		Probe_Result cr;
		if (c.is_kk)
		{
			cr.status    = Probe_Result::Status::OK;
			cr.wdl       = WDL_Entry::DRAW;
			cr.has_dtm   = true;
			cr.dtm       = 0;
			cr.has_dtm50 = true;
			cr.dtm50_wdl = WDL_Entry::DRAW;
			cr.dtm50     = 0;
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, child_rule50, c.ep, 0);
		}
		// Flat mate distance must resolve for every child or the ranking is
		// unsound; the rule-true layer is additionally required to honor 50MR.
		if (cr.status != Probe_Result::Status::OK || !cr.has_dtm
		    || cr.wdl == WDL_Entry::ILLEGAL)
			return {};
		if (use_rule50 && (!cr.has_dtm50 || cr.dtm50_wdl == WDL_Entry::ILLEGAL))
			return {};

		// From the root side-to-move's POV. flat_* ignore 50MR (5-class, exact
		// mate plies); rule_* respect it (3-class, clamped at the window).
		const WDL_Entry flat_wdl = invert_wdl(cr.wdl);
		const int       flat_d   = static_cast<int>(cr.dtm) + 1;  // +1 for my move

		WDL_Entry report_wdl = WDL_Entry::DRAW;
		int v = 0, rank = 0, score = 0;

		if (!use_rule50)
		{
			// 50MR ignored: cursed/blessed are real wins/losses. Rank by flat DTM.
			if (flat_wdl == WDL_Entry::WIN || flat_wdl == WDL_Entry::CURSED_WIN)
			{
				report_wdl = WDL_Entry::WIN;
				v = flat_d;
				score = rank = TB_VALUE_MATE - v;       // shorter mate -> higher rank
			}
			else if (flat_wdl == WDL_Entry::LOSE || flat_wdl == WDL_Entry::BLESSED_LOSS)
			{
				report_wdl = WDL_Entry::LOSE;
				v = -flat_d;
				score = rank = -TB_VALUE_MATE + flat_d; // slower loss -> higher rank
			}
			// DRAW: 0/0/0.
		}
		else
		{
			// Band by the rule-true verdict; order clean bands by the
			// 50MR-respecting mate distance (the actual path to glory).
			const WDL_Entry rule_wdl = invert_wdl(cr.dtm50_wdl);
			if (rule_wdl == WDL_Entry::WIN)
			{
				report_wdl = WDL_Entry::WIN;
				v = static_cast<int>(cr.dtm50) + 1;
				score = rank = TB_VALUE_MATE - v;
			}
			else if (rule_wdl == WDL_Entry::LOSE)
			{
				report_wdl = WDL_Entry::LOSE;
				const int d = static_cast<int>(cr.dtm50) + 1;
				v = -d;
				score = rank = -TB_VALUE_MATE + d;
			}
			else if (flat_wdl == WDL_Entry::WIN || flat_wdl == WDL_Entry::CURSED_WIN)
			{
				// Forced mate that 50MR draws (cursed-by-table or clock-expired):
				// a winning try, nerfed strictly between draw and any clean mate;
				// shorter flat mate is the better try. v reports the DTM distance.
				report_wdl = WDL_Entry::CURSED_WIN;
				v = flat_d;
				rank  = std::max(1, 899 - flat_d);
				score = TB_VALUE_DRAW + 2;
			}
			else if (flat_wdl == WDL_Entry::LOSE || flat_wdl == WDL_Entry::BLESSED_LOSS)
			{
				// Forced loss that 50MR saves: symmetric losing band; a slower
				// flat mate resists longer and ranks nearer the draw.
				report_wdl = WDL_Entry::BLESSED_LOSS;
				v = -flat_d;
				rank  = std::min(-1, flat_d - 899);
				score = TB_VALUE_DRAW - 2;
			}
			// else: genuine draw, 0/0/0.
		}

		out.push_back(Root_Move{root.mirrored ? rank_mirror_move(m) : m, report_wdl, v, rank, score});
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}
