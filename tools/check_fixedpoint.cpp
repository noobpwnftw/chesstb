// Recompute each table value from legal children and compare it to disk.
// Full tables only: dropped, loss-only and relaxed frames are rejected, in the
// material under test and in every sub-table it reads, as are missing
// sub-tables.
//
//   ./check_fixedpoint -r KRRK
//   ./check_fixedpoint --check-dtz -r KRRK,KQK
//   ./check_fixedpoint --check-dtm --list five.txt
//   ./check_fixedpoint --enumerate 5 --wdl ./wdl --dtz ./dtz --dtm ./dtm

#include "chess/castling_group.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "egtb/piece_config_closure.h"

#include "probe/probe.h"
#include "probe/position_index.h"
#include "probe/table_files.h"

#include "util/cache.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/progress_bar.h"
#include "util/thread_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* wdl_name(WDL_Entry w)
{
	switch (w) {
		case WDL_Entry::ILLEGAL:      return "ILLEGAL";
		case WDL_Entry::LOSE:         return "LOSE";
		case WDL_Entry::BLESSED_LOSS: return "BLESSED_LOSS";
		case WDL_Entry::DRAW:         return "DRAW";
		case WDL_Entry::CURSED_WIN:   return "CURSED_WIN";
		case WDL_Entry::WIN:          return "WIN";
	}
	return "?";
}

bool is_win_class(WDL_Entry w)
{
	return w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN;
}

bool is_loss_class(WDL_Entry w)
{
	return w == WDL_Entry::LOSE || w == WDL_Entry::BLESSED_LOSS;
}

WDL_Entry fold_dtm_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN) return WDL_Entry::WIN;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::LOSE;
	return w;
}

WDL_Entry invert_wdl(WDL_Entry w)
{
	switch (w) {
		case WDL_Entry::WIN:          return WDL_Entry::LOSE;
		case WDL_Entry::CURSED_WIN:   return WDL_Entry::BLESSED_LOSS;
		case WDL_Entry::DRAW:         return WDL_Entry::DRAW;
		case WDL_Entry::BLESSED_LOSS: return WDL_Entry::CURSED_WIN;
		case WDL_Entry::LOSE:         return WDL_Entry::WIN;
		case WDL_Entry::ILLEGAL:      return WDL_Entry::ILLEGAL;
	}
	return WDL_Entry::ILLEGAL;
}

int wdl_rank(WDL_Entry w)
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

bool prefer_new(WDL_Entry nw, uint16_t nv, WDL_Entry ow, uint16_t ov)
{
	const int nr = wdl_rank(nw), orank = wdl_rank(ow);
	if (nr != orank) return nr > orank;
	if (is_win_class(nw)) return nv < ov;
	if (is_loss_class(nw)) return nv > ov;
	return false;
}

struct Config_And_Literal_Key {
	Piece_Config cfg;
	Material_Key literal_key;
};

Config_And_Literal_Key piece_config_and_literal_key_from_position_local(const Position& pos)
{
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

	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	Material_Key literal_key;
	for (Piece pc : ALL_PIECES)
	{
		Bitboard b = pos.piece_bb(pc);
		while (b)
		{
			const Square sq = b.pop_first_square();
			bool holds_right = false;
			for (size_t i = 0; i < num_castling_rooks; ++i)
				if (castling_rooks[i] == sq) { holds_right = true; break; }
			if (holds_right) continue;
			pieces[n++] = pc;
			literal_key.add_piece(pc);
		}
	}
	if (num_castling_rooks > 0)
		literal_key.add_castling(rights[WHITE], rights[BLACK]);

	return { Piece_Config(Const_Span<Piece>(pieces.data(), n), rights), literal_key };
}

struct Child_Pos {
	Position pos;
	Piece_Config ps;
	Square ep = SQ_END;
	bool is_zeroing = false;
};

Child_Pos make_child(const Position& parent, Move m)
{
	const bool is_pawn = piece_type(parent.piece_at(m.from())) == PAWN;
	const bool zeroing = is_pawn || parent.move_is_capture(m);
	Square ep = (is_pawn && is_pawn_double_push(m)) ? ep_square_of_double_push(m) : SQ_END;
	Position pos = parent;
	(void)pos.do_move(m);

	auto [cps, lit] = piece_config_and_literal_key_from_position_local(pos);
	if (lit != cps.base_material_key())
	{
		pos = pos.mirror();
		if (ep != SQ_END) ep = sq_rank_mirror(ep);
	}

	return { std::move(pos), std::move(cps), ep, zeroing };
}

struct DTM_Derived {
	WDL_Entry wdl = WDL_Entry::DRAW;  // folded WIN/DRAW/LOSE value
	uint16_t value = 0;
	bool missing_child = false;
};

struct Child_DTM {
	WDL_Entry wdl = WDL_Entry::DRAW;  // child side to move, folded
	uint16_t value = 0;
	bool missing = false;
};

Child_DTM probe_child_dtm(Probe_Tables& tables, const Child_Pos& c)
{
	if (c.ps.is_bare_kings()) return {};
	const Probe_Result pr = tables.probe(c.ps, c.pos, IGNORE_50MR);
	if (pr.status != Probe_Result::Status::OK || !pr.has_dtm)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { fold_dtm_wdl(pr.wdl), static_cast<uint16_t>(pr.dtm), false };
}

Child_DTM effective_child_dtm_after_move(Probe_Tables& tables, const Position& parent, Move m)
{
	const Child_Pos child = make_child(parent, m);
	Child_DTM best = probe_child_dtm(tables, child);
	if (child.ep == SQ_END) return best;

	(void)child.pos.visit_legal_ep_captures(child.ep, [&](Move ep) FORCE_INLINE_LAMBDA {
		const Child_DTM after_ep = probe_child_dtm(tables, make_child(child.pos, ep));
		if (after_ep.missing)
		{
			best.missing = true;
			return false;
		}
		const Child_DTM opp_choice{
			invert_wdl(after_ep.wdl),
			static_cast<uint16_t>(after_ep.value + 1),
			false
		};
		if (best.missing || prefer_new(opp_choice.wdl, opp_choice.value, best.wdl, best.value))
			best = opp_choice;
		return false;
	});
	return best;
}

DTM_Derived derive_dtm_from_children(Probe_Tables& tables, const Position& pos)
{
	bool any_legal = false;
	bool have = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best = 0;
	bool missing = false;

	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const Child_DTM child = effective_child_dtm_after_move(tables, pos, m);
		if (child.missing)
		{
			missing = true;
			return false;
		}

		const WDL_Entry my_w = invert_wdl(child.wdl);
		const uint16_t my_v = static_cast<uint16_t>(child.value + 1);
		if (!have || prefer_new(my_w, my_v, best_wdl, best))
		{
			best_wdl = my_w;
			best = my_v;
			have = true;
		}
		return false;
	});

	if (!any_legal)
		return { pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW, 0, missing };
	if (!have)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { best_wdl, best, missing };
}

struct DTZ_Derived {
	WDL_Entry wdl = WDL_Entry::DRAW;
	uint16_t value = 0;
	bool missing_child = false;
};

struct Child_DTZ {
	WDL_Entry wdl = WDL_Entry::DRAW;  // child side to move
	uint16_t value = 0;
	bool missing = false;
};

Child_DTZ probe_child_dtz(Probe_Tables& tables, const Child_Pos& c)
{
	if (c.ps.is_bare_kings()) return {};
	const Probe_Result pr = tables.probe(c.ps, c.pos, IGNORE_50MR);
	if (pr.status != Probe_Result::Status::OK || !pr.has_dtz)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { pr.wdl, static_cast<uint16_t>(pr.dtz), false };
}

Child_DTZ effective_child_dtz_after_move(Probe_Tables& tables, const Position& parent, Move m)
{
	const Child_Pos child = make_child(parent, m);
	Child_DTZ best = probe_child_dtz(tables, child);
	if (child.ep == SQ_END) return best;

	(void)child.pos.visit_legal_ep_captures(child.ep, [&](Move ep) FORCE_INLINE_LAMBDA {
		const Child_DTZ after_ep = probe_child_dtz(tables, make_child(child.pos, ep));
		if (after_ep.missing)
		{
			best.missing = true;
			return false;
		}
		const Child_DTZ opp_choice{ invert_wdl(after_ep.wdl), uint16_t{1}, false };
		if (best.missing || prefer_new(opp_choice.wdl, opp_choice.value, best.wdl, best.value))
			best = opp_choice;
		return false;
	});
	return best;
}

DTZ_Derived derive_dtz_from_children(Probe_Tables& tables, const Position& pos)
{
	bool any_legal = false;
	bool have = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best = 0;
	bool missing = false;

	(void)pos.visit_legal_moves([&](Move m) FORCE_INLINE_LAMBDA {
		any_legal = true;

		const Child_DTZ child = effective_child_dtz_after_move(tables, pos, m);
		if (child.missing)
		{
			missing = true;
			return false;
		}

		WDL_Entry my_w = invert_wdl(child.wdl);
		const bool zeroing = piece_type(pos.piece_at(m.from())) == PAWN || pos.move_is_capture(m);
		const uint16_t my_v = zeroing ? uint16_t{1} : static_cast<uint16_t>(child.value + 1);
		if (my_v > DTZ_MAX_NON_CURSED)
		{
			if (my_w == WDL_Entry::WIN) my_w = WDL_Entry::CURSED_WIN;
			if (my_w == WDL_Entry::LOSE) my_w = WDL_Entry::BLESSED_LOSS;
		}

		if (!have || prefer_new(my_w, my_v, best_wdl, best))
		{
			best_wdl = my_w;
			best = my_v;
			have = true;
		}
		return false;
	});

	if (!any_legal)
		return { pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW, 0, missing };
	if (!have)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { best_wdl, best, missing };
}

size_t g_num_threads = std::max(1u, std::thread::hardware_concurrency());

Thread_Pool& global_pool()
{
	static Thread_Pool pool(g_num_threads);
	return pool;
}

std::string fen_of(const Position& pos)
{
	char fen[128] = {};
	pos.to_fen(Span(fen, sizeof(fen)));
	return std::string(fen);
}

struct Options
{
	std::string wdl_dir = "./wdl/";
	std::string dtz_dir = "./dtz/";
	std::string dtm_dir = "./dtm/";
	std::string dtm50_dir = "./dtm50/";
	bool check_dtz = true;
	bool check_dtm = true;
	bool cursed_only = false;
	size_t sample_cap = 20;
	size_t cache_mib = DEFAULT_BLOCK_CACHE_BYTES / (1024 * 1024);
};

struct Shard
{
	size_t scanned = 0;
	size_t checked_dtz = 0;
	size_t checked_dtm = 0;
	size_t dtz_mismatch = 0;
	size_t dtm_mismatch = 0;
	size_t missing = 0;
	std::vector<std::string> samples;
};

void push_sample(Shard& s, size_t cap, const std::string& line)
{
	if (s.samples.size() < cap) s.samples.push_back(line);
}

std::filesystem::path table_path(const std::string& dir, const Piece_Config& ps, const char* ext)
{
	return std::filesystem::path(dir) / (ps.name() + ext);
}

enum struct File_State { ABSENT, NOT_FULL, FULL };

template <typename File>
File_State load_full_format(File& f, const Piece_Config& ps,
                            const std::filesystem::path& path, const std::string& label)
{
	if (!std::filesystem::exists(path)) return File_State::ABSENT;

	try
	{
		f.load(ps, path);
	}
	catch (const std::exception& e)
	{
		std::printf("  %s unreadable: %s\n", label.c_str(), e.what());
		return File_State::NOT_FULL;
	}

	// Symmetric material stores WHITE only; load() marks BLACK dropped.
	const auto [mat_key, mir_key] = ps.material_keys();
	for (Color c : egtb_table_colors(mat_key == mir_key ? 1 : 2))
	{
		const char* kind = f.is_dropped[c]   ? "dropped"
		                 : f.is_loss_only[c] ? "loss-only"
		                 : f.is_relaxed[c]   ? "relaxed"
		                 : nullptr;
		if (kind == nullptr) continue;
		std::printf("  %s is not full format (%s %s-to-move frame): %s\n",
			label.c_str(), kind, c == WHITE ? "white" : "black", path.c_str());
		return File_State::NOT_FULL;
	}
	return File_State::FULL;
}

template <typename File>
bool require_full_file(File& f, const Piece_Config& ps,
                       const std::filesystem::path& path, const std::string& label)
{
	const File_State state = load_full_format(f, ps, path, label);
	if (state == File_State::ABSENT)
		std::printf("  %s missing: %s\n", label.c_str(), path.c_str());
	return state == File_State::FULL;
}

// Either the standalone table or the pack's layer 0 will do; a shrunk one is
// still refused.
bool require_full_dtm(const Options& opt, const Piece_Config& ps, const std::string& prefix)
{
	const std::filesystem::path dtm_path = table_path(opt.dtm_dir, ps, DTM_EXT);
	const std::filesystem::path dtm50_path = table_path(opt.dtm50_dir, ps, DTM50_EXT);

	DTM_File dtm_file;
	DTM50_File dtm50_file;
	const File_State dtm   = load_full_format(dtm_file, ps, dtm_path, prefix + "DTM");
	const File_State dtm50 = load_full_format(dtm50_file, ps, dtm50_path, prefix + "DTM50");
	if (dtm == File_State::NOT_FULL || dtm50 == File_State::NOT_FULL) return false;
	if (dtm != File_State::FULL && dtm50 != File_State::FULL)
	{
		std::printf("  %sDTM missing: %s (and %s)\n",
			prefix.c_str(), dtm_path.c_str(), dtm50_path.c_str());
		return false;
	}
	return true;
}

// Mirrors EGTB_Generator::enumerate_sub_materials.
Unique_Piece_Configs one_move_sub_configs(const Piece_Config& ps)
{
	Unique_Piece_Configs out;

	for (const auto& [_captured, sub] : sub_configs_by_capture(ps))
		out.add_unique(sub);
	if (ps.has_opposing_pair())
		for (const Color survivor : { WHITE, BLACK })
			out.add_unique(pair_broken_survivor(ps, survivor));

	for (size_t i = 0; i < ps.num_pieces(); ++i)
	{
		const Piece pawn = ps.pieces()[i];
		if (piece_type(pawn) != PAWN) continue;
		const Color mover = piece_color(pawn);

		for (const Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			out.add_unique(with_replaced_piece(ps, i, piece_make(mover, promo)));

		for (size_t j = 0; j < ps.num_pieces(); ++j)
		{
			const Piece victim = ps.pieces()[j];
			if (j == i || piece_color(victim) == mover || piece_type(victim) == KING)
				continue;

			std::vector<Piece> pieces;
			pieces.reserve(ps.num_pieces() + 1);
			for (size_t k = 0; k < ps.num_pieces(); ++k)
				if (k != j) pieces.push_back(ps.pieces()[k]);
			if (ps.has_opposing_pair())
			{
				pieces.push_back(WHITE_PAWN);
				pieces.push_back(BLACK_PAWN);
			}
			for (const Piece_Type promo : { QUEEN, ROOK, BISHOP, KNIGHT })
			{
				pieces[i < j ? i : i - 1] = piece_make(mover, promo);
				out.add_unique(Piece_Config(Const_Span<Piece>(pieces.data(), pieces.size())));
			}
		}
	}

	out.remove_if([](const Piece_Config& sub) { return sub.is_bare_kings(); });
	return out;
}

bool require_full_sub_tables(const Options& opt, const Piece_Config& ps)
{
	bool ok = true;
	for (const Piece_Config& sub : one_move_sub_configs(ps))
	{
		const std::string prefix = "sub-table " + sub.name() + " ";

		WDL_File wdl;
		if (!require_full_file(wdl, sub, table_path(opt.wdl_dir, sub, WDL_EXT), prefix + "WDL"))
			ok = false;

		if (opt.check_dtz)
		{
			DTZ_File dtz;
			if (!require_full_file(dtz, sub, table_path(opt.dtz_dir, sub, DTZ_EXT), prefix + "DTZ"))
				ok = false;
		}
		if (opt.check_dtm && !require_full_dtm(opt, sub, prefix))
			ok = false;
	}
	return ok;
}

bool dtz_values_match(WDL_Entry w, uint16_t actual, uint16_t derived, size_t entry_bytes)
{
	if (w == WDL_Entry::DRAW) return true;
	if (entry_bytes == 1 && (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS))
	{
		// One-byte cursed encoding can shift the stored value by one.
		const int a = static_cast<int>(actual);
		const int d = static_cast<int>(derived);
		return std::abs(a - d) <= 1;
	}
	return actual == derived;
}

bool check_material(const Options& opt, const std::string& name)
{
	if (!Piece_Config::is_constructible_from(name)) {
		std::printf("%-8s: invalid material name\n", name.c_str());
		return false;
	}

	Piece_Config ps(name);
	const std::filesystem::path wdl_path = table_path(opt.wdl_dir, ps, WDL_EXT);
	const std::filesystem::path dtz_path = table_path(opt.dtz_dir, ps, DTZ_EXT);

	std::printf("==== %s ====\n", ps.name().c_str());
	WDL_File wdl_file;
	DTZ_File dtz_file;
	if (!require_full_file(wdl_file, ps, wdl_path, "WDL")) return false;
	if (opt.check_dtz && !require_full_file(dtz_file, ps, dtz_path, "DTZ")) return false;
	if (opt.check_dtm && !require_full_dtm(opt, ps, "")) return false;
	if (!require_full_sub_tables(opt, ps)) return false;

	std::array<size_t, COLOR_NB> dtz_entry_bytes{};
	if (opt.check_dtz)
		for (Color c : { WHITE, BLACK })
			dtz_entry_bytes[c] = dtz_file.per_color[c].entry_bytes;

	Position_Index_Config epsi(ps);
	// Native (identity-permutation) index — the inverse of position_from_index.
	const Index_Storage_Layout native = make_index_storage_layout(epsi, 0);
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = mat_key == mir_key;
	const size_t N = epsi.num_positions();

	constexpr size_t CHUNK_SIZE = CACHE_LINE_SIZE * CHAR_BIT * 64;
	std::atomic<size_t> next_idx{0};
	Concurrent_Progress_Bar progress_bar(
		N, std::max<size_t>(1, g_num_threads * CHUNK_SIZE), ps.name());

	Probe_Tables dtz_tables;
	dtz_tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
	dtz_tables.add_wdl_path(opt.wdl_dir);
	dtz_tables.add_dtz_path(opt.dtz_dir);

	Probe_Tables dtm_tables;
	dtm_tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
	dtm_tables.add_wdl_path(opt.wdl_dir);
	dtm_tables.add_dtm_path(opt.dtm_dir);
	dtm_tables.add_dtm50_path(opt.dtm50_dir);

	auto shards = global_pool().run_sync_task_on_all_threads([&](size_t) -> Shard {
		Shard s;
		while (true)
		{
			const size_t lo = next_idx.fetch_add(CHUNK_SIZE, std::memory_order_relaxed);
			if (lo >= N) break;
			const size_t hi = std::min(lo + CHUNK_SIZE, N);
			for (size_t i = lo; i < hi; ++i)
			{
				for (Color stm : { WHITE, BLACK })
				{
					if (symmetric && stm == BLACK) break;
					Position pos;
					const auto idx = static_cast<Board_Index>(i);
					if (!position_from_index(epsi, idx, stm, out_param(pos)))
						continue;
					if (!pos.is_legal())
						continue;
					if (board_index_of_position(epsi, native, pos) != idx) continue;

					if (opt.cursed_only)
					{
						const WDL_Entry w = dtz_tables.probe_wdl(ps, pos, SQ_END, 0);
						if (w != WDL_Entry::CURSED_WIN && w != WDL_Entry::BLESSED_LOSS)
							continue;
					}

					bool counted = false;

					if (opt.check_dtm)
					{
						const Probe_Result pr = dtm_tables.probe(ps, pos, IGNORE_50MR);
						if (pr.status == Probe_Result::Status::OK)
						{
							if (!counted) { ++s.scanned; counted = true; }
							const DTM_Derived d = derive_dtm_from_children(dtm_tables, pos);
							if (d.missing_child || !pr.has_dtm)
							{
								++s.missing;
								push_sample(s, opt.sample_cap, "[DTM_MISSING] " + ps.name() + " fen=" + fen_of(pos));
							}
							else
							{
								++s.checked_dtm;
								const WDL_Entry actual_w = fold_dtm_wdl(pr.wdl);
								const uint16_t actual_v = static_cast<uint16_t>(pr.dtm);
								const bool value_mismatch = d.wdl != WDL_Entry::DRAW && actual_v != d.value;
								if (actual_w != d.wdl || value_mismatch)
								{
									++s.dtm_mismatch;
									char line[512];
									std::snprintf(line, sizeof(line),
										"[DTM] idx=%zu stm=%s table=%s/%u derived=%s/%u fen=%s",
										i, stm == WHITE ? "W" : "B",
										wdl_name(actual_w), static_cast<unsigned>(actual_v),
										wdl_name(d.wdl), static_cast<unsigned>(d.value),
										fen_of(pos).c_str());
									push_sample(s, opt.sample_cap, line);
								}
							}
						}
					}

					if (opt.check_dtz)
					{
						const Probe_Result pr = dtz_tables.probe(ps, pos, IGNORE_50MR);
						if (pr.status == Probe_Result::Status::OK)
						{
							if (!counted) { ++s.scanned; counted = true; }
							const DTZ_Derived d = derive_dtz_from_children(dtz_tables, pos);
							if (d.missing_child || !pr.has_dtz)
							{
								++s.missing;
								push_sample(s, opt.sample_cap, "[DTZ_MISSING] " + ps.name() + " fen=" + fen_of(pos));
							}
							else
							{
								++s.checked_dtz;
								const uint16_t actual_v = static_cast<uint16_t>(pr.dtz);
								const bool value_mismatch =
									!dtz_values_match(d.wdl, actual_v, d.value, dtz_entry_bytes[stm]);
								if (pr.wdl != d.wdl || value_mismatch)
								{
									++s.dtz_mismatch;
									char line[512];
									std::snprintf(line, sizeof(line),
										"[DTZ] idx=%zu stm=%s table=%s/%u derived=%s/%u fen=%s",
										i, stm == WHITE ? "W" : "B",
										wdl_name(pr.wdl), static_cast<unsigned>(actual_v),
										wdl_name(d.wdl), static_cast<unsigned>(d.value),
										fen_of(pos).c_str());
									push_sample(s, opt.sample_cap, line);
								}
							}
						}
					}
				}
			}
			progress_bar += hi - lo;
		}
		return s;
	});
	progress_bar.set_finished();

	Shard total;
	for (const auto& sh : shards)
	{
		total.scanned += sh.scanned;
		total.checked_dtz += sh.checked_dtz;
		total.checked_dtm += sh.checked_dtm;
		total.dtz_mismatch += sh.dtz_mismatch;
		total.dtm_mismatch += sh.dtm_mismatch;
		total.missing += sh.missing;
		for (const auto& sample : sh.samples)
			if (total.samples.size() < opt.sample_cap) total.samples.push_back(sample);
	}

	std::printf("  scanned=%zu  checked_dtz=%zu  checked_dtm=%zu\n",
		total.scanned, total.checked_dtz, total.checked_dtm);
	const size_t violations = total.dtz_mismatch + total.dtm_mismatch + total.missing;
	if (violations == 0)
	{
		std::printf("  OK: all requested 1-ply fixed-point checks hold\n");
		return true;
	}

	std::printf("  VIOLATIONS: dtz=%zu dtm=%zu missing=%zu\n",
		total.dtz_mismatch, total.dtm_mismatch, total.missing);
	for (const auto& sample : total.samples)
		std::printf("    SAMPLE: %s\n", sample.c_str());
	return false;
}

std::vector<std::string> read_list_file(const std::string& path)
{
	std::ifstream f(path);
	if (!f) { std::fprintf(stderr, "Cannot open list file %s\n", path.c_str()); std::exit(1); }
	std::vector<std::string> out;
	for (std::string line; std::getline(f, line);)
	{
		auto h = line.find_first_of(";#");
		if (h != std::string::npos) line.resize(h);
		while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
		size_t i = 0; while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
		if (i < line.size()) out.push_back(line.substr(i));
	}
	return out;
}

// Split a `-r` argument on commas, matching the generator's own -r parsing.
std::vector<std::string> split_materials(const char* csv)
{
	std::vector<std::string> out;
	for (const char* p = csv; *p;)
	{
		const char* e = p;
		while (*e && *e != ',') ++e;
		if (e != p) out.emplace_back(p, e);
		p = *e ? e + 1 : e;
	}
	return out;
}

std::vector<std::string> enumerate_materials(size_t max_pieces)
{
	const Piece_Type types[] = { QUEEN, ROOK, BISHOP, KNIGHT, PAWN };
	Unique_Piece_Configs seen;

	std::function<void(size_t, size_t, std::vector<Piece_Type>&,
	                   const std::function<void(const std::vector<Piece_Type>&)>&)> ems;
	ems = [&](size_t start, size_t left, std::vector<Piece_Type>& cur,
	          const std::function<void(const std::vector<Piece_Type>&)>& cb) {
		if (left == 0) { cb(cur); return; }
		for (size_t i = start; i < 5; ++i) {
			cur.push_back(types[i]);
			ems(i, left - 1, cur, cb);
			cur.pop_back();
		}
	};

	for (size_t total = 3; total <= max_pieces; ++total) {
		const size_t nk = total - 2;
		for (size_t w = 0; w <= nk; ++w) {
			const size_t b = nk - w;
			std::vector<Piece_Type> wp;
			ems(0, w, wp, [&](const std::vector<Piece_Type>& wpv) {
				std::vector<Piece_Type> bp;
				ems(0, b, bp, [&](const std::vector<Piece_Type>& bpv) {
					std::vector<Piece> pcs;
					pcs.reserve(total);
					pcs.push_back(piece_make(WHITE, KING));
					for (auto t : wpv) pcs.push_back(piece_make(WHITE, t));
					pcs.push_back(piece_make(BLACK, KING));
					for (auto t : bpv) pcs.push_back(piece_make(BLACK, t));
					if (!Piece_Config::is_constructible_from(Const_Span<Piece>(pcs.data(), pcs.size())))
						return;
					seen.add_unique(Piece_Config(Const_Span<Piece>(pcs.data(), pcs.size())));
				});
			});
		}
	}

	Unique_Piece_Configs ordered;
	for (const Piece_Config& ps : seen)
		ordered.add_closure_in_dependency_order(ps, true);
	ordered.remove_if([](const Piece_Config& ps) { return ps.is_bare_kings(); });

	std::vector<std::string> out;
	out.reserve(ordered.size());
	for (const Piece_Config& ps : ordered) out.push_back(ps.name());
	return out;
}

}  // namespace

int main(int argc, char** argv)
{
	try {
		Options opt;
		std::vector<std::string> mats;
		bool explicit_mode = false;

		for (int i = 1; i < argc; ++i)
		{
			std::string a = argv[i];
			if (a == "-r" && i + 1 < argc)      {
				auto more = split_materials(argv[++i]);
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "-t" && i + 1 < argc)      { g_num_threads = std::max<size_t>(1, std::strtoull(argv[++i], nullptr, 10)); continue; }
			if (a == "--wdl" && i + 1 < argc)   { opt.wdl_dir = argv[++i]; continue; }
			if (a == "--dtz" && i + 1 < argc)   { opt.dtz_dir = argv[++i]; continue; }
			if (a == "--dtm" && i + 1 < argc)   { opt.dtm_dir = argv[++i]; continue; }
			if (a == "--dtm50" && i + 1 < argc) { opt.dtm50_dir = argv[++i]; continue; }
			if (a == "--check-dtz")             { if (!explicit_mode) { opt.check_dtz = opt.check_dtm = false; explicit_mode = true; } opt.check_dtz = true; continue; }
			if (a == "--check-dtm")             { if (!explicit_mode) { opt.check_dtz = opt.check_dtm = false; explicit_mode = true; } opt.check_dtm = true; continue; }
			if (a == "--cursed-only")           { opt.cursed_only = true; continue; }
			if (a == "--limit" && i + 1 < argc) { opt.sample_cap = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (a == "--cache" && i + 1 < argc) { opt.cache_mib = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (a == "--list" && i + 1 < argc)  {
				auto more = read_list_file(argv[++i]);
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "--enumerate" && i + 1 < argc) {
				auto more = enumerate_materials(std::strtoull(argv[++i], nullptr, 10));
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "-h" || a == "--help") {
				std::cout << "Usage: " << argv[0] << " [options]\n"
					"Options:\n"
					"  -r LIST           comma-separated material names\n"
					"  -t N              worker threads (default: hardware_concurrency)\n"
					"  --check-dtz       check DTZ only (combinable with --check-dtm)\n"
					"  --check-dtm       check DTM only (combinable with --check-dtz)\n"
					"  --list FILE       newline-separated material names\n"
					"  --enumerate N     check every material with <= N pieces\n"
					"  --wdl DIR         WDL directory (default ./wdl/)\n"
					"  --dtz DIR         DTZ directory (default ./dtz/)\n"
					"  --dtm DIR         DTM directory (default ./dtm/)\n"
					"  --dtm50 DIR       DTM50 directory (default ./dtm50/)\n"
					"  --cursed-only     DTZ, and only cells stored cursed\n"
					"  --limit N         max sample FENs per material (default 20)\n"
					"  --cache MiB       decoded-block cache budget per table set (default 64)\n";
				return 0;
			}
			std::cerr << "unknown arg: " << a << "\n";
			return 1;
		}

		if (opt.cursed_only) { opt.check_dtz = true; opt.check_dtm = false; }

		if (mats.empty())
		{
			std::cerr << "No materials given. Use -r LIST, --list FILE, or --enumerate N.\n";
			return 1;
		}

		for (const auto& mat : mats)
			if (!check_material(opt, mat))
				return 1;
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
