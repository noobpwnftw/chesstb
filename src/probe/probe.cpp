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

// Sentinel for root probers using DTZ-only ordering; public probe() defaults
// rule50 to 0.
constexpr unsigned SKIP_DTM50 = ~0u;

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

struct Child_Pos { Position pos; Piece_Config ps; bool is_kk; bool is_zeroing; };

struct DTM50_Result
{
	WDL_Entry wdl = WDL_Entry::ILLEGAL;
	uint16_t dtm = 0;
};

Child_Pos make_child(const Position& parent, Move m)
{
	const bool zeroing = move_is_zeroing(parent, m);
	Position pos = parent;
	(void)pos.do_move(m);

	auto [cps, lit] = piece_config_and_literal_key_from_position(pos);
	if (lit != cps.base_material_key())
		pos = mirror_for_canonical(pos);

	const bool is_kk = (cps.num_pieces() <= 2);
	return Child_Pos{std::move(pos), std::move(cps), is_kk, zeroing};
}

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
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
		if (pos.is_pseudo_legal_move_legal(ml[i])) return true;
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
		p.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
		for (size_t i = 0; i < ml.size(); ++i)
		{
			if (!p.is_pseudo_legal_move_legal(ml[i])) continue;
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

}  // namespace

struct Probe_Tables::Impl
{
	const uint64_t epoch = next_epoch();
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

	NODISCARD const Position_Index_Config& get_epsi(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Cache<const Position_Index_Config> tl;
		const Position_Index_Config* hit;
		if (tl.lookup(epoch, k, hit)) return *hit;
		{
			std::shared_lock rlk(epsi_mu);
			auto it = epsi_cache.find(k);
			if (it != epsi_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return *it->second; }
		}
		auto e = std::make_unique<Position_Index_Config>(ps);
		std::unique_lock wlk(epsi_mu);
		auto [it, inserted] = epsi_cache.try_emplace(k, std::move(e));
		const Position_Index_Config* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return *raw;
	}

	NODISCARD WDL_File* open_wdl(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Cache<WDL_File> tl;
		WDL_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(wdl_mu);
			auto it = wdl_cache.find(k);
			if (it != wdl_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<WDL_File> f;
		if (find_in_dirs(ps, WDL_EXT, wdl_dirs, &path))
		{
			f = std::make_unique<WDL_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(wdl_mu);
		auto [it, inserted] = wdl_cache.try_emplace(k, std::move(f));
		WDL_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTC_File* open_dtc(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Cache<DTC_File> tl;
		DTC_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtc_mu);
			auto it = dtc_cache.find(k);
			if (it != dtc_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTC_File> f;
		if (find_in_dirs(ps, DTC_EXT, dtc_dirs, &path))
		{
			f = std::make_unique<DTC_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(dtc_mu);
		auto [it, inserted] = dtc_cache.try_emplace(k, std::move(f));
		DTC_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTM_File* open_dtm(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Cache<DTM_File> tl;
		DTM_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtm_mu);
			auto it = dtm_cache.find(k);
			if (it != dtm_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTM_File> f;
		if (find_in_dirs(ps, DTM_EXT, dtm_dirs, &path))
		{
			f = std::make_unique<DTM_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(dtm_mu);
		auto [it, inserted] = dtm_cache.try_emplace(k, std::move(f));
		DTM_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTM50_File* open_dtm50(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Cache<DTM50_File> tl;
		DTM50_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtm50_mu);
			auto it = dtm50_cache.find(k);
			if (it != dtm50_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTM50_File> f;
		if (find_in_dirs(ps, DTM50_EXT, dtm50_dirs, &path))
		{
			f = std::make_unique<DTM50_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(dtm50_mu);
		auto [it, inserted] = dtm50_cache.try_emplace(k, std::move(f));
		DTM50_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD Probe_Result probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth);
	NODISCARD WDL_Entry probe_wdl_internal(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> probe_dtc_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD std::optional<uint16_t> probe_dtm_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM50_Result probe_dtm50_internal(const Piece_Config& ps, const Position& pos,
	                                            WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD WDL_Entry derive_wdl(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> derive_dtc(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD std::optional<uint16_t> derive_dtm(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD DTM50_Result derive_dtm50(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth);

	Probe_Result apply_ep_overlay(const Position& root, const Probe_Result& no_ep, Square ep_square);

	void scan_paths();
};

namespace {

// Symmetric materials store only the WHITE-to-move frame.
Position mirror_symmetric_black_stm(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
	{
		const Piece p = pos.piece_at(sq);
		if (p != PIECE_NONE)
			swapped.put_piece(piece_opp_color(p), sq_rank_mirror(sq));
	}
	swapped.set_turn(WHITE);
	return swapped;
}

struct Canonical_Root
{
	Piece_Config ps;
	Position pos;
	Square ep_square = SQ_END;
	bool mirrored = false;
};

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

std::optional<Canonical_Root> canonical_root_from_config(
	const Piece_Config& ps, const Position& input, Square ep_square)
{
	const Material_Key literal_key = input.material_key();
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

Move rank_mirror_move(Move m)
{
	if (m.is_ep_capture())
		return Move::make_ep_capture(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
	if (m.is_promotion())
		return Move::make_promotion(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()), m.promotion());
	return Move::make_quiet(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
}

}  // namespace

WDL_Entry Probe_Tables::Impl::probe_wdl_internal(const Piece_Config& ps, const Position& pos, int depth)
{
	WDL_File* w = open_wdl(ps);
	if (!w) return WDL_Entry::ILLEGAL;

	const Position_Index_Config& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return WDL_Entry::ILLEGAL;

	const Color stm = pos.turn();
	return w->is_dropped[stm]
		? derive_wdl(ps, pos, depth)
		: w->read(stm, idx);
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtc_internal(
	const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	DTC_File* d = open_dtc(ps);
	if (!d) return std::nullopt;

	const Position_Index_Config& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return std::nullopt;

	const Color stm = pos.turn();
	return d->is_dropped[stm]
		? derive_dtc(ps, pos, depth)
		: d->read(stm, idx, wdl);
}

std::optional<uint16_t> Probe_Tables::Impl::probe_dtm_internal(
	const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	DTM_File* d = open_dtm(ps);
	if (!d) return std::nullopt;

	const Position_Index_Config& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return std::nullopt;

	const Color stm = pos.turn();
	return d->is_dropped[stm]
		? derive_dtm(ps, pos, depth)
		: d->read(stm, idx, wdl);
}

WDL_Entry Probe_Tables::Impl::derive_wdl(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best = WDL_Entry::LOSE;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		const WDL_Entry cw = c.is_kk
			? WDL_Entry::DRAW
			: probe_wdl_internal(c.ps, c.pos, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) continue;

		const WDL_Entry mw = invert_wdl(cw);
		if (wdl_rank(mw) > wdl_rank(best)) best = mw;
		have_candidate = true;
	}

	if (!any_legal) return pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	if (!have_candidate) return WDL_Entry::ILLEGAL;
	return best;
}

std::optional<uint16_t> Probe_Tables::Impl::derive_dtc(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtc = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
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
			cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			const auto child_dtc = probe_dtc_internal(c.ps, c.pos, cw, depth + 1);
			if (!child_dtc) continue;
			cd = *child_dtc;
		}

		WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtc = c.is_zeroing
			? uint16_t{1}
			: static_cast<uint16_t>(1u + cd);
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

std::optional<uint16_t> Probe_Tables::Impl::derive_dtm(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return std::nullopt;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
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
			cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			const auto child_dtm = probe_dtm_internal(c.ps, c.pos, cw, depth + 1);
			if (!child_dtm) continue;
			cd = *child_dtm;
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

Probe_Result Probe_Tables::Impl::probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth)
{
	if (pos.turn() == BLACK && is_symmetric_material(ps))
		return probe_impl(ps, mirror_symmetric_black_stm(pos), rule50, depth);

	Probe_Result r;
	WDL_File* w = open_wdl(ps);
	DTC_File* d = open_dtc(ps);
	DTM_File* m = open_dtm(ps);
	// rule50 ≥ 100: auto-50MR DRAW, no DTM50 layer to read.
	const bool rule50_drawn = rule50 != SKIP_DTM50 && rule50 >= DTM50_HMC_COUNT;
	DTM50_File* m50 = nullptr;
	if (rule50 != SKIP_DTM50 && !rule50_drawn)
		m50 = open_dtm50(ps);
	if (!w && !d && !m && !m50 && !rule50_drawn) return r;

	const Position_Index_Config& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE)
	{
		r.status = Probe_Result::Status::ILLEGAL_POS;
		return r;
	}
	if (!pos.is_legal())
	{
		r.status = Probe_Result::Status::ILLEGAL_POS;
		return r;
	}

	r.status = Probe_Result::Status::OK;
	if (w) r.wdl = probe_wdl_internal(ps, pos, depth);
	if (d && w)
	{
		const auto dtc = probe_dtc_internal(ps, pos, r.wdl, depth);
		r.has_dtc = dtc.has_value();
		if (dtc) r.dtc = *dtc;
	}
	if (m && w)
	{
		const auto dtm = probe_dtm_internal(ps, pos, r.wdl, depth);
		r.has_dtm = dtm.has_value();
		if (dtm) r.dtm = *dtm;
	}
	if (rule50_drawn)
	{
		const bool mated = r.wdl == WDL_Entry::LOSE && is_checkmate(pos);
		r.dtm50_wdl = mated ? WDL_Entry::LOSE : WDL_Entry::DRAW;
		r.dtm50 = 0;
		r.has_dtm50 = true;
	}
	else if (m50 && w)
	{
		const DTM50_Result d50 = probe_dtm50_internal(ps, pos, r.wdl, rule50, depth);
		r.dtm50_wdl = d50.wdl;
		r.dtm50 = d50.dtm;
		r.has_dtm50 = d50.wdl != WDL_Entry::ILLEGAL;
	}
	return r;
}

Probe_Result Probe_Tables::Impl::apply_ep_overlay(const Position& root,
                                                   const Probe_Result& no_ep,
                                                   Square ep_square)
{
	if (no_ep.status != Probe_Result::Status::OK || ep_square == SQ_END)
		return no_ep;

	Move_List eps;
	add_ep_moves(root, ep_square, eps);
	if (eps.empty()) return no_ep;

	Probe_Result best = no_ep;
	WDL_Entry best_dtc_wdl = no_ep.wdl;
	uint16_t  best_dtc     = no_ep.has_dtc ? static_cast<uint16_t>(no_ep.dtc) : 0;
	WDL_Entry best_dtm_wdl = fold_dtm_wdl(no_ep.wdl);
	uint16_t  best_dtm     = no_ep.has_dtm ? static_cast<uint16_t>(no_ep.dtm) : 0;
	WDL_Entry best_dtm50_wdl = no_ep.has_dtm50 ? no_ep.dtm50_wdl : fold_dtm50_wdl(no_ep.wdl);
	uint16_t  best_dtm50     = no_ep.has_dtm50 ? static_cast<uint16_t>(no_ep.dtm50) : 0;

	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!root.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(root, eps[i]);
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
			cr = probe_impl(child.ps, child.pos, 0, 0);  // EP is zeroing
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			continue;

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		if (wdl_rank(my_wdl) > wdl_rank(best.wdl))
			best.wdl = my_wdl;

		if (best.has_dtc && cr.has_dtc)
		{
			const uint16_t my_dtc = 1;
			if (prefer_new(my_wdl, my_dtc, best_dtc_wdl, best_dtc))
			{
				best_dtc_wdl = my_wdl;
				best_dtc = my_dtc;
				best.dtc = my_dtc;
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

namespace {

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

void Probe_Tables::rescan() { m_impl->scan_paths(); }

Probe_Result Probe_Tables::probe(const Position& pos, unsigned rule50)
{
	const Canonical_Root root = canonical_root_from_position(pos, SQ_END);
	return m_impl->probe_impl(root.ps, root.pos, rule50, 0);
}

Probe_Result Probe_Tables::probe(const Position& pos, Square ep_square, unsigned rule50)
{
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Probe_Result base = m_impl->probe_impl(root.ps, root.pos, rule50, 0);
	return m_impl->apply_ep_overlay(root.pos, base, root.ep_square);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, unsigned rule50)
{
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, SQ_END);
	if (!root) return illegal_probe_result();
	return m_impl->probe_impl(root->ps, root->pos, rule50, 0);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, ep_square);
	if (!root) return illegal_probe_result();
	const Probe_Result base = m_impl->probe_impl(root->ps, root->pos, rule50, 0);
	return m_impl->apply_ep_overlay(root->pos, base, root->ep_square);
}

WDL_Entry Probe_Tables::probe_wdl(const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	const Probe_Result r = probe(pos, ep_square);
	return r.status == Probe_Result::Status::OK ? r.wdl : WDL_Entry::ILLEGAL;
}

DTM50_Result Probe_Tables::Impl::probe_dtm50_internal(
	const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	if (rule50 >= DTM50_HMC_COUNT)
		return { WDL_Entry::DRAW, 0 };
	const uint16_t hmc = static_cast<uint16_t>(rule50);
	DTM50_File* m = open_dtm50(ps);
	if (!m) return {};

	const Position_Index_Config& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return {};

	const Color stm = pos.turn();
	const DTM50_Result d = m->is_dropped[stm]
		? derive_dtm50(ps, pos, rule50, depth)
		: DTM50_Result{ fold_dtm50_wdl(wdl), m->read(stm, idx, wdl, hmc) };

	// See recover_mate_at_hmc.
	if (hmc > 0 && d.dtm == 0 && (wdl == WDL_Entry::WIN || wdl == WDL_Entry::LOSE))
		return recover_mate_at_hmc(pos, wdl);
	return d;
}

// rule50-aware derive: per-child hmc (zeroing resets, quiet increments);
// once ≥100, the move is DRAW unless it mates.
DTM50_Result Probe_Tables::Impl::derive_dtm50(
	const Piece_Config& ps, const Position& pos, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return {};

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
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
			const WDL_Entry cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			cd = probe_dtm50_internal(c.ps, c.pos, cw, child_rule50, depth + 1);
			if (cd.wdl == WDL_Entry::ILLEGAL) continue;
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
		return pos.is_in_check()
			? DTM50_Result{ WDL_Entry::LOSE, 0 }
			: DTM50_Result{ WDL_Entry::DRAW, 0 };
	if (!have_candidate)
		return {};

	if (best_wdl == WDL_Entry::WIN || best_wdl == WDL_Entry::LOSE)
		return { best_wdl, best_dtm };
	return { WDL_Entry::DRAW, 0 };
}

namespace {

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
	probe_pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);

	const int bound = use_rule50 ? 900 : 1;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m)) continue;

		Child_Pos c = make_child(probe_pos, m);
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
			cr = m_impl->probe_impl(c.ps, c.pos, SKIP_DTM50, 0);
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
			c.pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(cml));
			bool any = false;
			for (size_t j = 0; j < cml.size(); ++j)
			{
				if (c.pos.is_pseudo_legal_move_legal(cml[j]))
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
	probe_pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m)) continue;

		Child_Pos c = make_child(probe_pos, m);
		Probe_Result cr;
		if (c.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, SKIP_DTM50, 0);
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			return {};

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		int v = wdl_to_v(my_wdl);
		if (!use_rule50) v = v > 0 ? 2 : v < 0 ? -2 : 0;

		Root_Move r{root.mirrored ? rank_mirror_move(m) : m, my_wdl, 0, WdlToRank[v + 2], WdlToValue[v + 2]};
		out.push_back(r);
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}
