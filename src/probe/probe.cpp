#include "chess/castling_group.h"
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
		if (file_exists_case_exact(p))
		{
			if (out) *out = std::move(p);
			return true;
		}
	}
	return false;
}

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
		if (epoch_src.load(std::memory_order_relaxed) != epoch)
			continue;

		auto [it, inserted] = cache.try_emplace(k, std::move(built));
		tl.insert(epoch, k, it->second);
		return it->second.get();
	}
}

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

NODISCARD WDL_Entry fold_50mr_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN)   return WDL_Entry::DRAW;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::DRAW;
	return w;
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

NODISCARD bool below_pinned_class(WDL_Entry pinned, WDL_Entry offer)
{
	return pinned != WDL_Entry::ILLEGAL && wdl_rank(offer) < wdl_rank(pinned);
}

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

NODISCARD WDL_Entry dtz_lift(WDL_Entry my_wdl)
{
	return my_wdl == WDL_Entry::LOSE ? WDL_Entry::BLESSED_LOSS : my_wdl;
}

NODISCARD bool is_symmetric_material(const Piece_Config& ps)
{
	const auto [mat_key, mir_key] = ps.material_keys();
	return mat_key == mir_key;
}

NODISCARD bool is_win_class(WDL_Entry w)
{
	return w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN;
}

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

void fill_pawnless_dtc_from_dtz(In_Out_Param<Probe_Result> r, unsigned rule50)
{
	const bool clean = r->wdl == WDL_Entry::WIN || r->wdl == WDL_Entry::LOSE;
	if (!clean)
	{
		r->has_dtc = true;
		r->dtc_wdl = WDL_Entry::DRAW;
		return;
	}
	if (!r->has_dtz) return;

	const bool busted = rule50 != IGNORE_50MR
		&& layer_busted_by_dtz(r->wdl, r->dtz, rule50);
	r->has_dtc = true;
	r->dtc_wdl = busted ? WDL_Entry::DRAW : r->wdl;
	r->dtc = busted ? 0 : r->dtz;
}

struct Child_Pos { Position pos; Piece_Config ps; Square ep; bool is_zeroing; };

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


NODISCARD std::optional<uint16_t> dtz_from_draw_flip(
	uint16_t h, WDL_Entry wdl, const Position& pos)
{
	if (wdl == WDL_Entry::DRAW || wdl == WDL_Entry::ILLEGAL) return uint16_t{0};
	if (wdl != WDL_Entry::WIN && wdl != WDL_Entry::LOSE) return std::nullopt;
	if (h == 0)
		return (wdl == WDL_Entry::LOSE && pos.is_checkmate()) ? uint16_t{0} : uint16_t{1};
	if (h == 1) return std::nullopt;
	return static_cast<uint16_t>(DTZ_MAX_NON_CURSED + 2u - h);
}

struct Skipped_Children
{
	void of_class(WDL_Entry my_wdl) { best_rank = std::max(best_rank, wdl_rank(my_wdl)); }
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

struct DTZ_Minimax
{
	void zeroing_child(WDL_Entry child_wdl) { offer(child_wdl, 1); }

	void quiet_child(WDL_Entry child_wdl, const DTM50_Result& child)
	{
		if (child.has_dtz)
			offer(child_wdl, static_cast<uint16_t>(1u + child.dtz));
		else
			skipped.of_dtz_class(invert_wdl(child_wdl));
	}

	void unwalked() { skipped.unknown(); }

	void finish(DTM50_Result& out, bool any_legal) const
	{
		if (!any_legal)
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

	std::mutex tables_mu;
	std::unordered_map<Material_Key, std::shared_ptr<WDL_File>, Material_Key_Hash>   wdl_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTZ_File>, Material_Key_Hash>   dtz_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTC_File>, Material_Key_Hash>   dtc_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTM_File>, Material_Key_Hash>   dtm_cache;
	std::unordered_map<Material_Key, std::shared_ptr<DTM50_File>, Material_Key_Hash> dtm50_cache;

	std::atomic<size_t> largest_pieces{0};

	template <typename File>
	NODISCARD File* open_table(
		std::unordered_map<Material_Key, std::shared_ptr<File>, Material_Key_Hash>& cache,
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

	NODISCARD bool has_any_table(const Piece_Config& ps)
	{
		return open_wdl(ps) != nullptr;
	}

	NODISCARD Probe_Result probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth);
	NODISCARD WDL_Entry probe_wdl_impl(const Piece_Config& ps, const Position& pos, Square ep_square, int depth);
	NODISCARD std::optional<Canonical_Root> route_specialized(const Position& pos, Square ep_square);
	NODISCARD Child_Pos make_child(const Position& parent, Move m);
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
	NODISCARD std::optional<uint16_t> derive_dtz(const Position& pos, WDL_Entry wdl, int depth,
	                                            std::optional<uint16_t> stored = std::nullopt);
	NODISCARD std::optional<DTC_Cell> derive_dtc(const Position& pos, WDL_Entry wdl,
	                                            unsigned rule50, int depth);
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
		std::lock_guard<std::mutex> lk(tables_mu);
		wdl_cache.clear();
		dtz_cache.clear();
		dtc_cache.clear();
		dtm_cache.clear();
		dtm50_cache.clear();
		epoch.store(next_epoch(), std::memory_order_release);
	}
};

WDL_Entry Probe_Tables::Impl::relax_bound_wdl(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	WDL_Entry best = WDL_Entry::ILLEGAL;
	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		if (!m.is_promotion() && !pos.move_is_capture(m)) return false;

		Child_Pos c = make_child(pos, m);
		const WDL_Entry cw = c.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) return false;

		const WDL_Entry mine = invert_wdl(cw);
		if (wdl_rank(mine) > wdl_rank(best)) best = mine;
		return best == WDL_Entry::WIN;
	});
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

	const uint16_t stored = fr.mirrored ? d->read(fr.frame, pos.mirror(), wdl)
	                                    : d->read(fr.frame, pos, wdl);
	if (!d->is_relaxed[fr.frame] || !is_win_class(wdl)) return stored;
	return derive_dtz(pos, wdl, depth, stored);
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

std::optional<bool> Probe_Tables::Impl::ep_conversion_wins(const Child_Pos& c, int depth)
{
	bool unknown = false;
	const bool wins = c.pos.visit_legal_ep_captures(c.ep, [&](Move m) FORCE_INLINE_LAMBDA {
		Child_Pos gc = make_child(c.pos, m);
		const WDL_Entry gw = gc.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: probe_wdl_impl(gc.ps, gc.pos, gc.ep, depth + 1);
		if (gw == WDL_Entry::ILLEGAL) { unknown = true; return false; }
		return fold_50mr_wdl(invert_wdl(gw)) == WDL_Entry::WIN;
	});
	if (wins) return true;
	if (unknown) return std::nullopt;
	return false;
}

std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_win(
	const Position& pos, unsigned rule50, int depth)
{
	bool have = false;
	uint16_t best_order = 0;
	uint16_t best_value = 0;
	const unsigned budget_plies = dtc_budget_plies(rule50);
	Unbounded_Fold unbounded(WDL_Entry::WIN);
	Skipped_Children skipped;

	const bool pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		const bool conversion = m.is_promotion() || pos.move_is_capture(m);
		const bool push = !conversion && piece_type(pos.piece_at(m.from())) == PAWN;
		Child_Pos c = make_child(pos, m);
		if (c.ps.is_bare_kings()) return false;

		const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
		if (cw != WDL_Entry::LOSE) return false;

		uint16_t order = 0;
		uint16_t value = 1;
		if (conversion)
		{
			unbounded.zeroing_child();
		}
		else
		{
			const unsigned child_rule50 = push ? 0u
				: (rule50 == IGNORE_50MR ? IGNORE_50MR : rule50 + 1u);
			const auto cell = child_dtc_cell(c, cw, child_rule50, depth + 1);
			if (!cell) { skipped.unknown(); return false; }
			if (push) unbounded.zeroing_child();
			else unbounded.quiet_child(cell->dtz);
			if (!cell->priced()) return false;
			order = static_cast<uint16_t>(cell->order + (push ? 1 : 0));
			value = push ? uint16_t{ 1 } : static_cast<uint16_t>(cell->value + 1);
		}
		if (value > budget_plies) return false;

		if (!have || order < best_order || (order == best_order && value < best_value))
		{
			best_order = order;
			best_value = value;
			have = true;
		}
		return best_order == 0 && best_value == 1;
	});
	if (!pinned && skipped.unpin(WDL_Entry::WIN)) return std::nullopt;

	DTC_Cell out;
	out.dtz = pinned ? uint16_t{ 1 } : unbounded.value();
	ASSERT(!pinned || out.dtz == 1);
	if (have)
	{
		out.order = best_order;
		out.value = best_value;
	}
	return out;
}

std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_loss(
	const Position& pos, unsigned rule50, int depth)
{
	uint16_t worst[DTC_PACK_LAYERS] = {};
	bool drawn[DTC_PACK_LAYERS] = {};
	Unbounded_Fold unbounded(WDL_Entry::LOSE);

	const auto raise = [&](size_t k, uint16_t val) {
		if (val > DTZ_MAX_NON_CURSED) drawn[k] = true;
		else if (val > worst[k]) worst[k] = val;
	};

	bool any_legal = false;

	const bool unknown = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		// Only a capture can bare the kings, so `conversion` already covers that.
		const bool conversion = m.is_promotion() || pos.move_is_capture(m);
		if (conversion)
		{
			for (size_t k = 0; k < DTC_PACK_LAYERS; ++k) raise(k, 1);
			unbounded.zeroing_child();
			return false;
		}

		Child_Pos c = make_child(pos, m);
		const bool push = piece_type(pos.piece_at(m.from())) == PAWN;

		if (c.ep != SQ_END)
		{
			const std::optional<bool> ep_wins = ep_conversion_wins(c, depth + 1);
			if (!ep_wins) return true;
			if (*ep_wins)
			{
				ASSERT(push);
				for (size_t k = 0; k < DTC_PACK_LAYERS; ++k) raise(k, 1);
				unbounded.zeroing_child();
				return false;
			}
		}

		DTC_Curve curve;
		if (!read_dtc_curve(open_dtc(c.ps), c.ps, c.pos, WDL_Entry::WIN, curve))
			return true;

		if (push) unbounded.zeroing_child();
		else unbounded.quiet_child(curve.value[DTC_BUDGET_LAYERS]);
		for (size_t k = 0; k < DTC_PACK_LAYERS; ++k)
		{
			const uint16_t v = curve.value[k];
			if (v == DTC_Cell::DRAWN) drawn[k] = true;
			else raise(k, push ? uint16_t{ 1 } : static_cast<uint16_t>(v + 1));
		}
		return false;
	});
	if (unknown) return std::nullopt;

	if (!any_legal) return DTC_Cell{ 0, 0, 0 };

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

std::optional<DTC_Cell> Probe_Tables::Impl::derive_dtc_cursed(
	const Position& pos, WDL_Entry wdl, int depth)
{
	const bool winning = wdl == WDL_Entry::CURSED_WIN;
	Unbounded_Fold unbounded(wdl);
	bool any_legal = false;
	Skipped_Children skipped;

	const bool pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const bool conversion = m.is_promotion() || pos.move_is_capture(m);
		const bool push = !conversion && piece_type(pos.piece_at(m.from())) == PAWN;
		Child_Pos c = make_child(pos, m);
		if (c.ps.is_bare_kings())
		{
			// Baring the kings takes a capture, and from a blessed loss such a
			// capture would be an outright draw, not a loss.
			ASSERT(winning);
			return false;
		}

		const WDL_Entry cw = probe_wdl_impl(c.ps, c.pos, c.ep, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
		if (winning && cw != WDL_Entry::LOSE && cw != WDL_Entry::BLESSED_LOSS) return false;

		if (conversion || push)
		{
			unbounded.zeroing_child();
			return winning && invert_wdl(cw) == wdl;
		}
		const auto cell = probe_dtc_internal(open_dtc(c.ps), c.ps, c.pos, cw,
		                                     IGNORE_50MR, depth + 1);
		if (!cell) { skipped.unknown(); return false; }
		unbounded.quiet_child(cell->dtz);
		return false;
	});
	if (!pinned && skipped.unpin(wdl)) return std::nullopt;

	DTC_Cell out;
	if (any_legal) out.dtz = pinned ? uint16_t{ 1 } : unbounded.value();
	ASSERT(!pinned || out.dtz == 1);
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

WDL_Entry Probe_Tables::Impl::derive_wdl(const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best = WDL_Entry::LOSE;
	Skipped_Children skipped;

	const bool pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
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
			if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
			mw = invert_wdl(cw);
		}
		else
		{
			const WDL_Stored cs = read_wdl_stored(open_wdl(c.ps), c.pos, depth + 1);
			if (cs == WDL_Stored::ILLEGAL) { skipped.unknown(); return false; }
			mw = invert_stored(cs);
		}

		if (wdl_rank(mw) > wdl_rank(best)) best = mw;
		have_candidate = true;
		return best == WDL_Entry::WIN;
	});

	if (pinned) return best;
	if (!any_legal) return pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	if (!have_candidate || skipped.unpin(best)) return WDL_Entry::ILLEGAL;
	return best;
}

// In a relaxed winning frame, first check whether a zeroing move proves DTZ 1.
// If none does, use the decoded value. Quiet moves cannot prove this condition,
// and a missing zeroing child makes the result unknown.
std::optional<uint16_t> Probe_Tables::Impl::derive_dtz(const Position& pos, WDL_Entry wdl, int depth,
                                                       std::optional<uint16_t> stored)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;
	ASSERT(wdl != WDL_Entry::DRAW);
	ASSERT(!stored || is_win_class(wdl));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtz = 0;
	Skipped_Children skipped;

	const bool pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
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
				if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
			}
			my_dtz = 1;
		}
		else
		{
			if (stored) return false;
			if (child_class_is_forced(wdl))
			{
				cw = WDL_Entry::WIN;
			}
			else
			{
				cw = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
				if (below_pinned_class(wdl, dtz_lift(invert_wdl(cw)))) return false;
			}
			const auto child_dtz = probe_dtz_internal(open_dtz(c.ps), c.ps, c.pos, cw, depth + 1);
			if (!child_dtz) { skipped.of_dtz_class(invert_wdl(cw)); return false; }
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
		return is_win_class(wdl) && best_wdl == wdl && best_dtz == 1;
	});

	if (pinned) return best_dtz;
	if (stored) return skipped.unpin(best_wdl) ? std::nullopt : stored;
	if (!any_legal) return 0;
	if (!have_candidate || skipped.unpin(best_wdl)) return std::nullopt;
	if (best_wdl == WDL_Entry::DRAW) return 0;
	return best_dtz;
}

std::optional<uint16_t> Probe_Tables::Impl::derive_dtm(const Position& pos, WDL_Entry wdl, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;
	const WDL_Entry pinned = fold_dtm_wdl(wdl);
	ASSERT(pinned != WDL_Entry::DRAW);

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtm = 0;
	Skipped_Children skipped;

	const bool distance_pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
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
					return false;
				}
				if (!cr.has_dtm)
				{
					skipped.of_class(invert_wdl(fold_dtm_wdl(cr.wdl)));
					return false;
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
					if (cw == WDL_Entry::ILLEGAL) { skipped.unknown(); return false; }
					if (below_pinned_class(pinned, invert_wdl(fold_dtm_wdl(cw)))) return false;
				}
				const auto child_dtm = probe_dtm_internal(open_dtm(c.ps), c.ps, c.pos, cw, depth + 1);
				if (!child_dtm)
				{
					skipped.of_class(invert_wdl(fold_dtm_wdl(cw)));
					return false;
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
		return is_win_class(pinned) && best_wdl == pinned && best_dtm == 1;
	});

	if (distance_pinned) return best_dtm;
	if (!any_legal) return 0;
	if (!have_candidate || skipped.unpin(best_wdl)) return std::nullopt;

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return best_dtm;
	return 0;
}

DTM50_Result Probe_Tables::Impl::derive_dtm50(
	const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};
	const WDL_Entry pinned = fold_50mr_wdl(wdl);
	ASSERT(pinned != WDL_Entry::DRAW);

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;
	Skipped_Children skipped;

	const bool distance_pinned = pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
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
			cd = c.pos.is_checkmate(c.ep)
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
					return false;
				}
				if (!cr.has_dtm50)
				{
					skipped.of_class(invert_wdl(fold_50mr_wdl(cr.wdl)));
					return false;
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
						return false;
					}
					if (below_pinned_class(wdl, invert_wdl(cw))) return false;
				}
				cd = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, cw, child_rule50, depth + 1);
				if (cd.wdl == WDL_Entry::ILLEGAL)
				{
					skipped.of_class(invert_wdl(fold_50mr_wdl(cw)));
					return false;
				}
			}
		}

		const WDL_Entry my_wdl = invert_wdl(fold_50mr_wdl(cd.wdl));
		const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.dtm));

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
		return is_win_class(pinned) && best_wdl == pinned && best_dtm == 1;
	});

	DTM50_Result out;
	if (distance_pinned)
	{
		out.wdl = best_wdl;
		out.dtm = best_dtm;
	}
	else if (!any_legal)
	{
		out.wdl = pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	}
	else if (have_candidate && !skipped.unpin(best_wdl))
	{
		const bool decisive = best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE;
		out.wdl = decisive ? best_wdl : WDL_Entry::DRAW;
		out.dtm = decisive ? best_dtm : 0;
	}

	return out;
}

DTM50_Result Probe_Tables::Impl::derive_dtm50_flat(const Position& pos, WDL_Entry wdl, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};
	const WDL_Entry pinned = fold_dtm_wdl(wdl);
	ASSERT(pinned != WDL_Entry::DRAW);

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;
	DTZ_Minimax dtz;
	Skipped_Children skipped;

	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
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
				return false;
			}
			dtz.zeroing_child(cr.wdl);
			if (!cr.has_dtm)
			{
				skipped.of_class(invert_wdl(fold_dtm_wdl(cr.wdl)));
				return false;
			}
			cw = cr.wdl;
			cd = static_cast<uint16_t>(cr.dtm);
		}
		else
		{
			WDL_Entry raw_wdl;
			if (child_class_is_forced(wdl))
			{
				raw_wdl = WDL_Entry::WIN;
			}
			else
			{
				raw_wdl = probe_wdl_internal(open_wdl(c.ps), c.ps, c.pos, depth + 1);
				if (raw_wdl == WDL_Entry::ILLEGAL)
				{
					skipped.unknown();
					dtz.unwalked();
					return false;
				}
				if (below_pinned_class(pinned, invert_wdl(fold_dtm_wdl(raw_wdl)))) return false;
			}
			const DTM50_Result child = probe_dtm50_internal(open_dtm50(c.ps), c.ps, c.pos, raw_wdl, IGNORE_50MR, depth + 1);
			if (c.is_zeroing) dtz.zeroing_child(raw_wdl);
			else              dtz.quiet_child(raw_wdl, child);
			if (child.wdl == WDL_Entry::ILLEGAL)
			{
				skipped.of_class(invert_wdl(fold_dtm_wdl(raw_wdl)));
				return false;
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
		return false;
	});

	DTM50_Result out;
	if (!any_legal)
	{
		out.wdl = pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW;
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

Probe_Result Probe_Tables::Impl::probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, Square ep_square, int depth)
{
	Probe_Result r;
	WDL_File* w = open_wdl(ps);
	const bool rule50_drawn = rule50 != IGNORE_50MR && rule50 >= DTM50_HMC_COUNT;
	if (!w && !rule50_drawn) return r;

	r.status = Probe_Result::Status::OK;
	if (w)
	{
		r.wdl = probe_wdl_internal(w, ps, pos, depth);
		if (r.wdl == WDL_Entry::ILLEGAL) return r;
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
			if (!ps.has_pawns())
				fill_pawnless_dtc_from_dtz(inout_param(r), rule50);
			if (rule50_drawn)
			{
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
				if (fold_50mr_wdl(r.wdl) == WDL_Entry::DRAW)
				{
					r.dtm50_wdl = WDL_Entry::DRAW;
					r.dtm50 = 0;
					r.has_dtm50 = true;
				}
				else if (r.has_dtm && dtm50_layer_pinned_by_dtm(r.wdl, r.dtm, rule50))
				{
					r.dtm50_wdl = r.wdl;
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

		if (!r.has_dtc && ps.has_pawns())
			if (DTC_File* c = open_dtc(ps))
				if (const auto cell = probe_dtc_internal(c, ps, pos, r.wdl, rule50, depth))
				{
					r.has_dtc = true;
					r.dtc_wdl = cell->priced() ? r.wdl : WDL_Entry::DRAW;
					r.dtc_order = cell->priced() ? cell->order : 0;
					r.dtc = cell->priced() ? cell->value : 0;
					ASSERT(cell->dtz != DTC_Cell::DRAWN);
					r.has_dtz = true;
					r.dtz = cell->dtz;
				}

		if (!r.has_dtz)
			if (DTZ_File* d = open_dtz(ps))
			{
				const auto dtz = probe_dtz_internal(d, ps, pos, r.wdl, depth);
				r.has_dtz = dtz.has_value();
				if (dtz) r.dtz = *dtz;
			}

		if (!r.has_dtc && !ps.has_pawns())
			fill_pawnless_dtc_from_dtz(inout_param(r), rule50);
	}

	if (ep_square == SQ_END)
		return r;

	Probe_Result best = r;
	WDL_Entry best_dtz_wdl = r.wdl;
	uint16_t  best_dtz     = r.has_dtz ? static_cast<uint16_t>(r.dtz) : 0;
	WDL_Entry best_dtm_wdl = fold_dtm_wdl(r.wdl);
	uint16_t  best_dtm     = r.has_dtm ? static_cast<uint16_t>(r.dtm) : 0;
	WDL_Entry best_dtm50_wdl = r.has_dtm50 ? r.dtm50_wdl : fold_50mr_wdl(r.wdl);
	uint16_t  best_dtm50     = r.has_dtm50 ? static_cast<uint16_t>(r.dtm50) : 0;
	WDL_Entry best_dtc_wdl   = r.has_dtc ? r.dtc_wdl : r.wdl;
	uint16_t  best_dtc_order = r.has_dtc ? static_cast<uint16_t>(r.dtc_order) : 0;
	uint16_t  best_dtc       = r.has_dtc ? static_cast<uint16_t>(r.dtc) : 0;
	bool any_ep = false;

	const bool unknown = pos.visit_legal_ep_captures(ep_square, [&](Move m) FORCE_INLINE_LAMBDA {
		any_ep = true;
		Child_Pos child = make_child(pos, m);
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
			cr = probe_impl(child.ps, child.pos, 0, SQ_END, depth + 1);
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			return true;

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		if (wdl_rank(my_wdl) > wdl_rank(best.wdl))
			best.wdl = my_wdl;

		const bool ep_outranks_dtz = wdl_rank(my_wdl) > wdl_rank(best_dtz_wdl);
		if (ep_outranks_dtz
			|| (best.has_dtz && prefer_new(my_wdl, 1, best_dtz_wdl, best_dtz)))
		{
			best_dtz_wdl = my_wdl;
			best_dtz = 1;
			best.dtz = (my_wdl == WDL_Entry::DRAW) ? 0 : 1;
			best.has_dtz = true;
		}

		// The ep capture is legal, so a DTM that ignored it would be wrong: if the
		// child cannot supply one, this position's DTM is undetermined.
		if (!cr.has_dtm) best.has_dtm = false;
		else if (best.has_dtm)
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

		if (!cr.has_dtm50) best.has_dtm50 = false;
		else if (best.has_dtm50)
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
		return false;
	});
	if (unknown) return {};
	if (!any_ep) return r;
	return best;
}

WDL_Entry Probe_Tables::Impl::probe_wdl_impl(const Piece_Config& ps, const Position& pos, Square ep_square, int depth)
{
	WDL_Entry best = probe_wdl_internal(open_wdl(ps), ps, pos, depth);
	if (best == WDL_Entry::ILLEGAL || ep_square == SQ_END)
		return best;

	(void)pos.visit_legal_ep_captures(ep_square, [&](Move m) FORCE_INLINE_LAMBDA {
		Child_Pos child = make_child(pos, m);
		const WDL_Entry cw = child.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: probe_wdl_internal(open_wdl(child.ps), child.ps, child.pos, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) return false;

		const WDL_Entry mine = invert_wdl(cw);
		if (wdl_rank(mine) > wdl_rank(best))
			best = mine;
		return false;
	});
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

void specialized_literal_key(const Piece_Config& ps, const Position& pos,
                            Material_Key& out)
{
	Square castling_rooks[COLOR_NB * Castling_Group::MAX_RIGHTS];
	size_t num_castling_rooks = 0;
	Piece_Config::Castling_Rights_Counts rights{ 0, 0 };
	if (ps.has_castling())
		for (const Color c : { WHITE, BLACK })
			for (const bool h_side : { false, true })
				if (pos.can_castle(c, h_side))
				{
					castling_rooks[num_castling_rooks++] = pos.castling_rook_square(c, h_side);
					rights[c] += 1;
				}

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
			if (s == pw || s == pb) continue;
			bool holds_right = false;
			for (size_t i = 0; i < num_castling_rooks; ++i)
				if (castling_rooks[i] == s) { holds_right = true; break; }
			if (holds_right) continue;
			k.add_piece(pc);
		}
	}
	if (ps.has_opposing_pair())
		k.add_pair();
	if (num_castling_rooks > 0)
		k.add_castling(rights[WHITE], rights[BLACK]);
	out = k;
}

std::optional<Piece_Config> specialized_config_from_position(const Position& pos, bool with_pair)
{
	Square pw = SQ_END, pb = SQ_END;
	if (with_pair)
	{
		Square ws[16], bs[16];
		size_t nw = 0, nb = 0;
		Bitboard wb = pos.piece_bb(WHITE_PAWN);
		while (wb) ws[nw++] = wb.pop_first_square();
		Bitboard bb = pos.piece_bb(BLACK_PAWN);
		while (bb) bs[nb++] = bb.pop_first_square();
		if (!Pair_Group::find_canonical(Const_Span<Square>(ws, nw),
		                                Const_Span<Square>(bs, nb), pw, pb))
			return std::nullopt;
	}

	Square castling_rooks[COLOR_NB * Castling_Group::MAX_RIGHTS];
	size_t num_castling_rooks = 0;
	Piece_Config::Castling_Rights_Counts rights{ 0, 0 };
	for (const Color c : { WHITE, BLACK })
		for (const bool h_side : { false, true })
			if (pos.can_castle(c, h_side))
			{
				castling_rooks[num_castling_rooks++] = pos.castling_rook_square(c, h_side);
				rights[c] += 1;
			}

	if (!with_pair && num_castling_rooks == 0) return std::nullopt;

	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	for (Piece pc : ALL_PIECES)
	{
		Bitboard b = pos.piece_bb(pc);
		while (b)
		{
			const Square sq = b.pop_first_square();
			if (sq == pw || sq == pb) continue;
			bool holds_right = false;
			for (size_t i = 0; i < num_castling_rooks; ++i)
				if (castling_rooks[i] == sq) { holds_right = true; break; }
			if (!holds_right) pieces[n++] = pc;
		}
	}

	Piece_Config ps(Const_Span<Piece>(pieces.data(), n), rights);
	if (with_pair) ps.mark_opposing_pair();
	return ps;
}

Canonical_Root canonical_root_from_config(
	const Piece_Config& ps, const Position& input, Square ep_square)
{
	Material_Key literal_key;
	specialized_literal_key(ps, input, literal_key);
	const auto [base_key, mirror_key] = ps.material_keys();
	if (literal_key == base_key)
		return Canonical_Root{ ps, input, ep_square, false };
	ASSERT(literal_key == mirror_key);

	Position pos = input.mirror();
	if (ep_square != SQ_END)
		ep_square = sq_rank_mirror(ep_square);
	return Canonical_Root{ ps, std::move(pos), ep_square, true };
}

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

std::optional<Canonical_Root> Probe_Tables::Impl::route_specialized(
	const Position& pos, Square ep_square)
{
	if (pos.castling() != NO_CASTLING)
	{
		if (std::optional<Piece_Config> both = specialized_config_from_position(pos, true))
		{
			Canonical_Root r = canonical_root_from_config(*both, pos, ep_square);
			if (has_any_table(r.ps))
				return r;
		}
		std::optional<Piece_Config> castling_ps = specialized_config_from_position(pos, false);
		ASSERT(castling_ps.has_value());
		return canonical_root_from_config(*castling_ps, pos, ep_square);
	}

	if (std::optional<Piece_Config> pair_ps = specialized_config_from_position(pos, true))
	{
		Canonical_Root r = canonical_root_from_config(*pair_ps, pos, ep_square);
		if (has_any_table(r.ps))
			return r;
	}
	return std::nullopt;
}

Child_Pos Probe_Tables::Impl::make_child(const Position& parent, Move m)
{
	const bool is_pawn = piece_type(parent.piece_at(m.from())) == PAWN;
	const bool zeroing = is_pawn || parent.move_is_capture(m);
	const Square raw_ep = (is_pawn && is_pawn_double_push(m)) ? ep_square_of_double_push(m) : SQ_END;
	Position pos = parent;
	(void)pos.do_move(m);

	if (std::optional<Canonical_Root> r = route_specialized(pos, raw_ep))
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
	const std::optional<Canonical_Root> routed = m_impl->route_specialized(pos, ep_square);
	const Canonical_Root root = routed ? *routed : canonical_root_from_position(pos, ep_square);
	return m_impl->probe_impl(root.ps, root.pos, rule50, root.ep_square, 0);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, unsigned rule50)
{
	return probe(ps, pos, SQ_END, rule50);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	if (const std::optional<Canonical_Root> routed = m_impl->route_specialized(pos, ep_square))
		return m_impl->probe_impl(routed->ps, routed->pos, rule50, routed->ep_square, 0);
	const Canonical_Root root = canonical_root_from_config(ps, pos, ep_square);
	return m_impl->probe_impl(root.ps, root.pos, rule50, root.ep_square, 0);
}

WDL_Entry Probe_Tables::probe_wdl(const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	const std::optional<Canonical_Root> routed = m_impl->route_specialized(pos, ep_square);
	const Canonical_Root root = routed ? *routed : canonical_root_from_position(pos, ep_square);
	return m_impl->probe_wdl_impl(root.ps, root.pos, root.ep_square, 0);
}

WDL_Entry Probe_Tables::probe_wdl(
	const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	if (const std::optional<Canonical_Root> routed = m_impl->route_specialized(pos, ep_square))
		return m_impl->probe_wdl_impl(routed->ps, routed->pos, routed->ep_square, 0);
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
	if (m.is_castling())
		return Move::make_castling(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
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

	const int bound = use_rule50 ? 900 : 1;

	auto visit = [&](Move m) FORCE_INLINE_LAMBDA {
		Child_Pos c = m_impl->make_child(probe_pos, m);
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
			return true;

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		int v;
		if (c.is_zeroing)
		{
			v = zeroing_signed_dtz(my_wdl);
		}
		else
		{
			if (!cr.has_dtz) return true;
			if (cr.wdl == WDL_Entry::LOSE && cr.dtz == 0)
			{
				ASSERT(c.pos.is_checkmate(c.ep));
				v = 1;
			}
			else
			{
				v = -signed_dtz_of(cr);
				if (v > 0) ++v;
				else if (v < 0) --v;
			}
		}

		int rank = fathom_dtz_rank(v, rule50, has_repeated);

		const bool conversion = m.is_promotion() || probe_pos.move_is_capture(m);
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
		return false;
	};

	// `||` short-circuits, so a stop in the main scan skips the ep scan.
	if (probe_pos.visit_legal_moves(visit)
	    || probe_pos.visit_legal_ep_captures(root.ep_square, visit))
		return {};

	std::sort(out.begin(), out.end(),
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


	auto visit = [&](Move m) FORCE_INLINE_LAMBDA {
		Child_Pos c = m_impl->make_child(probe_pos, m);
		const WDL_Entry cw = c.ps.is_bare_kings()
			? WDL_Entry::DRAW
			: m_impl->probe_wdl_impl(c.ps, c.pos, c.ep, 0);
		if (cw == WDL_Entry::ILLEGAL)
			return true;

		const WDL_Entry my_wdl = invert_wdl(cw);
		int v = wdl_to_v(my_wdl);
		if (!use_rule50) v = v > 0 ? 2 : v < 0 ? -2 : 0;

		Root_Move r{root.mirrored ? rank_mirror_move(m) : m, my_wdl, 0, WdlToRank[v + 2], WdlToValue[v + 2]};
		out.push_back(r);
		return false;
	};

	// `||` short-circuits, so a stop in the main scan skips the ep scan.
	if (probe_pos.visit_legal_moves(visit)
	    || probe_pos.visit_legal_ep_captures(root.ep_square, visit))
		return {};

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


	auto visit = [&](Move m) FORCE_INLINE_LAMBDA {
		Child_Pos c = m_impl->make_child(probe_pos, m);
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
		if (cr.status != Probe_Result::Status::OK || !cr.has_dtm
		    || cr.wdl == WDL_Entry::ILLEGAL)
			return true;
		if (use_rule50 && (!cr.has_dtm50 || cr.dtm50_wdl == WDL_Entry::ILLEGAL))
			return true;

		const WDL_Entry flat_wdl = invert_wdl(cr.wdl);
		const int       flat_d   = static_cast<int>(cr.dtm) + 1;

		WDL_Entry report_wdl = WDL_Entry::DRAW;
		int v = 0, rank = 0, score = 0;

		if (!use_rule50)
		{
			if (flat_wdl == WDL_Entry::WIN || flat_wdl == WDL_Entry::CURSED_WIN)
			{
				report_wdl = WDL_Entry::WIN;
				v = flat_d;
				score = rank = TB_VALUE_MATE - v;
			}
			else if (flat_wdl == WDL_Entry::LOSE || flat_wdl == WDL_Entry::BLESSED_LOSS)
			{
				report_wdl = WDL_Entry::LOSE;
				v = -flat_d;
				score = rank = -TB_VALUE_MATE + flat_d;
			}
			// DRAW: 0/0/0.
		}
		else
		{
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
				report_wdl = WDL_Entry::CURSED_WIN;
				v = flat_d;
				rank  = std::max(1, 899 - flat_d);
				score = TB_VALUE_DRAW + 2;
			}
			else if (flat_wdl == WDL_Entry::LOSE || flat_wdl == WDL_Entry::BLESSED_LOSS)
			{
				report_wdl = WDL_Entry::BLESSED_LOSS;
				v = -flat_d;
				rank  = std::min(-1, flat_d - 899);
				score = TB_VALUE_DRAW - 2;
			}
		}

		out.push_back(Root_Move{root.mirrored ? rank_mirror_move(m) : m, report_wdl, v, rank, score});
		return false;
	};

	// `||` short-circuits, so a stop in the main scan skips the ep scan.
	if (probe_pos.visit_legal_moves(visit)
	    || probe_pos.visit_legal_ep_captures(root.ep_square, visit))
		return {};

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}
