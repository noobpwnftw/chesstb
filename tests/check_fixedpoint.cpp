// Recompute each table value from legal children and compare it to disk.
// Uses full tables only; dropped-STM shipping files are rejected.
//
//   ./check_fixedpoint KRRK
//   ./check_fixedpoint --dtm KRRK
//   ./check_fixedpoint --dtc --list five.txt
//   ./check_fixedpoint --enumerate 5 --wdl ./wdl --dtc ./dtc --dtm ./dtm

#include "probe/probe.h"
#include "probe/position_index.h"

#include "chess/attack.h"
#include "chess/piece_config.h"
#include "chess/position.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/progress_bar.h"
#include "util/thread_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr unsigned SKIP_DTM50 = ~0u;

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

bool move_is_zeroing(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN
	    || pos.piece_at(m.to()) != PIECE_NONE;
}

bool is_pawn_double_push(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN
	    && std::abs(static_cast<int>(sq_rank(m.to())) - static_cast<int>(sq_rank(m.from()))) == 2;
}

struct Config_And_Literal_Key {
	Piece_Config cfg;
	Material_Key literal_key;
};

Config_And_Literal_Key piece_config_and_literal_key_from_position_local(const Position& pos)
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

struct Child_Pos {
	Position pos;
	Piece_Config ps;
	bool is_kk = false;
	bool is_zeroing = false;
};

Child_Pos make_child(const Position& parent, Move m)
{
	const bool zeroing = move_is_zeroing(parent, m);
	Position pos = parent;
	(void)pos.do_move(m);

	auto [cps, lit] = piece_config_and_literal_key_from_position_local(pos);
	if (lit != cps.base_material_key())
	{
		Position swapped;
		swapped.clear();
		for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
		{
			const Piece q = pos.piece_at(sq);
			if (q != PIECE_NONE)
				swapped.put_piece(piece_opp_color(q), sq_rank_mirror(sq));
		}
		swapped.set_turn(color_opp(pos.turn()));
		pos = swapped;
	}

	return { std::move(pos), std::move(cps), cps.num_pieces() <= 2, zeroing };
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
	if (c.is_kk) return {};
	const Probe_Result pr = tables.probe(c.ps, c.pos, SKIP_DTM50);
	if (pr.status != Probe_Result::Status::OK || !pr.has_dtm)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { fold_dtm_wdl(pr.wdl), static_cast<uint16_t>(pr.dtm), false };
}

Child_DTM effective_child_dtm_after_move(Probe_Tables& tables, const Position& parent, Move m)
{
	Child_Pos no_ep_child = make_child(parent, m);
	Child_DTM best = probe_child_dtm(tables, no_ep_child);
	if (!is_pawn_double_push(parent, m)) return best;

	Position pre_ep = parent;
	(void)pre_ep.do_move(m);
	const Color opp = pre_ep.turn();
	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(m.to());

	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(push_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square own_pawn_sq = sq_make(opp_ep_rank, static_cast<File>(f));
		if (pre_ep.piece_at(own_pawn_sq) != piece_make(opp, PAWN)) continue;
		const Square ep_to_sq = sq_make(ep_target_rank, push_file);
		if (pre_ep.piece_at(ep_to_sq) != PIECE_NONE) continue;
		const Move ep = Move::make_ep_capture(own_pawn_sq, ep_to_sq);
		if (!pre_ep.is_pseudo_legal_move_legal(ep)) continue;

		const Child_Pos ep_child = make_child(pre_ep, ep);
		const Child_DTM after_ep = probe_child_dtm(tables, ep_child);
		if (after_ep.missing)
		{
			best.missing = true;
			continue;
		}
		const Child_DTM opp_choice{
			invert_wdl(after_ep.wdl),
			static_cast<uint16_t>(after_ep.value + 1),
			false
		};
		if (best.missing || prefer_new(opp_choice.wdl, opp_choice.value, best.wdl, best.value))
			best = opp_choice;
	}
	return best;
}

DTM_Derived derive_dtm_from_children(Probe_Tables& tables, const Position& pos)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best = 0;
	bool missing = false;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const Child_DTM child = effective_child_dtm_after_move(tables, pos, m);
		if (child.missing)
		{
			missing = true;
			continue;
		}

		const WDL_Entry my_w = invert_wdl(child.wdl);
		const uint16_t my_v = static_cast<uint16_t>(child.value + 1);
		if (!have || prefer_new(my_w, my_v, best_wdl, best))
		{
			best_wdl = my_w;
			best = my_v;
			have = true;
		}
	}

	if (!any_legal)
		return { pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW, 0, missing };
	if (!have)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { best_wdl, best, missing };
}

struct DTC_Derived {
	WDL_Entry wdl = WDL_Entry::DRAW;
	uint16_t value = 0;
	bool missing_child = false;
};

struct Child_DTC {
	WDL_Entry wdl = WDL_Entry::DRAW;  // child side to move
	uint16_t value = 0;
	bool missing = false;
};

Child_DTC probe_child_dtc(Probe_Tables& tables, const Child_Pos& c)
{
	if (c.is_kk) return {};
	const Probe_Result pr = tables.probe(c.ps, c.pos, SKIP_DTM50);
	if (pr.status != Probe_Result::Status::OK || !pr.has_dtc)
		return { WDL_Entry::ILLEGAL, 0, true };
	return { pr.wdl, static_cast<uint16_t>(pr.dtc), false };
}

Child_DTC effective_child_dtc_after_move(Probe_Tables& tables, const Position& parent, Move m)
{
	Child_Pos no_ep_child = make_child(parent, m);
	Child_DTC best = probe_child_dtc(tables, no_ep_child);
	if (!is_pawn_double_push(parent, m)) return best;

	Position pre_ep = parent;
	(void)pre_ep.do_move(m);
	const Color opp = pre_ep.turn();
	const Rank opp_ep_rank    = (opp == WHITE) ? RANK_5 : RANK_4;
	const Rank ep_target_rank = (opp == WHITE) ? RANK_6 : RANK_3;
	const File push_file = sq_file(m.to());

	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(push_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square own_pawn_sq = sq_make(opp_ep_rank, static_cast<File>(f));
		if (pre_ep.piece_at(own_pawn_sq) != piece_make(opp, PAWN)) continue;
		const Square ep_to_sq = sq_make(ep_target_rank, push_file);
		if (pre_ep.piece_at(ep_to_sq) != PIECE_NONE) continue;
		const Move ep = Move::make_ep_capture(own_pawn_sq, ep_to_sq);
		if (!pre_ep.is_pseudo_legal_move_legal(ep)) continue;

		const Child_Pos ep_child = make_child(pre_ep, ep);
		const Child_DTC after_ep = probe_child_dtc(tables, ep_child);
		if (after_ep.missing)
		{
			best.missing = true;
			continue;
		}
		const Child_DTC opp_choice{ invert_wdl(after_ep.wdl), uint16_t{1}, false };
		if (best.missing || prefer_new(opp_choice.wdl, opp_choice.value, best.wdl, best.value))
			best = opp_choice;
	}
	return best;
}

DTC_Derived derive_dtc_from_children(Probe_Tables& tables, const Position& pos)
{
	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best = 0;
	bool missing = false;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		const Child_DTC child = effective_child_dtc_after_move(tables, pos, m);
		if (child.missing)
		{
			missing = true;
			continue;
		}

		WDL_Entry my_w = invert_wdl(child.wdl);
		// Match generator check_loss semantics for zeroing moves into cursed
		// children: they contribute 101, not 1.
		const bool cursed_child = child.wdl == WDL_Entry::CURSED_WIN
		                       || child.wdl == WDL_Entry::BLESSED_LOSS;
		const uint16_t my_v = move_is_zeroing(pos, m)
			? (cursed_child
				? static_cast<uint16_t>(DTC_MAX_NON_CURSED_DTZ + 1)
				: uint16_t{1})
			: static_cast<uint16_t>(child.value + 1);
		if (my_v > DTC_MAX_NON_CURSED_DTZ)
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
	}

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
	std::string dtc_dir = "./dtc/";
	std::string dtm_dir = "./dtm/";
	bool check_dtc = true;
	bool check_dtm = true;
	size_t sample_cap = 20;
};

struct Shard
{
	size_t scanned = 0;
	size_t checked_dtc = 0;
	size_t checked_dtm = 0;
	size_t dtc_mismatch = 0;
	size_t dtm_mismatch = 0;
	size_t missing = 0;
	std::vector<std::string> samples;
};

void push_sample(Shard& s, size_t cap, const std::string& line)
{
	if (s.samples.size() < cap) s.samples.push_back(line);
}

bool require_full_file(const std::filesystem::path& path, EGTB_Magic magic, const char* label)
{
	if (!std::filesystem::exists(path))
	{
		std::printf("  %s missing: %s\n", label, path.c_str());
		return false;
	}
	constexpr uint8_t SINGULAR_FLAG = 0x80;
	constexpr uint8_t DROPPED_FLAG  = 0x40;

	Memory_Mapped_File m;
	const bool full_format = [&]() {
		if (!m.open_readonly(path.c_str())) return false;
		const Const_Span<uint8_t> s = m.data_span();
		if (s.size() < 12) return false;

		Serial_Memory_Reader r(s);
		if (r.read<uint32_t>() != narrowing_static_cast<uint32_t>(magic)) return false;

		const uint32_t key_and_table_num = r.read<uint32_t>();
		const Fixed_Vector<Color, 2> colors = egtb_table_colors(key_and_table_num & 3);
		const bool egtb_table = magic == EGTB_Magic::DTC_MAGIC
		                     || magic == EGTB_Magic::DTM_MAGIC
		                     || magic == EGTB_Magic::DTM50_MAGIC;

		for (size_t i = 0; i < colors.size(); ++i)
		{
			if (static_cast<size_t>(s.end() - r.caret()) < 1) return false;
			const uint8_t flag = r.read<uint8_t>();
			if (flag & DROPPED_FLAG) return false;
			if (flag & SINGULAR_FLAG)
			{
				if (static_cast<size_t>(s.end() - r.caret()) < 1) return false;
				r.advance(1);
			}
			else if (egtb_table)
			{
				if (static_cast<size_t>(s.end() - r.caret()) < 31) return false;
				r.advance(29);
				const uint16_t num_ranks = r.read<uint16_t>();
				if (static_cast<size_t>(s.end() - r.caret()) < num_ranks * 2u) return false;
				r.advance(num_ranks * 2);
			}
			else
			{
				if (static_cast<size_t>(s.end() - r.caret()) < 26) return false;
				r.advance(26);
			}
		}
		return true;
	}();

	if (!full_format)
	{
		std::printf("  %s is not full format: %s\n", label, path.c_str());
		return false;
	}
	return true;
}

std::array<size_t, COLOR_NB> dtc_entry_bytes_by_color(const std::filesystem::path& path)
{
	constexpr uint8_t SINGULAR_FLAG = 0x80;
	constexpr uint8_t DROPPED_FLAG  = 0x40;

	std::array<size_t, COLOR_NB> out{};
	Memory_Mapped_File m;
	if (!m.open_readonly(path.c_str())) return out;

	Serial_Memory_Reader r(m.data_span());
	if (r.read<uint32_t>() != narrowing_static_cast<uint32_t>(EGTB_Magic::DTC_MAGIC))
		return out;

	const uint32_t key_and_table_num = r.read<uint32_t>();
	const Fixed_Vector<Color, 2> colors = egtb_table_colors(key_and_table_num & 3);
	for (Color c : colors)
	{
		const uint8_t flag = r.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			r.advance(1);
			continue;
		}
		if (flag & DROPPED_FLAG)
			continue;

		r.advance(4);  // index permutation config
		out[c] = r.read<uint8_t>();
		r.advance(4 + 4 + 8 + 8);
		const uint16_t num_ranks = r.read<uint16_t>();
		r.advance(static_cast<size_t>(num_ranks) * 2);
	}
	return out;
}

bool dtc_values_match(WDL_Entry w, uint16_t actual, uint16_t derived, size_t entry_bytes)
{
	if (w == WDL_Entry::DRAW) return true;
	if (entry_bytes == 1 && (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS))
	{
		// One-byte cursed encoding and check_loss cutoffs can shift the stored
		// value by one from this tool's direct 1-ply derivation.
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
	const std::filesystem::path wdl_path = std::filesystem::path(opt.wdl_dir) / (ps.name() + ".lzw");
	const std::filesystem::path dtc_path = std::filesystem::path(opt.dtc_dir) / (ps.name() + ".lzdtc");
	const std::filesystem::path dtm_path = std::filesystem::path(opt.dtm_dir) / (ps.name() + ".lzdtm");

	std::printf("==== %s ====\n", ps.name().c_str());
	if (!require_full_file(wdl_path, EGTB_Magic::WDL_MAGIC, "WDL")) return false;
	if (opt.check_dtc && !require_full_file(dtc_path, EGTB_Magic::DTC_MAGIC, "DTC")) return false;
	if (opt.check_dtm && !require_full_file(dtm_path, EGTB_Magic::DTM_MAGIC, "DTM")) return false;

	const std::array<size_t, COLOR_NB> dtc_entry_bytes = opt.check_dtc
		? dtc_entry_bytes_by_color(dtc_path)
		: std::array<size_t, COLOR_NB>{};

	Position_Index_Config epsi(ps);
	// Native (identity-permutation) index — the inverse of position_from_index.
	const Index_Storage_Layout native = make_index_storage_layout(epsi, 0);
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = mat_key == mir_key;
	const size_t N = epsi.num_positions();

	constexpr size_t CHUNK_SIZE = 64 * 64 * 8;
	std::atomic<size_t> next_idx{0};
	Concurrent_Progress_Bar progress_bar(
		N, std::max<size_t>(1, g_num_threads * CHUNK_SIZE), ps.name());

	auto shards = global_pool().run_sync_task_on_all_threads([&](size_t) -> Shard {
		Probe_Tables tables;
		tables.add_wdl_path(opt.wdl_dir);
		tables.add_dtc_path(opt.dtc_dir);
		tables.add_dtm_path(opt.dtm_dir);

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

					const Probe_Result pr = tables.probe(ps, pos, SKIP_DTM50);
					if (pr.status != Probe_Result::Status::OK) continue;
					++s.scanned;

					if (opt.check_dtm)
					{
						const DTM_Derived d = derive_dtm_from_children(tables, pos);
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

					if (opt.check_dtc)
					{
						const DTC_Derived d = derive_dtc_from_children(tables, pos);
						if (d.missing_child || !pr.has_dtc)
						{
							++s.missing;
							push_sample(s, opt.sample_cap, "[DTC_MISSING] " + ps.name() + " fen=" + fen_of(pos));
						}
						else
						{
							++s.checked_dtc;
							const uint16_t actual_v = static_cast<uint16_t>(pr.dtc);
							const bool value_mismatch =
								!dtc_values_match(d.wdl, actual_v, d.value, dtc_entry_bytes[stm]);
							if (pr.wdl != d.wdl || value_mismatch)
							{
								++s.dtc_mismatch;
								char line[512];
								std::snprintf(line, sizeof(line),
									"[DTC] idx=%zu stm=%s table=%s/%u derived=%s/%u fen=%s",
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
			progress_bar += hi - lo;
		}
		return s;
	});
	progress_bar.set_finished();

	Shard total;
	for (const auto& sh : shards)
	{
		total.scanned += sh.scanned;
		total.checked_dtc += sh.checked_dtc;
		total.checked_dtm += sh.checked_dtm;
		total.dtc_mismatch += sh.dtc_mismatch;
		total.dtm_mismatch += sh.dtm_mismatch;
		total.missing += sh.missing;
		for (const auto& sample : sh.samples)
			if (total.samples.size() < opt.sample_cap) total.samples.push_back(sample);
	}

	std::printf("  scanned=%zu  checked_dtc=%zu  checked_dtm=%zu\n",
		total.scanned, total.checked_dtc, total.checked_dtm);
	const size_t violations = total.dtc_mismatch + total.dtm_mismatch + total.missing;
	if (violations == 0)
	{
		std::printf("  OK: all requested 1-ply fixed-point checks hold\n");
		return true;
	}

	std::printf("  VIOLATIONS: dtc=%zu dtm=%zu missing=%zu\n",
		total.dtc_mismatch, total.dtm_mismatch, total.missing);
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

std::vector<std::string> enumerate_materials(size_t max_pieces)
{
	const Piece_Type types[] = { QUEEN, ROOK, BISHOP, KNIGHT, PAWN };
	std::set<std::pair<size_t, std::string>> seen;

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
					Piece_Config p(Const_Span<Piece>(pcs.data(), pcs.size()));
					seen.emplace(total, p.name());
				});
			});
		}
	}

	std::vector<std::string> out;
	out.reserve(seen.size());
	for (const auto& [_, n] : seen) out.push_back(n);
	return out;
}

}  // namespace

int main(int argc, char** argv)
{
	try {
		attack_init();

		Options opt;
		std::vector<std::string> mats;
		bool explicit_mode = false;

		for (int i = 1; i < argc; ++i)
		{
			std::string a = argv[i];
			if (a == "-t" && i + 1 < argc) {
				const char* v = argv[++i];
				char* end = nullptr;
				const long long n = std::strtoll(v, &end, 10);
				if (end == v || *end != '\0' || n <= 0) {
					std::fprintf(stderr, "-t needs a positive integer (got \"%s\")\n", v);
					return 1;
				}
				g_num_threads = static_cast<size_t>(n);
				continue;
			}
			if (a == "--wdl" && i + 1 < argc)   { opt.wdl_dir = argv[++i]; continue; }
			if (a == "--dtc-dir" && i + 1 < argc) { opt.dtc_dir = argv[++i]; continue; }
			if (a == "--dtm-dir" && i + 1 < argc) { opt.dtm_dir = argv[++i]; continue; }
			if (a == "--dtc" || a == "--dtz")   { if (!explicit_mode) { opt.check_dtc = opt.check_dtm = false; explicit_mode = true; } opt.check_dtc = true; continue; }
			if (a == "--dtm")                   { if (!explicit_mode) { opt.check_dtc = opt.check_dtm = false; explicit_mode = true; } opt.check_dtm = true; continue; }
			if (a == "--limit" && i + 1 < argc) { opt.sample_cap = std::strtoull(argv[++i], nullptr, 10); continue; }
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
				std::printf(
					"Usage: %s [options] MATERIAL...\n"
					"Options:\n"
					"  -t N             worker threads (default: hardware_concurrency)\n"
					"  --dtm            check DTM only (can be combined with --dtc)\n"
					"  --dtc, --dtz     check DTC only (can be combined with --dtm)\n"
					"  --list FILE      newline-separated material names\n"
					"  --enumerate N    check every material with <= N pieces\n"
					"  --wdl DIR        WDL directory (default ./wdl/)\n"
					"  --dtc-dir DIR    DTC directory (default ./dtc/)\n"
					"  --dtm-dir DIR    DTM directory (default ./dtm/)\n"
					"  --limit N        max sample FENs per material (default 20)\n",
					argv[0]);
				return 0;
			}
			mats.push_back(a);
		}

		if (mats.empty())
		{
			std::fprintf(stderr, "No materials given. Use positional args, --list FILE, or --enumerate N.\n");
			return 1;
		}

		bool ok = true;
		for (const auto& mat : mats)
			ok &= check_material(opt, mat);
		return ok ? 0 : 1;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
