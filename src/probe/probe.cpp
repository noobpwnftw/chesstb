#include "probe/probe.h"
#include "probe/entry.h"
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
// full miss and outside the lock, returning the owning pointer to insert —
// which may be null (e.g. no table on disk). Concurrent builders race on
// try_emplace; the loser's freshly built object is discarded.
//
// Returns a raw pointer: this is the probe hot path, called once per child, so
// it must not copy a shared_ptr. The thread-local slot holds the owning handle,
// which is what keeps the table alive if the map's reference is dropped.
template <typename Map, typename Make>
auto cached_open(const std::atomic<uint64_t>& epoch_src, std::mutex& mu, Map& cache,
                 typename Map::key_type k, Make&& make)
	-> typename Map::mapped_type::element_type*
{
	using Handle = typename Map::mapped_type;
	using T = typename Handle::element_type;
	thread_local TL_Cache<T, typename Map::key_type, Handle> tl;

	for (;;)
	{
		const uint64_t epoch = epoch_src.load(std::memory_order_acquire);

		if (const Handle* hit = tl.find(epoch, k)) return hit->get();
		{
			std::lock_guard<std::mutex> lk(mu);
			auto it = cache.find(k);
			if (it != cache.end())
			{
				tl.insert(epoch, k, it->second);
				return it->second.get();
			}
		}

		Handle built = make();

		std::lock_guard<std::mutex> lk(mu);
		// `make` ran unlocked; invalidate_tables() bumps the epoch under `mu`, so
		// a build that straddled it is discarded and retried against the new
		// epoch rather than resurrected into the cleared map.
		if (epoch_src.load(std::memory_order_relaxed) != epoch)
			continue;

		auto [it, inserted] = cache.try_emplace(k, std::move(built));
		tl.insert(epoch, k, it->second);
		return it->second.get();
	}
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

// 5-class WDL → the three a clocked metric holds, DTM50's layers and DTC's
// budgets alike: cursed and blessed are unreachable under 50MR.
NODISCARD WDL_Entry fold_50mr_wdl(WDL_Entry w)
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

NODISCARD bool prefer_new(WDL_Entry new_wdl, uint16_t new_dtz,
                          WDL_Entry old_wdl, uint16_t old_dtz)
{
	const int rn = wdl_rank(new_wdl), ro = wdl_rank(old_wdl);
	if (rn != ro) return rn > ro;
	const bool win_side = (new_wdl == WDL_Entry::WIN || new_wdl == WDL_Entry::CURSED_WIN);
	const bool loss_side = (new_wdl == WDL_Entry::LOSE || new_wdl == WDL_Entry::BLESSED_LOSS);
	if (win_side)  return new_dtz < old_dtz;
	if (loss_side) return new_dtz > old_dtz;
	return false;
}

// Derivation stands in for `read(stm, pos, wdl)`. Reads are class-driven: the
// WDL companion pins the class, while the file only prices the distance. A child
// that cannot reach the pinned class never wins the minimax and never needs a
// probe. ILLEGAL pins nothing -- the WDL table had no answer either.
NODISCARD bool below_pinned_class(WDL_Entry pinned, WDL_Entry offer)
{
	return pinned != WDL_Entry::ILLEGAL && wdl_rank(offer) < wdl_rank(pinned);
}

// Same, on DTC's key: pushes owed lead, and the wait only breaks a tie in them.
NODISCARD bool prefer_new_dtc(WDL_Entry new_wdl, uint16_t new_order, uint16_t new_value,
                              WDL_Entry old_wdl, uint16_t old_order, uint16_t old_value)
{
	const int nr = wdl_rank(new_wdl), orank = wdl_rank(old_wdl);
	if (nr != orank) return nr > orank;
	const bool win_side  = new_wdl == WDL_Entry::WIN || new_wdl == WDL_Entry::CURSED_WIN;
	const bool loss_side = new_wdl == WDL_Entry::LOSE || new_wdl == WDL_Entry::BLESSED_LOSS;
	if (win_side)  return (new_order != old_order) ? new_order < old_order : new_value < old_value;
	if (loss_side) return (new_order != old_order) ? new_order > old_order : new_value > old_value;
	return false;
}

// A DTZ minimax lifts a LOSE to BLESSED_LOSS past 100, the higher rank of the two.
NODISCARD WDL_Entry dtz_lift(WDL_Entry my_wdl)
{
	return my_wdl == WDL_Entry::LOSE ? WDL_Entry::BLESSED_LOSS : my_wdl;
}

NODISCARD bool is_symmetric_material(const Piece_Config& ps)
{
	const auto [mat_key, mir_key] = ps.material_keys();
	return mat_key == mir_key;
}

// Win classes are the ones a loss-only frame leaves out; CURSED_WIN counts,
// since a distance cell is stored under the win flag either way.
NODISCARD bool is_win_class(WDL_Entry w)
{
	return w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN;
}

// Which frame holds a cell, and whether it is there to be read. A dropped frame
// is reachable through the mirror when the material is its own mirror; a
// loss-only frame holds no win. Both gaps are filled by the same one-ply derive,
// which the caller runs on the unmirrored position.
struct Frame_Read
{
	Color frame = WHITE;
	bool mirrored = false;
	bool readable = false;
};

template <typename File>
NODISCARD Frame_Read locate_frame(const File& f, const Piece_Config& ps,
                                  const Position& pos, WDL_Entry wdl)
{
	const Color stm = pos.turn();
	const auto priced = [&](Color frame) { return !(f.is_loss_only[frame] && is_win_class(wdl)); };

	if (!f.is_dropped[stm]) return { stm, false, priced(stm) };
	if (!is_symmetric_material(ps)) return { stm, false, false };
	const Color kept = color_opp(stm);
	return { kept, true, priced(kept) };
}

// Two bounds apply to the layer at `rule50`, one for each distance. Both apply
// only to W/L positions, since cursed/blessed positions are DRAW at every layer
// and no distance is assigned to them.
//
// Pinned: rule50 + dtm <= 100 means the entire mating line fits within the
// window, so the layer uses the flat DTM. The bound is inclusive because the
// game ends at checkmate, before a draw can be claimed.
//
// Busted: rule50 + dtz > 100 means no surviving line lets the winner reset the
// count in time, so the layer is DRAW. This one-directional bound assigns no
// distance to positions that survive. It uses the same inequality encoded by a
// cell's transition to DRAW (h = 102 - dtz), based on the DTZ returned by the
// flat probe.
//
// An unpriced distance reads as 0, so the pin needs a has_dtm at the call site
// while the bust cannot fire on one. Which case left DTZ unread does not matter
// there: the cursed class a read declines to price, or a derive whose minimax
// went unpinned.
NODISCARD bool dtm50_layer_pinned_by_dtm(WDL_Entry wdl, uint16_t dtm, unsigned rule50)
{
	if (wdl != WDL_Entry::WIN && wdl != WDL_Entry::LOSE) return false;
	return rule50 + static_cast<unsigned>(dtm) <= DTZ_MAX_NON_CURSED;
}

NODISCARD bool layer_busted_by_dtz(WDL_Entry wdl, uint16_t dtz, unsigned rule50)
{
	if (wdl != WDL_Entry::WIN && wdl != WDL_Entry::LOSE) return false;
	return rule50 + static_cast<unsigned>(dtz) > DTZ_MAX_NON_CURSED;
}

struct Child_Pos { Position pos; Piece_Config ps; Square ep; bool is_zeroing; };

// A LOSE pin makes WIN a safe surrogate for every child's relevant class. For
// an exact five-class pin, every child is a clean WIN for the opponent; a cursed
// child would have made the parent BLESSED_LOSS. A folded DTM pin may itself
// represent BLESSED_LOSS, but WIN and CURSED_WIN are equivalent to that minimax.
// Either way, the class read is pure overhead: at the bottom rank,
// below_pinned_class cannot prune, and a missing distance table still surfaces
// as a failed distance read.
NODISCARD bool child_class_is_forced(WDL_Entry pinned)
{
	return pinned == WDL_Entry::LOSE;
}

struct DTM50_Result
{
	WDL_Entry wdl = WDL_Entry::ILLEGAL;
	uint16_t dtm = 0;
	bool has_dtz = false;
	uint16_t dtz = 0;
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

NODISCARD bool has_legal_move(const Position& pos, Square ep_square = SQ_END)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	add_ep_moves(pos, ep_square, ml);
	const Position::Legality ctx = pos.legality_context();
	for (size_t i = 0; i < ml.size(); ++i)
		if (pos.is_pseudo_legal_move_legal(ml[i], ctx)) return true;
	return false;
}

NODISCARD bool is_checkmate(const Position& pos, Square ep_square = SQ_END)
{
	return pos.is_in_check(pos.turn()) && !has_legal_move(pos, ep_square);
}

// The DRAW flip pins DTZ: a W/L cell still decisive at hmc = 100 - dtz turns one
// tick later, so its flip layer (hmc + 1) is h = 102 - dtz. h == 1 is a cell
// already drawn at a fresh clock -- the cursed band, whose distance the flip
// says nothing about; never flipping means dtz <= 1, which only a mate splits.
// DRAW/ILLEGAL answer 0, the don't-care the DTZ table decodes.
NODISCARD std::optional<uint16_t> dtz_from_draw_flip(
	uint16_t h, WDL_Entry wdl, const Position& pos)
{
	if (wdl == WDL_Entry::DRAW || wdl == WDL_Entry::ILLEGAL) return uint16_t{0};
	if (wdl != WDL_Entry::WIN && wdl != WDL_Entry::LOSE) return std::nullopt;
	if (h == 0)
		return (wdl == WDL_Entry::LOSE && is_checkmate(pos)) ? uint16_t{0} : uint16_t{1};
	if (h == 1) return std::nullopt;
	return static_cast<uint16_t>(DTZ_MAX_NON_CURSED + 2u - h);
}

// Children a derive could not price. A skip only unpins the minimax if the class
// it could have offered outranks the kept best; an unknown class bounds nothing
// and unpins outright. Every deriver runs this -- a best over a partial move set
// is a wrong answer, not a partial one.
struct Skipped_Children
{
	void of_class(WDL_Entry my_wdl) { best_rank = std::max(best_rank, wdl_rank(my_wdl)); }
	// Same, for a DTZ minimax: the child's unknown distance may push past 100,
	// which lifts a LOSE to BLESSED_LOSS -- a higher rank, so bound by that.
	void of_dtz_class(WDL_Entry my_wdl) { of_class(dtz_lift(my_wdl)); }
	void unknown() { blind = true; }
	NODISCARD bool unpin(WDL_Entry best_wdl) const
	{
		return blind || best_rank >= wdl_rank(best_wdl);
	}

private:
	int best_rank = -1;
	bool blind = false;
};

// The DTZ half of a derive: zeroing distance ranks moves its own way, so it
// minimaxes beside the mate distance over the same children. Fed per child;
// `finish` writes the field a read cell gets from its flip.
struct DTZ_Minimax
{
	// A zeroing move ends the count at this ply, whatever the child holds.
	void zeroing_child(WDL_Entry child_wdl) { offer(child_wdl, 1); }

	void quiet_child(WDL_Entry child_wdl, const DTM50_Result& child)
	{
		if (child.has_dtz)
			offer(child_wdl, static_cast<uint16_t>(1u + child.dtz));
		else
			skipped.of_dtz_class(invert_wdl(child_wdl));
	}

	// Priced against the clock instead of visited: nothing to take from it.
	void unwalked() { skipped.unknown(); }

	void finish(DTM50_Result& out, bool any_legal) const
	{
		if (!any_legal)  // mate or stalemate: terminal, zeroing distance 0
		{
			out.has_dtz = true;
			return;
		}
		if (!have || skipped.unpin(best_wdl)) return;
		out.has_dtz = true;
		out.dtz = (best_wdl == WDL_Entry::DRAW) ? 0 : best_dtz;
	}

private:
	void offer(WDL_Entry child_wdl, uint16_t dtz)
	{
		WDL_Entry my_wdl = invert_wdl(child_wdl);
		if (dtz > DTZ_MAX_NON_CURSED)
		{
			if (my_wdl == WDL_Entry::WIN)  my_wdl = WDL_Entry::CURSED_WIN;
			if (my_wdl == WDL_Entry::LOSE) my_wdl = WDL_Entry::BLESSED_LOSS;
		}
		if (!have || prefer_new(my_wdl, dtz, best_wdl, best_dtz))
		{
			best_wdl = my_wdl;
			best_dtz = dtz;
			have = true;
		}
	}

	bool have = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtz = 0;
	Skipped_Children skipped;
};

// DTC's fold for the unbounded row it carries: a zeroing move ends the count one
// ply out, a quiet move waits one longer, and the class says whether the shortest
// or the longest of them stands. One child unable to say leaves the row unanswered,
// since a best over part of the moves is a wrong answer rather than a partial one.
struct Unbounded_Fold
{
	explicit Unbounded_Fold(WDL_Entry mover_class)
		: winning(mover_class == WDL_Entry::WIN || mover_class == WDL_Entry::CURSED_WIN) {}

	void zeroing_child() { offer(1); }
	void quiet_child(uint16_t child_dtz)
	{
		if (child_dtz == DTC_Cell::DRAWN) blind = true;
		else offer(static_cast<uint16_t>(child_dtz + 1));
	}

	NODISCARD uint16_t value() const { return (have && !blind) ? best : DTC_Cell::DRAWN; }

private:
	void offer(uint16_t v)
	{
		if (!have || (winning ? v < best : v > best)) best = v;
		have = true;
	}

	bool winning;
	bool have = false;
	bool blind = false;
	uint16_t best = 0;
};

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

	std::shared_ptr<Block_Pool> blocks = std::make_shared<Block_Pool>();

	std::vector<std::filesystem::path> wdl_dirs;
	std::vector<std::filesystem::path> dtz_dirs;
	std::vector<std::filesystem::path> dtc_dirs;
	std::vector<std::filesystem::path> dtm_dirs;
	std::vector<std::filesystem::path> dtm50_dirs;

	// Keyed by material key to avoid per-probe string hashing. One lock covers
	// all four maps; the thread-local front in cached_open keeps it off the hit path.
	std::mutex tables_mu;
	std::unordered_map<Material_Key, std::shared_ptr<WDL_File>>   wdl_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTZ_File>>   dtz_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTC_File>>   dtc_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTM_File>>   dtm_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTM50_File>> dtm50_cache;

	std::atomic<size_t> largest_pieces{0};

	// Builds the table for `ps` if a matching file exists in `dirs`, else
	// caches a null entry so the lookup isn't retried on every probe.
	template <typename File>
	NODISCARD File* open_table(
		std::unordered_map<Material_Key, std::shared_ptr<File>>& cache,
		const std::vector<std::filesystem::path>& dirs, const char* ext, const Piece_Config& ps)
	{
		return cached_open(epoch, tables_mu, cache, ps.min_material_key(),
			[&]() -> std::shared_ptr<File> {
				std::filesystem::path path;
				if (!find_in_dirs(ps, ext, dirs, &path)) return nullptr;
				auto f = std::make_shared<File>(blocks);
				f->load(ps, path);
				return f;
			});
	}

	NODISCARD WDL_File*   open_wdl  (const Piece_Config& ps) { return open_table(wdl_cache,   wdl_dirs,   WDL_EXT,   ps); }
	NODISCARD DTZ_File*   open_dtz  (const Piece_Config& ps) { return open_table(dtz_cache,   dtz_dirs,   DTZ_EXT,   ps); }
	NODISCARD DTC_File*   open_dtc  (const Piece_Config& ps) { return open_table(dtc_cache,   dtc_dirs,   DTC_EXT,   ps); }
	NODISCARD DTM_File*   open_dtm  (const Piece_Config& ps) { return open_table(dtm_cache,   dtm_dirs,   DTM_EXT,   ps); }
	NODISCARD DTM50_File* open_dtm50(const Piece_Config& ps) { return open_table(dtm50_cache, dtm50_dirs, DTM50_EXT, ps); }

	// Whether `ps` is probeable on disk, to decide opposing-pair routing: prefer
	// the 'p' table for an opposing-pair position only if present.
	NODISCARD bool has_any_table(const Piece_Config& ps)
	{
		return open_wdl(ps) != nullptr;
	}

	NODISCARD Probe_Result probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth);
	NODISCARD WDL_Entry probe_wdl_impl(const Piece_Config& ps, const Position& pos, Square ep_square, int depth);
	// Opposing-pair routing bound to this Impl's table availability: the 'p'-table
	// root when `pos` has an opposing pair whose table exists, else nullopt. Lets
	// the root-move rankers steer children that keep the pair to the 'p' table.
	NODISCARD std::optional<Canonical_Root> route_pair(const Position& pos, Square ep_square);
	// Apply `m` to `parent` and resolve the child to probe: prefers the child's
	// opposing-pair table when on disk (a move that keeps the pair stays in a 'p'
	// material the board-derived config would miss), else its full material.
	// Owns the child ep so the routed/mirrored board and ep can't desync.
	NODISCARD Child_Pos make_child(const Position& parent, Move m);
	// The *_internal helpers take an already-opened table handle; they never re-open it.
	NODISCARD WDL_Entry probe_wdl_internal(WDL_File* w, const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD WDL_Stored read_wdl_stored(WDL_File* w, const Position& pos, int depth);
	NODISCARD WDL_Entry relax_bound_wdl(const Position& pos, int depth);
	NODISCARD WDL_Entry raise_by_bound(const WDL_File& w, Color frame, WDL_Entry stored,
	                                      const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> probe_dtz_internal(DTZ_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD std::optional<DTC_Cell> probe_dtc_internal(DTC_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD std::optional<uint16_t> probe_dtm_internal(DTM_File* m, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM50_Result probe_dtm50_internal(DTM50_File* m, const Piece_Config& ps, const Position& pos,
	                                            WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD WDL_Entry derive_wdl(const Position& pos, int depth);
	// The distance derives take the class their dropped-frame read would have used.
	NODISCARD std::optional<uint16_t> derive_dtz(const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD std::optional<DTC_Cell> derive_dtc(const Position& pos, WDL_Entry wdl,
	                                            unsigned rule50, int depth);
	// A child's own record knows nothing of ep rights, so a double push answers
	// through the overlay instead. Both mirror what the generator prices into a
	// push with effective_opp_wdl_after_dp.
	NODISCARD std::optional<DTC_Cell> child_dtc_cell(const Child_Pos& c, WDL_Entry cw,
	                                                 unsigned child_rule50, int depth);
	NODISCARD std::optional<bool> ep_conversion_wins(const Child_Pos& c, int depth);
	NODISCARD std::optional<DTC_Cell> derive_dtc_win(const Position& pos, unsigned rule50, int depth);
	NODISCARD std::optional<DTC_Cell> derive_dtc_loss(const Position& pos, unsigned rule50, int depth);
	NODISCARD std::optional<DTC_Cell> derive_dtc_cursed(const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD bool read_dtc_curve(DTC_File* d, const Piece_Config& ps, const Position& pos,
	                              WDL_Entry wdl, DTC_Curve& out);
	NODISCARD std::optional<uint16_t> derive_dtm(const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM50_Result derive_dtm50(const Position& pos, WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD DTM50_Result derive_dtm50_flat(const Position& pos, WDL_Entry wdl, int depth);


	void scan_paths();

	void invalidate_tables()
	{
		// Under the lock, so a cached_open that is mid-build sees the new epoch
		// before it can publish.
		std::lock_guard<std::mutex> lk(tables_mu);
		wdl_cache.clear();
		dtz_cache.clear();
		dtc_cache.clear();
		dtm_cache.clear();
		dtm50_cache.clear();
		epoch.store(next_epoch(), std::memory_order_release);
	}
};

// Captures and promotions only: their children live in a sub-table, so this
// never re-enters the frame it is resolving.
WDL_Entry Probe_Tables::Impl::relax_bound_wdl(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	WDL_Entry best = WDL_Entry::ILLEGAL;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!m.is_promotion() && pos.is_empty(m.to())) continue;
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;

		Child_Pos c = make_child(pos, m);
		const WDL_Entry cw = c.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) continue;

		const WDL_Entry mine = invert_wdl(cw);
		if (wdl_rank(mine) > wdl_rank(best)) best = mine;
	}
	return best;
}

WDL_Entry Probe_Tables::Impl::raise_by_bound(
	const WDL_File& w, Color frame, WDL_Entry stored, const Position& pos, int depth)
{
	if (!w.is_relaxed[frame] || stored == WDL_Entry::ILLEGAL)
		return stored;
	const WDL_Entry bound = relax_bound_wdl(pos, depth);
	return wdl_rank(bound) > wdl_rank(stored) ? bound : stored;
}

// Raw on-disk code, markers intact. derive_wdl reads quiet children this way —
// they always land in the kept opposite-stm frame of the same material.
WDL_Stored Probe_Tables::Impl::read_wdl_stored(WDL_File* w, const Position& pos, int depth)
{
	if (!w) return WDL_Stored::ILLEGAL;

	const Color stm = pos.turn();
	const WDL_Stored s = w->read(stm, pos);
	if (!w->is_relaxed[stm] || s == WDL_Stored::ILLEGAL)
		return s;
	if (s == WDL_Stored::BOUNDARY_WIN || s == WDL_Stored::BOUNDARY_LOSS)
		return s;

	const WDL_Entry stored = static_cast<WDL_Entry>(s);
	const WDL_Entry bound = relax_bound_wdl(pos, depth);
	return wdl_rank(bound) > wdl_rank(stored) ? static_cast<WDL_Stored>(bound) : s;
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
		const Position mp = pos.mirror();
		return raise_by_bound(*w, mp.turn(), wdl_from_storage(w->read(mp.turn(), mp)),
		                         pos, depth);
	}
	return raise_by_bound(*w, stm, wdl_from_storage(w->read(stm, pos)), pos, depth);
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtz_internal(
	DTZ_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	if (!d) return std::nullopt;

	const Frame_Read fr = locate_frame(*d, ps, pos, wdl);
	if (!fr.readable) return derive_dtz(pos, wdl, depth);
	if (!fr.mirrored) return d->read(fr.frame, pos, wdl);
	const Position mp = pos.mirror();
	return d->read(fr.frame, mp, wdl);
}

std::optional<DTC_Cell> Probe_Tables::Impl::probe_dtc_internal(
	DTC_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	if (!d) return std::nullopt;

	const Frame_Read fr = locate_frame(*d, ps, pos, wdl);
	if (!fr.readable) return derive_dtc(pos, wdl, rule50, depth);
	if (!fr.mirrored) return d->read(fr.frame, pos, wdl, rule50);
	const Position mp = pos.mirror();
	return d->read(fr.frame, mp, wdl, rule50);
}

// The whole record of a position this table does hold, for a derive to minimax
// over. The child keeps the physical material, so the pack its own config names
// answers -- this file, or the 'p' table that re-indexes an opposing pair.
bool Probe_Tables::Impl::read_dtc_curve(DTC_File* d, const Piece_Config& ps,
                                        const Position& pos, WDL_Entry wdl, DTC_Curve& out)
{
	if (!d) return false;
	const Frame_Read fr = locate_frame(*d, ps, pos, wdl);
	if (!fr.readable) return false;
	if (!fr.mirrored)
	{
		d->read_curve(fr.frame, pos, wdl, out_param(out));
		return true;
	}
	const Position mp = pos.mirror();
	d->read_curve(fr.frame, mp, wdl, out_param(out));
	return true;
}

// The child's DTC answer with its ep rights folded in. A double push leaves the
// opponent a capture the child's own record cannot express, and the overlay is
// where that is priced, so a child carrying ep rights answers through it.
std::optional<DTC_Cell> Probe_Tables::Impl::child_dtc_cell(
	const Child_Pos& c, WDL_Entry cw, unsigned child_rule50, int depth)
{
	if (c.ep == SQ_END)
		return probe_dtc_internal(open_dtc(c.ps), c.ps, c.pos, cw, child_rule50, depth);

	const Probe_Result cr = probe_impl(c.ps, c.pos, child_rule50, c.ep, depth);
	if (!cr.has_dtc || !cr.has_dtz) return std::nullopt;
	DTC_Cell cell;
	cell.dtz = static_cast<uint16_t>(cr.dtz);
	if (cr.dtc_wdl != WDL_Entry::DRAW)
	{
		cell.order = static_cast<uint16_t>(cr.dtc_order);
		cell.value = static_cast<uint16_t>(cr.dtc);
	}
	return cell;
}

// Whether an ep capture out of `c` wins for the side holding it. A capture is a
// conversion, so it owes no push and lands one ply out: the cheapest answer DTC
// has, which every budget affords. Nothing answers for a capture whose
// sub-table is absent, and the child's whole value turns on it, so that is a
// third answer rather than a no -- though a capture that does win settles it
// whatever the other one says. `depth` is the child's, so a grandchild is one
// deeper, as under the overlay.
std::optional<bool> Probe_Tables::Impl::ep_conversion_wins(const Child_Pos& c, int depth)
{
	Move_List eps;
	add_ep_moves(c.pos, c.ep, eps);
	bool unknown = false;
	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!c.pos.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos gc = make_child(c.pos, eps[i]);
		const WDL_Entry gw = gc.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: probe_wdl_impl(gc.ps, gc.pos, gc.ep, depth + 1);
		if (gw == WDL_Entry::ILLEGAL) { unknown = true; continue; }
		if (fold_50mr_wdl(invert_wdl(gw)) == WDL_Entry::WIN) return true;
	}
	if (unknown) return std::nullopt;
	return false;
}

// DTC by one-ply minimax, for a frame the file does not hold. It never leaves the
// material: a push and a quiet move both keep it, so they answer from the pack
// their own config names -- this file, or the 'p' table an opposing pair
// re-indexes into -- and a conversion is terminal at value 1 under its own WDL, as
// inside a layer's retro. A winner's push spends one of the budget and reads one
// budget lower; nothing else moves it. The same walk folds the unbounded row the
// pack carries, so this derives everything the table serves.
//
// The class, known before any child is read, says how much of one to read: a win
// takes the cheapest budget any move offers, which is the pair a read returns at
// the clock behind that move, while a loss settles on the budget the most stubborn
// defence forces, which no single pair names.
std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_win(
	const Position& pos, unsigned rule50, int depth)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool have = false;
	uint16_t best_order = 0;
	uint16_t best_value = 0;
	const unsigned budget_plies = dtc_budget_plies(rule50);
	Unbounded_Fold unbounded(WDL_Entry::WIN);

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;

		const bool conversion = m.is_promotion() || m.is_ep_capture()
		                     || pos.piece_at(m.to()) != PIECE_NONE;
		const bool push = !conversion && piece_type(pos.piece_at(m.from())) == PAWN;
		Child_Pos c = make_child(pos, m);
		if (c.ps.is_bare_kings()) continue;

		// A double push hands the opponent an ep capture, which the class has to
		// carry: the generator prices the same reply into the push it evaluates.
		const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) return std::nullopt;
		// Only a clean loss for the other side carries a clean win, and anything
		// else leaves a budget drawn however the rest are priced.
		if (cw != WDL_Entry::LOSE) continue;

		uint16_t order = 0;
		uint16_t value = 1;
		if (conversion)
		{
			unbounded.zeroing_child();
		}
		else
		{
			// A push zeroes the clock and spends one of the budget; a quiet move
			// spends a ply of the clock and none of the budget.
			const unsigned child_rule50 = push ? 0u
				: (rule50 == IGNORE_50MR ? IGNORE_50MR : rule50 + 1u);
			const auto cell = child_dtc_cell(c, cw, child_rule50, depth + 1);
			if (!cell) return std::nullopt;
			// The row is clock-free, so it takes this child whatever the clock made
			// of its budgets.
			if (push) unbounded.zeroing_child();
			else unbounded.quiet_child(cell->dtz);
			if (!cell->priced()) continue;  // that clock has taken this line
			order = static_cast<uint16_t>(cell->order + (push ? 1 : 0));
			value = push ? uint16_t{ 1 } : static_cast<uint16_t>(cell->value + 1);
		}
		// The clock this position holds: a child behind a zeroing move answered
		// against a fresh one.
		if (value > budget_plies) continue;

		if (!have || order < best_order || (order == best_order && value < best_value))
		{
			best_order = order;
			best_value = value;
			have = true;
		}
	}

	DTC_Cell out;
	out.dtz = unbounded.value();
	if (have)
	{
		out.order = best_order;
		out.value = best_value;
	}
	return out;
}

// Every move prices a loss, so this one walks the children's whole records: the
// budget it settles on is the highest any defence needs, and the value there is
// the longest wait among them, which a child's own cheapest budget does not
// report. Every child is a win for the other side, so none needs its class read.
std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_loss(
	const Position& pos, unsigned rule50, int depth)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	uint16_t worst[DTC_PACK_LAYERS] = {};
	bool drawn[DTC_PACK_LAYERS] = {};
	bool any_legal = false;
	Unbounded_Fold unbounded(WDL_Entry::LOSE);

	// Past the band the budget answers nothing, the ply ceiling being the layer's
	// own draw.
	const auto raise = [&](size_t k, uint16_t val) {
		if (val > DTZ_MAX_NON_CURSED) drawn[k] = true;
		else if (val > worst[k]) worst[k] = val;
	};

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		const bool conversion = m.is_promotion() || m.is_ep_capture()
		                     || pos.piece_at(m.to()) != PIECE_NONE;
		Child_Pos c = make_child(pos, m);
		if (conversion || c.ps.is_bare_kings())
		{
			// A conversion ends the count here, whatever budget the rest settle on.
			for (size_t k = 0; k < DTC_PACK_LAYERS; ++k) raise(k, 1);
			unbounded.zeroing_child();
			continue;
		}

		// A push is the zeroing move itself and spends none of the winner's budget;
		// a quiet move waits one ply more at the same budget.
		const bool push = piece_type(pos.piece_at(m.from())) == PAWN;

		// A double push leaves the winner an ep capture the child's own record
		// cannot express. It converts, so where it wins it settles every budget one
		// ply out and this defence gains nothing by the push.
		if (c.ep != SQ_END)
		{
			const std::optional<bool> ep_wins = ep_conversion_wins(c, depth + 1);
			if (!ep_wins) return std::nullopt;
			if (*ep_wins)
			{
				ASSERT(push);  // only a double push leaves ep rights behind
				for (size_t k = 0; k < DTC_PACK_LAYERS; ++k) raise(k, 1);
				unbounded.zeroing_child();
				continue;
			}
		}

		DTC_Curve curve;
		if (!read_dtc_curve(open_dtc(c.ps), c.ps, c.pos, WDL_Entry::WIN, curve))
			return std::nullopt;

		if (push) unbounded.zeroing_child();
		else unbounded.quiet_child(curve.value[DTC_BUDGET_LAYERS]);
		for (size_t k = 0; k < DTC_PACK_LAYERS; ++k)
		{
			const uint16_t v = curve.value[k];
			if (v == DTC_Cell::DRAWN) drawn[k] = true;
			else raise(k, push ? uint16_t{ 1 } : static_cast<uint16_t>(v + 1));
		}
	}

	if (!any_legal) return DTC_Cell{ 0, 0, 0 };  // mate: converted, nothing owed

	DTC_Cell out;
	out.dtz = unbounded.value();
	const unsigned budget_plies = dtc_budget_plies(rule50);
	for (size_t k = 0; k < DTC_PACK_LAYERS; ++k)
		if (!drawn[k] && worst[k] <= budget_plies)
		{
			out.order = static_cast<uint16_t>(k);
			out.value = worst[k];
			break;
		}
	return out;
}

// A cursed class has no budget to look for, none of them settling it, so the
// unbounded row is the whole of what this derives.
std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_cursed(
	const Position& pos, WDL_Entry wdl, int depth)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();
	const bool winning = wdl == WDL_Entry::CURSED_WIN;
	Unbounded_Fold unbounded(wdl);
	bool any_legal = false;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		const bool conversion = m.is_promotion() || m.is_ep_capture()
		                     || pos.piece_at(m.to()) != PIECE_NONE;
		const bool push = !conversion && piece_type(pos.piece_at(m.from())) == PAWN;
		Child_Pos c = make_child(pos, m);
		if (c.ps.is_bare_kings())
		{
			if (winning) continue;  // a draw no cursed win would take
			return std::nullopt;
		}

		const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) return std::nullopt;
		// A win runs over the moves that win and nothing else can beat them; a loss
		// is priced by every move it has.
		if (winning && cw != WDL_Entry::LOSE && cw != WDL_Entry::BLESSED_LOSS) continue;

		if (conversion || push)
		{
			unbounded.zeroing_child();
			continue;
		}
		const auto cell = probe_dtc_internal(open_dtc(c.ps), c.ps, c.pos, cw,
		                                     IGNORE_50MR, depth + 1);
		if (!cell) return std::nullopt;
		unbounded.quiet_child(cell->dtz);
	}

	DTC_Cell out;
	if (any_legal) out.dtz = unbounded.value();
	return out;
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtm_internal(
	DTM_File* d, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	if (!d) return std::nullopt;

	const Frame_Read fr = locate_frame(*d, ps, pos, wdl);
	if (!fr.readable) return derive_dtm(pos, wdl, depth);
	if (!fr.mirrored) return d->read(fr.frame, pos, wdl);
	const Position mp = pos.mirror();
	return d->read(fr.frame, mp, wdl);
}

DTM50_Result Probe_Tables::Impl::probe_dtm50_internal(
	DTM50_File* m, const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	const bool flat = (rule50 == IGNORE_50MR);
	if (!flat && rule50 >= DTM50_HMC_COUNT)
		return { WDL_Entry::DRAW, 0 };
	if (!m) return {};

	// A cell carries the DRAW flip that pins DTZ; a derived one prices it itself.
	// Only layer 0 bothers: zeroing is clock-free, so probe_impl reads DTZ off
	// the flat probe and a layered one would only recompute what it drops.
	auto from_cell = [&](const DTM50_Cell& cell) {
		DTM50_Result r = (cell.value == DTM50_Cell::DRAWN)
			? DTM50_Result{ WDL_Entry::DRAW, 0 }
			: DTM50_Result{ flat ? wdl : fold_50mr_wdl(wdl), cell.value };
		if (!flat) return r;
		if (const auto dtz = dtz_from_draw_flip(cell.draw_flip, wdl, pos))
		{
			r.has_dtz = true;
			r.dtz = *dtz;
		}
		return r;
	};

	const Frame_Read fr = locate_frame(*m, ps, pos, wdl);
	if (!fr.readable)
	{
		return flat ? derive_dtm50_flat(pos, wdl, depth)
		            : derive_dtm50(pos, wdl, rule50, depth);
	}
	if (!fr.mirrored) return from_cell(m->read(fr.frame, pos, wdl, rule50));
	const Position mp = pos.mirror();
	return from_cell(m->read(fr.frame, mp, wdl, rule50));
}

std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc(
	const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;
	if (wdl == WDL_Entry::WIN)  return derive_dtc_win(pos, rule50, depth);
	if (wdl == WDL_Entry::LOSE) return derive_dtc_loss(pos, rule50, depth);
	return derive_dtc_cursed(pos, wdl, depth);
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
	Skipped_Children skipped;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry mw;
		if (c.ps.is_bare_kings())
		{
			mw = WDL_Entry::DRAW;
		}
		else if (c.is_zeroing)
		{
			const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
			// No entry, so no class either: nothing bounds this skip.
			if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); continue; }
			mw = invert_wdl(cw);
		}
		else
		{
			const WDL_Stored cs = read_wdl_stored(open_wdl(c.ps), c.pos, depth + 1);
			if (cs == WDL_Stored::ILLEGAL) { skipped.unknown(); continue; }
			mw = invert_stored(cs);
		}

		if (wdl_rank(mw) > wdl_rank(best)) best = mw;
		have_candidate = true;
	}

	if (!any_legal) return ctx.in_check ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	if (!have_candidate || skipped.unpin(best)) return WDL_Entry::ILLEGAL;
	return best;
}

// DTZ by one-ply minimax for a dropped frame of the DTZ table itself. Reads
// nothing but DTZ; a pack on hand prices zeroing in its own derive.
std::optional<uint16_t> Probe_Tables::Impl::derive_dtz(const Position& pos, WDL_Entry wdl, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;
	// A DRAW converts nowhere, and every caller prices it without a derive.
	ASSERT(wdl != WDL_Entry::DRAW);

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtz = 0;
	Skipped_Children skipped;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		uint16_t my_dtz;
		if (c.ps.is_bare_kings())
		{
			cw = WDL_Entry::DRAW;
			my_dtz = 1;
		}
		else if (c.is_zeroing)
		{
			if (child_class_is_forced(wdl))
			{
				cw = WDL_Entry::WIN;
			}
			else
			{
				cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
				if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); continue; }
			}
			my_dtz = 1;
		}
		else
		{
			if (child_class_is_forced(wdl))
			{
				cw = WDL_Entry::WIN;
			}
			else
			{
				cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); continue; }
				// Outranked: it loses the minimax however near it converts, so skipping
				// its distance cannot affect the result.
				if (below_pinned_class(wdl, dtz_lift(invert_wdl(cw)))) continue;
			}
			const auto child_dtz = probe_dtz_internal(open_dtz(c.ps), c.ps, c.pos, cw, depth + 1);
			if (!child_dtz) { skipped.of_dtz_class(invert_wdl(cw)); continue; }
			my_dtz = static_cast<uint16_t>(1u + *child_dtz);
		}

		WDL_Entry my_wdl = invert_wdl(cw);
		if (my_dtz > DTZ_MAX_NON_CURSED)
		{
			if (my_wdl == WDL_Entry::WIN)  my_wdl = WDL_Entry::CURSED_WIN;
			if (my_wdl == WDL_Entry::LOSE) my_wdl = WDL_Entry::BLESSED_LOSS;
		}

		if (!have_candidate || prefer_new(my_wdl, my_dtz, best_wdl, best_dtz))
		{
			best_wdl = my_wdl;
			best_dtz = my_dtz;
			have_candidate = true;
		}
	}

	if (!any_legal) return 0;
	if (!have_candidate || skipped.unpin(best_wdl)) return std::nullopt;
	if (best_wdl == WDL_Entry::DRAW) return 0;
	return best_dtz;
}

// DTM by one-ply minimax for a dropped frame of the DTM table. 50MR-free, so a
// cursed win mates like any other.
std::optional<uint16_t> Probe_Tables::Impl::derive_dtm(const Position& pos, WDL_Entry wdl, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;
	const WDL_Entry pinned = fold_dtm_wdl(wdl);
	// A DRAW mates nowhere, and every caller prices it without a derive.
	ASSERT(pinned != WDL_Entry::DRAW);

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtm = 0;
	Skipped_Children skipped;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		uint16_t cd;
		if (c.ps.is_bare_kings())
		{
			cw = WDL_Entry::DRAW;
			cd = 0;
		}
		else
		{
			if (c.ep != SQ_END)
			{
				Probe_Result cr = probe_impl(c.ps, c.pos, IGNORE_50MR, c.ep, depth + 1);
				if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
				{
					skipped.unknown();
					continue;
				}
				if (!cr.has_dtm)
				{
					skipped.of_class(invert_wdl(fold_dtm_wdl(cr.wdl)));
					continue;
				}
				cw = cr.wdl;
				cd = static_cast<uint16_t>(cr.dtm);
			}
			else
			{
				if (child_class_is_forced(pinned))
				{
					cw = WDL_Entry::WIN;
				}
				else
				{
					cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
					if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); continue; }
					// Outranked: no mate distance it holds can win the minimax, and a
					// skip of it would bound nothing -- of_class ranks it this far.
					if (below_pinned_class(pinned, invert_wdl(fold_dtm_wdl(cw)))) continue;
				}
				// The `dtm/` table alone: each derive stays on the file it rebuilds.
				const auto child_dtm = probe_dtm_internal(open_dtm(c.ps), c.ps, c.pos, cw, depth + 1);
				if (!child_dtm)
				{
					skipped.of_class(invert_wdl(fold_dtm_wdl(cw)));
					continue;
				}
				cd = *child_dtm;
			}
		}

		const WDL_Entry my_wdl = invert_wdl(fold_dtm_wdl(cw));
		const uint16_t my_dtm = static_cast<uint16_t>(1u + cd);

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal) return 0;
	if (!have_candidate || skipped.unpin(best_wdl)) return std::nullopt;

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return best_dtm;
	return 0;
}

// rule50-aware derive: per-child hmc (zeroing resets, quiet increments);
// once ≥100, the move is DRAW unless it mates. No DTZ rides along: zeroing is
// clock-free, so probe_impl prices it once off the layer-0 probe and drops
// whatever a layered one finds.
DTM50_Result Probe_Tables::Impl::derive_dtm50(
	const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};
	// Under the clock, cursed wins and blessed losses collapse to DRAW along with
	// a DRAW itself, and probe_impl settles all three before the layered probe.
	ASSERT(fold_50mr_wdl(wdl) != WDL_Entry::DRAW);

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;
	Skipped_Children skipped;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		const unsigned child_rule50 = c.is_zeroing ? 0u : (rule50 + 1u);

		DTM50_Result cd;
		if (c.ps.is_bare_kings())
		{
			cd = { WDL_Entry::DRAW, 0 };
		}
		else if (child_rule50 >= DTM50_HMC_COUNT)
		{
			// Quiet move past the 50MR window: DRAW unless the move is mate. The
			// mate test is the only thing that prices it, so it runs unscreened.
			cd = is_checkmate(c.pos, c.ep)
				? DTM50_Result{ WDL_Entry::LOSE, 0 }
				: DTM50_Result{ WDL_Entry::DRAW, 0 };
		}
		else
		{
			if (c.ep != SQ_END)
			{
				Probe_Result cr = probe_impl(c.ps, c.pos, child_rule50, c.ep, depth + 1);
				if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
				{
					skipped.unknown();
					continue;
				}
				if (!cr.has_dtm50)
				{
					skipped.of_class(invert_wdl(fold_50mr_wdl(cr.wdl)));
					continue;
				}
				cd = { cr.dtm50_wdl, static_cast<uint16_t>(cr.dtm50) };
			}
			else
			{
				WDL_Entry cw;
				if (child_class_is_forced(wdl))
				{
					cw = WDL_Entry::WIN;
				}
				else
				{
					cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
					if (cw == WDL_Entry::ILLEGAL)
					{
						skipped.unknown();
						continue;
					}
					// Outranked: the clock only walks an offer further toward DRAW,
					// never up to the pinned class, so the cell goes unread.
					if (below_pinned_class(wdl, invert_wdl(cw))) continue;
				}
				cd = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, cw, child_rule50, depth + 1);
				if (cd.wdl == WDL_Entry::ILLEGAL)
				{
					skipped.of_class(invert_wdl(fold_50mr_wdl(cw)));
					continue;
				}
			}
		}

		// cursed/blessed -> DRAW before inverting
		const WDL_Entry my_wdl = invert_wdl(fold_50mr_wdl(cd.wdl));
		const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.dtm));

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	DTM50_Result out;
	if (!any_legal)
	{
		out.wdl = ctx.in_check ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	}
	else if (have_candidate && !skipped.unpin(best_wdl))
	{
		const bool decisive = best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE;
		out.wdl = decisive ? best_wdl : WDL_Entry::DRAW;
		out.dtm = decisive ? best_dtm : 0;
	}

	return out;
}

// Reconstruct the pack's dropped layer-0 frame with an unbounded, one-ply DTM
// minimax. The result carries DTZ, just as a cell read derives it from the flip.
DTM50_Result Probe_Tables::Impl::derive_dtm50_flat(const Position& pos, WDL_Entry wdl, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};
	// Layer 0 is 50MR-free, so a cursed win still mates; only a true DRAW stays
	// one, and probe_impl prices that without reaching a derive.
	const WDL_Entry pinned = fold_dtm_wdl(wdl);
	ASSERT(pinned != WDL_Entry::DRAW);

	Move_List ml;
	pos.gen_pseudo_legal_moves(out_param(ml));
	const Position::Legality ctx = pos.legality_context();

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;
	DTZ_Minimax dtz;
	Skipped_Children skipped;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m, ctx)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);

		WDL_Entry cw;
		uint16_t  cd;
		if (c.ps.is_bare_kings())
		{
			cw = WDL_Entry::DRAW;
			cd = 0;
			dtz.zeroing_child(WDL_Entry::DRAW);
		}
		else if (c.ep != SQ_END)
		{
			Probe_Result cr = probe_impl(c.ps, c.pos, IGNORE_50MR, c.ep, depth + 1);
			if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			{
				skipped.unknown();
				dtz.unwalked();
				continue;
			}
			dtz.zeroing_child(cr.wdl);  // a double push zeroes the clock
			if (!cr.has_dtm)
			{
				skipped.of_class(invert_wdl(fold_dtm_wdl(cr.wdl)));
				continue;
			}
			cw = cr.wdl;
			cd = static_cast<uint16_t>(cr.dtm);
		}
		else
		{
			WDL_Entry raw_wdl;
			if (child_class_is_forced(wdl))
			{
				// Exact, not folded: the zeroing minimax running beside the mate one
				// classifies as much as it counts -- it derives the boundary cases the
				// pack leaves unpriced in the cursed band, and that needs the 5-class
				// child the fold would erase.
				raw_wdl = WDL_Entry::WIN;
			}
			else
			{
				raw_wdl = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (raw_wdl == WDL_Entry::ILLEGAL)
				{
					skipped.unknown();
					dtz.unwalked();
					continue;
				}
				// Outranked for the mate distance, and so for zeroing too: the best
				// a class can offer DTZ lifts no higher than what it offers the mate
				// minimax, so neither accumulator can take it and the cell goes unread.
				if (below_pinned_class(pinned, invert_wdl(fold_dtm_wdl(raw_wdl)))) continue;
			}
			// The pack alone: a child shipping only `dtm/` goes unpriced here.
			const DTM50_Result child = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, raw_wdl, IGNORE_50MR, depth + 1);
			if (c.is_zeroing) dtz.zeroing_child(raw_wdl);
			else              dtz.quiet_child(raw_wdl, child);
			if (child.wdl == WDL_Entry::ILLEGAL)
			{
				skipped.of_class(invert_wdl(fold_dtm_wdl(raw_wdl)));
				continue;
			}
			cw = child.wdl;
			cd = static_cast<uint16_t>(child.dtm);
		}

		const WDL_Entry my_wdl = invert_wdl(fold_dtm_wdl(cw));
		const uint16_t my_dtm = static_cast<uint16_t>(1u + cd);

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	DTM50_Result out;
	if (!any_legal)
	{
		out.wdl = ctx.in_check ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	}
	else if (have_candidate && !skipped.unpin(best_wdl))
	{
		const bool decisive = best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE;
		out.wdl = decisive ? best_wdl : WDL_Entry::DRAW;
		out.dtm = decisive ? best_dtm : 0;
	}

	dtz.finish(out, any_legal);
	return out;
}

// When `ep_square` is set, the ep-capture children are minimaxed against the
// no-ep result inline. Each such child is ep-free (probed with SQ_END), so the
// overlay bottoms out in one ply.
Probe_Result Probe_Tables::Impl::probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth)
{
	Probe_Result r;
	WDL_File* w = open_wdl(ps);
	const bool rule50_drawn = rule50 != IGNORE_50MR && rule50 >= DTM50_HMC_COUNT;
	// Every distance read is gated on `w`, so WDL absent (and not a rule50
	// auto-draw) means there is nothing to return.
	if (!w && !rule50_drawn) return r;

	r.status = Probe_Result::Status::OK;
	if (w)
	{
		r.wdl = probe_wdl_internal(w, ps, pos, depth);
		if (r.wdl == WDL_Entry::ILLEGAL) return r;
		// A DRAW is priced without opening anything: no mate to count, nowhere to
		// convert, and no clock that can turn it decisive. Every reader answers 0
		// for it anyway, so the tables only confirm what the class already said.
		if (r.wdl == WDL_Entry::DRAW)
		{
			r.has_dtm = true;
			r.has_dtz = true;
			r.has_dtc = true;
			r.dtc_wdl = WDL_Entry::DRAW;
			if (rule50 != IGNORE_50MR)
			{
				r.dtm50_wdl = WDL_Entry::DRAW;
				r.has_dtm50 = true;
			}
		}
		else if (DTM50_File* m50 = open_dtm50(ps))
		{
			const DTM50_Result d50 = probe_dtm50_internal(m50, ps, pos, r.wdl, IGNORE_50MR, depth);
			r.dtm = d50.dtm;
			r.has_dtm = d50.wdl != WDL_Entry::ILLEGAL;
			r.has_dtz = d50.has_dtz;
			r.dtz = d50.dtz;
			if (rule50_drawn)
			{
				// Mate outruns the claim, and a LOSE at flat distance 0 is one.
				// An unpriced distance tells them apart from neither, so the
				// layer goes unanswered there.
				if (r.has_dtm || r.wdl != WDL_Entry::LOSE)
				{
					r.dtm50_wdl = (r.wdl == WDL_Entry::LOSE && r.dtm == 0)
						? WDL_Entry::LOSE : WDL_Entry::DRAW;
					r.dtm50 = 0;
					r.has_dtm50 = true;
				}
			}
			else if (rule50 != IGNORE_50MR)
			{
				// Cursed/blessed are DRAW at every layer; the clock is never read.
				if (fold_50mr_wdl(r.wdl) == WDL_Entry::DRAW)
				{
					r.dtm50_wdl = WDL_Entry::DRAW;
					r.dtm50 = 0;
					r.has_dtm50 = true;
				}
				else if (r.has_dtm && dtm50_layer_pinned_by_dtm(r.wdl, r.dtm, rule50))
				{
					r.dtm50_wdl = r.wdl;  // plain W/L, so the DTM50 fold is the identity
					r.dtm50 = r.dtm;
					r.has_dtm50 = true;
				}
				else if (layer_busted_by_dtz(r.wdl, r.dtz, rule50))
				{
					r.dtm50_wdl = WDL_Entry::DRAW;
					r.dtm50 = 0;
					r.has_dtm50 = true;
				}
				else
				{
					const DTM50_Result d50r = probe_dtm50_internal(m50, ps, pos, r.wdl, rule50, depth);
					r.dtm50_wdl = d50r.wdl;
					r.dtm50 = d50r.dtm;
					r.has_dtm50 = d50r.wdl != WDL_Entry::ILLEGAL;
				}
			}
		}
		else if (DTM_File* m = open_dtm(ps))
		{
			const auto dtm = probe_dtm_internal(m, ps, pos, r.wdl, depth);
			r.has_dtm = dtm.has_value();
			if (dtm) r.dtm = *dtm;
		}

		// DTC first: its pack answers both metrics, the budget the caller's clock
		// picks and the unbounded row that is the DTZ table it embeds. A cursed class
		// or an outrun clock leaves no budget to pick, which the read reports by
		// pricing nothing, and a DRAW is priced above without opening anything.
		if (!r.has_dtc && ps.has_pawns())
			if (DTC_File* c = open_dtc(ps))
				if (const auto cell = probe_dtc_internal(c, ps, pos, r.wdl, rule50, depth))
				{
					r.has_dtc = true;
					r.dtc_wdl = cell->priced() ? r.wdl : WDL_Entry::DRAW;
					r.dtc_order = cell->priced() ? cell->order : 0;
					r.dtc = cell->priced() ? cell->value : 0;
					// A record carries that row and so does the derive, so a decisive
					// class always has one.
					ASSERT(cell->dtz != DTC_Cell::DRAWN);
					r.has_dtz = true;
					r.dtz = cell->dtz;
				}

		// What the packs above left: the DTM50 one stops at the cursed band, and a
		// DTC one answers only the materials it is built for.
		if (!r.has_dtz)
			if (DTZ_File* d = open_dtz(ps))
			{
				const auto dtz = probe_dtz_internal(d, ps, pos, r.wdl, depth);
				r.has_dtz = dtz.has_value();
				if (dtz) r.dtz = *dtz;
			}

		// Pawnless materials carry no pack: with no push to budget the stack is one
		// layer, every zeroing move is a capture and so a conversion, which leaves
		// DTZ's own number at order 0. A cursed class is drawn without reading it.
		if (!r.has_dtc && !ps.has_pawns())
		{
			const bool clean = r.wdl == WDL_Entry::WIN || r.wdl == WDL_Entry::LOSE;
			if (!clean)
			{
				r.has_dtc = true;
				r.dtc_wdl = WDL_Entry::DRAW;
			}
			else if (r.has_dtz)
			{
				const bool busted = rule50 != IGNORE_50MR
					&& layer_busted_by_dtz(r.wdl, r.dtz, rule50);
				r.has_dtc = true;
				r.dtc_wdl = busted ? WDL_Entry::DRAW : r.wdl;
				r.dtc = busted ? 0 : r.dtz;
			}
		}
	}

	if (ep_square == SQ_END)
		return r;

	Move_List eps;
	add_ep_moves(pos, ep_square, eps);
	if (eps.empty()) return r;

	Probe_Result best = r;
	WDL_Entry best_dtz_wdl = r.wdl;
	uint16_t  best_dtz     = r.has_dtz ? static_cast<uint16_t>(r.dtz) : 0;
	WDL_Entry best_dtm_wdl = fold_dtm_wdl(r.wdl);
	uint16_t  best_dtm     = r.has_dtm ? static_cast<uint16_t>(r.dtm) : 0;
	WDL_Entry best_dtm50_wdl = r.has_dtm50 ? r.dtm50_wdl : fold_50mr_wdl(r.wdl);
	uint16_t  best_dtm50     = r.has_dtm50 ? static_cast<uint16_t>(r.dtm50) : 0;
	// DTC compares on its own class, not the clock-independent one: a base this
	// clock has drawn must lose to an ep conversion that still wins.
	WDL_Entry best_dtc_wdl   = r.has_dtc ? r.dtc_wdl : r.wdl;
	uint16_t  best_dtc_order = r.has_dtc ? static_cast<uint16_t>(r.dtc_order) : 0;
	uint16_t  best_dtc       = r.has_dtc ? static_cast<uint16_t>(r.dtc) : 0;

	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!pos.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(pos, eps[i]);
		Probe_Result cr;
		if (child.ps.is_bare_kings())
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtz = best.has_dtz;
			cr.dtz = 0;
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
			return {};

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		if (wdl_rank(my_wdl) > wdl_rank(best.wdl))
			best.wdl = my_wdl;

		// An ep capture zeroes, so it converts at dtz 1 whatever the child holds.
		// The base value has to be known only to break a tie in class -- outranked,
		// it does not enter, which is how a cursed base the pack cannot pin still
		// reports a dtz once ep lifts it to a strict win or loss.
		const bool ep_outranks_dtz = wdl_rank(my_wdl) > wdl_rank(best_dtz_wdl);
		if (ep_outranks_dtz
			|| (best.has_dtz && prefer_new(my_wdl, 1, best_dtz_wdl, best_dtz)))
		{
			best_dtz_wdl = my_wdl;
			best_dtz = 1;
			best.dtz = (my_wdl == WDL_Entry::DRAW) ? 0 : 1;
			best.has_dtz = true;
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

		// An ep capture is a conversion: it owes no push and lands one ply out, so
		// (0, 1) needs no table to know, and the base value enters only to break a
		// tie in class. A conversion is terminal to DTC, so its class is the child's
		// WDL folded to what a layer holds, as the generator classifies one.
		const WDL_Entry my_dtc_wdl = fold_50mr_wdl(invert_wdl(cr.wdl));
		const bool ep_outranks_dtc = wdl_rank(my_dtc_wdl) > wdl_rank(best_dtc_wdl);
		if (ep_outranks_dtc
			|| (best.has_dtc && prefer_new_dtc(my_dtc_wdl, 0, 1, best_dtc_wdl, best_dtc_order, best_dtc)))
		{
			const bool ep_prices = my_dtc_wdl != WDL_Entry::DRAW;
			best_dtc_wdl = my_dtc_wdl;
			best_dtc_order = 0;
			best_dtc = 1;
			best.has_dtc = true;
			best.dtc_wdl = my_dtc_wdl;
			best.dtc_order = 0;
			best.dtc = ep_prices ? 1 : 0;
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
	WDL_Entry best = probe_wdl_internal(open_wdl(ps), ps, pos, depth);
	if (best == WDL_Entry::ILLEGAL || ep_square == SQ_END)
		return best;

	Move_List eps;
	add_ep_moves(pos, ep_square, eps);
	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!pos.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(pos, eps[i]);
		const WDL_Entry cw = child.ps.is_bare_kings()
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
		pos = input.mirror();
		if (ep_square != SQ_END)
			ep_square = sq_rank_mirror(ep_square);
		return { std::move(ps), std::move(pos), ep_square, true };
	}
	return { std::move(ps), std::move(pos), ep_square, false };
}

// Material key of `pos` for comparison against a (possibly pair-bearing)
// Piece_Config. When `ps` carries a pair, the canonical opposing pair's
// two pawns are excluded from the key and the pair flag is set instead -- the
// board shows them as ordinary pawns.
void pair_aware_literal_key(const Piece_Config& ps, const Position& pos,
                            Material_Key& out)
{
	Square pw = SQ_END, pb = SQ_END;
	if (ps.has_opposing_pair())
	{
		Square ws[16], bs[16];
		size_t nw = 0, nb = 0;
		Bitboard wb = pos.piece_bb(WHITE_PAWN);
		while (wb) ws[nw++] = wb.pop_first_square();
		Bitboard bb = pos.piece_bb(BLACK_PAWN);
		while (bb) bs[nb++] = bb.pop_first_square();
		Pair_Group::canonical_pair(Const_Span<Square>(ws, nw),
		                           Const_Span<Square>(bs, nb), pw, pb);
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
	if (ps.has_opposing_pair())
		k.add_pair();
	out = k;
}

// Opposing-pair material for `pos`: physical pieces minus the canonical opposing
// pair's two pawns, with the pair flag set. nullopt if there is no pair.
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
	ps.mark_opposing_pair();
	return ps;
}

Canonical_Root canonical_root_from_config(
	const Piece_Config& ps, const Position& input, Square ep_square)
{
	Material_Key literal_key;
	pair_aware_literal_key(ps, input, literal_key);
	const auto [base_key, mirror_key] = ps.material_keys();
	if (literal_key == base_key)
		return Canonical_Root{ ps, input, ep_square, false };
	ASSERT(literal_key == mirror_key);

	Position pos = input.mirror();
	if (ep_square != SQ_END)
		ep_square = sq_rank_mirror(ep_square);
	return Canonical_Root{ ps, std::move(pos), ep_square, true };
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
	for (const auto& d : dtz_dirs) scan_dir(d, DTZ_EXT);
	for (const auto& d : dtc_dirs) scan_dir(d, DTC_EXT);
	for (const auto& d : dtm_dirs) scan_dir(d, DTM_EXT);
	for (const auto& d : dtm50_dirs) scan_dir(d, DTM50_EXT);
	largest_pieces.store(lg, std::memory_order_release);
}

Probe_Tables::Probe_Tables() : m_impl(std::make_unique<Impl>()) {}
Probe_Tables::~Probe_Tables() = default;
Probe_Tables::Probe_Tables(Probe_Tables&&) noexcept = default;
Probe_Tables& Probe_Tables::operator=(Probe_Tables&&) noexcept = default;

// init() falls back to `dir` itself for every table subdirectory it does not
// find, so the same one arrives repeatedly; a repeat would only add a redundant
// lookup and another scan_paths pass. Compared lexically normalized, since
// `./wdl/` and `wdl` name one directory.
static void add_search_dir(std::vector<std::filesystem::path>& dirs, std::filesystem::path dir)
{
	const std::filesystem::path key = dir.lexically_normal();
	for (const std::filesystem::path& d : dirs)
		if (d.lexically_normal() == key) return;
	dirs.emplace_back(std::move(dir));
}

void Probe_Tables::add_wdl_path(std::filesystem::path dir) { add_search_dir(m_impl->wdl_dirs, std::move(dir)); }
void Probe_Tables::add_dtz_path(std::filesystem::path dir) { add_search_dir(m_impl->dtz_dirs, std::move(dir)); }
void Probe_Tables::add_dtc_path(std::filesystem::path dir) { add_search_dir(m_impl->dtc_dirs, std::move(dir)); }
void Probe_Tables::add_dtm_path(std::filesystem::path dir) { add_search_dir(m_impl->dtm_dirs, std::move(dir)); }
void Probe_Tables::add_dtm50_path(std::filesystem::path dir) { add_search_dir(m_impl->dtm50_dirs, std::move(dir)); }

bool Probe_Tables::init(const std::filesystem::path& dir)
{
	std::error_code ec;
	auto try_sub = [&](const char* sub) -> std::filesystem::path {
		auto p = dir / sub;
		return std::filesystem::is_directory(p, ec) ? p : dir;
	};
	add_search_dir(m_impl->wdl_dirs, try_sub("wdl"));
	add_search_dir(m_impl->dtz_dirs, try_sub("dtz"));
	add_search_dir(m_impl->dtc_dirs, try_sub("dtc"));
	add_search_dir(m_impl->dtm_dirs, try_sub("dtm"));
	add_search_dir(m_impl->dtm50_dirs, try_sub("dtm50"));
	m_impl->scan_paths();
	return m_impl->largest_pieces.load(std::memory_order_acquire) > 0;
}

size_t Probe_Tables::largest() const
{
	return m_impl->largest_pieces.load(std::memory_order_acquire);
}

void Probe_Tables::rescan() { m_impl->invalidate_tables(); m_impl->scan_paths(); }

void Probe_Tables::set_block_cache_bytes(size_t bytes) { m_impl->blocks->set_max_bytes(bytes); }
size_t Probe_Tables::block_cache_bytes() const { return m_impl->blocks->max_bytes(); }
size_t Probe_Tables::block_cache_bytes_used() const { return m_impl->blocks->cur_bytes(); }

// Opposing-pair routing shared by every probe entry point: if `pos` has such a
// pair and that 'p' table is on disk, return its canonical root. The 'p'
// table covers a partial domain and a position has the same value in either
// table, so "prefer the pair table when present" is always safe -- even for the
// explicit-ps overloads and the root-move rankers, where the physical material is
// the fallback.
std::optional<Canonical_Root> Probe_Tables::Impl::route_pair(
	const Position& pos, Square ep_square)
{
	if (std::optional<Piece_Config> pair_ps = pair_config_from_position(pos))
	{
		Canonical_Root r = canonical_root_from_config(*pair_ps, pos, ep_square);
		if (has_any_table(r.ps))
			return r;
	}
	return std::nullopt;
}

Child_Pos Probe_Tables::Impl::make_child(const Position& parent, Move m)
{
	const bool zeroing = move_is_zeroing(parent, m);
	Position pos = parent;
	(void)pos.do_move(m);
	const Square raw_ep = parent.ep_square_after_move(m);

	// Prefer the child's opposing-pair table when one is on disk. A move that keeps
	// the pair (any non-capture, including a free-pawn push) stays in a 'p'
	// material, which the board-derived config below would miss -- it sees the
	// pair pawns as ordinary free pawns. route_pair re-indexes into the pair table
	// and mirrors ep to match; it returns nullopt for captures/promotions (which
	// cross into a non-pair sub-table) and when no 'p' table is on disk, so we
	// then fall back to the board's full physical material.
	if (std::optional<Canonical_Root> r = route_pair(pos, raw_ep))
	{
		return Child_Pos{ std::move(r->pos), std::move(r->ps), r->ep_square, zeroing };
	}

	auto [cps, lit] = piece_config_and_literal_key_from_position(pos);
	Square ep = raw_ep;
	if (lit != cps.base_material_key())
	{
		pos = pos.mirror();
		if (ep != SQ_END) ep = sq_rank_mirror(ep);
	}
	return Child_Pos{ std::move(pos), std::move(cps), ep, zeroing };
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
	const Canonical_Root root = canonical_root_from_config(ps, pos, ep_square);
	return m_impl->probe_impl(root.ps, root.pos, rule50, root.ep_square, 0);
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
	const Canonical_Root root = canonical_root_from_config(ps, pos, ep_square);
	return m_impl->probe_wdl_impl(root.ps, root.pos, root.ep_square, 0);
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
	if (!r.has_dtz) return 0;
	const int v = static_cast<int>(r.dtz);
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
		// The clock the child actually holds, which is what DTC answers against.
		// Class and zeroing distance do not depend on it, so the Fathom ranking
		// below reads the same either way.
		const unsigned child_rule50 = !use_rule50 ? IGNORE_50MR
			: (c.is_zeroing ? 0u : rule50 + 1u);
		Probe_Result cr;
		if (c.ps.is_bare_kings())
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtz = true;
			cr.dtz = 0;
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, child_rule50, c.ep, 0);
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
			if (!cr.has_dtz) return {};
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

		int rank = fathom_dtz_rank(v, rule50, has_repeated);

		// What DTC makes of the same move: a capture or promotion converts here and
		// owes nothing, so it prices itself, while anything else takes the child's
		// pair, a push spending one more of the winner's budget and a quiet move
		// waiting a ply longer.
		const bool conversion = m.is_promotion() || m.is_ep_capture()
		                     || probe_pos.piece_at(m.to()) != PIECE_NONE;
		int order = -1;
		int wait = 0;
		if (conversion && (my_wdl == WDL_Entry::WIN || my_wdl == WDL_Entry::LOSE))
		{
			order = 0;
			wait = 1;
		}
		else if (!conversion && cr.has_dtc
			&& (cr.dtc_wdl == WDL_Entry::WIN || cr.dtc_wdl == WDL_Entry::LOSE))
		{
			const bool spends = c.is_zeroing && my_wdl == WDL_Entry::WIN;
			order = static_cast<int>(cr.dtc_order) + (spends ? 1 : 0);
			wait = c.is_zeroing ? 1 : static_cast<int>(cr.dtc) + 1;
		}
		// Priced, so the move answers on DTC's line instead: the wait it costs, and
		// the pushes still owed folded into the rank. Fathom leaves the bands next
		// to 1000 and -1000 unused, so those pushes fit without crossing a verdict.
		if (order >= 0)
		{
			if (rank == 1000)
			{
				rank = 1000 - order;
				v = wait;
			}
			else if (rank == -1000)
			{
				rank = -1000 + order;
				v = -wait;
			}
		}

		const int score = fathom_dtz_score(rank, bound);
		out.push_back(Root_Move{root.mirrored ? rank_mirror_move(m) : m, my_wdl, v, rank, score});
	}

	// Same rank is the same verdict owing the same pushes, so the wait decides --
	// shortest for a win, longest for a loss, which the signed field says at once.
	std::stable_sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) {
			if (a.rank != b.rank) return a.rank > b.rank;
			if (a.rank == 0) return false;
			return a.dtz < b.dtz;
		});
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
		const WDL_Entry cw = c.ps.is_bare_kings()
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
		if (c.ps.is_bare_kings())
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
