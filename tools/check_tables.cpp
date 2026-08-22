// Check disk-table invariants for every legal canonical position.
//
//   DTZ:   wins >0, losses 0 only at mate, dtz>100 implies cursed. (DRAW cells
//          skipped: WDL companion is class-authoritative; see egtb_entry.h.)
//   DTM:   wins >0, losses 0 only at mate, WIN odd, LOSE even.
//   DTM50: layer-0 entry present for every W/L cell.
//   DTC:   entry present for every W/L cell, priced at a fresh clock (some
//          budget reaches every clean win, the band being 100 plies), owing no
//          more than the budgets built, and drawn for the cursed classes no
//          budget settles.
//   DTZ x DTM50, DTZ x DTC: each pack pins dtz -- DTM50 by its DRAW flip, DTC by
//          the unbounded row it embeds -- so a probe instance holding only the
//          pack must land on the value the DTZ table holds.
//
//   ./check_tables -r KRRK[,KQQK,...]
//   ./check_tables --list FILE
//   ./check_tables --enumerate N
//   ./check_tables --limit 50 -r KRRK
//   ./check_tables --checksum-only --enumerate 5   (just run file checksums, no scan)
//   ./check_tables --wdl ./wdl --dtz ./dtz --dtc ./dtc --dtm ./dtm --dtm50 ./dtm50 -r KRRK

#include "probe/probe.h"
#include "probe/position_index.h"
#include "probe/table_files.h"
#include "chess/attack.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"
#include "util/progress_bar.h"
#include "util/thread_pool.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <atomic>
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
bool is_lose_class(WDL_Entry w)
{
	return w == WDL_Entry::LOSE || w == WDL_Entry::BLESSED_LOSS;
}

size_t g_num_threads = std::max(1u, std::thread::hardware_concurrency());

Thread_Pool& global_pool()
{
	static Thread_Pool pool(g_num_threads);
	return pool;
}

bool position_is_checkmate(const Position& pos)
{
	if (!pos.is_in_check()) return false;
	Position copy = pos;
	Move_List ml;
	copy.gen_pseudo_legal_moves(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
		if (copy.is_pseudo_legal_move_legal(ml[i])) return false;
	return true;
}

struct Shard
{
	size_t scanned = 0;
	size_t classified = 0;
	uint64_t per_wdl[8] = {};

	bool seen_dtz = false;
	bool seen_dtc = false;
	bool seen_dtm = false;
	bool seen_dtm50 = false;

	size_t v_missing_dtz = 0;
	size_t v_dtz_win_zero = 0;
	size_t v_dtz_lose_zero_non_mate = 0;
	size_t v_dtz_noncursed_range = 0;
	size_t v_pack_dtz_missing = 0;
	size_t v_pack_dtz_mismatch = 0;

	size_t v_missing_dtc = 0;
	size_t v_dtc_cursed_priced = 0;
	size_t v_dtc_unpriced_clean = 0;
	size_t v_dtc_order_range = 0;
	size_t v_dtc_win_zero = 0;
	size_t v_dtc_lose_zero_non_mate = 0;
	size_t v_dtc_value_range = 0;
	size_t v_dtc_dtz_missing = 0;
	size_t v_dtc_dtz_mismatch = 0;

	size_t v_missing_dtm = 0;
	size_t v_dtm_win_zero = 0;
	size_t v_dtm_lose_zero_non_mate = 0;
	size_t v_dtm_parity_win = 0;
	size_t v_dtm_parity_lose = 0;

	size_t v_missing_dtm50 = 0;
	std::vector<std::string> samples;
};

struct Options
{
	std::string wdl_dir = "./wdl/";
	std::string dtz_dir = "./dtz/";
	std::string dtc_dir = "./dtc/";
	std::string dtm_dir = "./dtm/";
	std::string dtm50_dir = "./dtm50/";
	size_t sample_cap = 20;
	size_t cache_mib = DEFAULT_BLOCK_CACHE_BYTES / (1024 * 1024);
	bool checksum_only = false;
};

void push_sample(Shard& s, size_t cap,
                 const Piece_Config& ps, size_t idx, Color stm,
                 const Position& pos, WDL_Entry w, uint16_t value,
                 const char* tag)
{
	if (s.samples.size() >= cap) return;
	char fen[128] = {};
	pos.to_fen(Span(fen, 128));
	char line[256];
	std::snprintf(line, sizeof(line),
		"[%s] %s idx=%zu stm=%s wdl=%s value=%u fen=%s",
		tag, ps.name().c_str(), idx, stm == WHITE ? "W" : "B",
		wdl_name(w), static_cast<unsigned>(value), fen);
	s.samples.emplace_back(line);
}

template <typename File>
bool open_one(const Piece_Config& ps, const std::string& dir, const char* ext)
{
	const std::filesystem::path path = std::filesystem::path(dir) / (ps.name() + ext);
	if (!std::filesystem::exists(path)) return false;
	File f;
	f.load(ps, path, /*verify_checksum=*/true);
	return true;
}

void open_material(const Options& opt, const std::string& name)
{
	if (!Piece_Config::is_constructible_from(name))
		throw std::runtime_error("invalid material name " + name);
	Piece_Config ps(name);

	const bool wdl   = open_one<WDL_File>  (ps, opt.wdl_dir,   WDL_EXT);
	const bool dtz   = open_one<DTZ_File>  (ps, opt.dtz_dir,   DTZ_EXT);
	const bool dtc   = open_one<DTC_File>  (ps, opt.dtc_dir,   DTC_EXT);
	const bool dtm   = open_one<DTM_File>  (ps, opt.dtm_dir,   DTM_EXT);
	const bool dtm50 = open_one<DTM50_File>(ps, opt.dtm50_dir, DTM50_EXT);

	if (!wdl && !dtz && !dtc && !dtm && !dtm50) {
		std::printf("%-8s: no tables on disk, skipped\n", ps.name().c_str());
		return;
	}
	std::printf("%-8s: checksum OK ->%s%s%s%s%s\n", ps.name().c_str(),
		wdl ? " WDL" : "", dtz ? " DTZ" : "", dtc ? " DTC" : "",
		dtm ? " DTM" : "", dtm50 ? " DTM50" : "");
}

bool check_material(const Options& opt, const std::string& name)
{
	if (!Piece_Config::is_constructible_from(name)) {
		std::printf("%-8s: invalid material name\n", name.c_str()); return false;
	}
	Piece_Config ps(name);

	Position_Index_Config epsi(ps);
	// Native (identity-permutation) index — the inverse of position_from_index.
	const Index_Storage_Layout native = make_index_storage_layout(epsi, 0);
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = (mat_key == mir_key);
	const size_t N = epsi.num_positions();

	// One instance per file group, so each metric is checked against the file that
	// holds it. DTZ lands in both -- the table here, the pack's DRAW flip there --
	// which is what makes the cross-check a check. Caches are shared per instance.
	Probe_Tables dtz_tables;
	dtz_tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
	dtz_tables.add_wdl_path(opt.wdl_dir);
	dtz_tables.add_dtz_path(opt.dtz_dir);

	// DTC's own instance, denied the DTZ table so its dtz can only come out of
	// the row the pack embeds.
	Probe_Tables dtc_tables;
	dtc_tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
	dtc_tables.add_wdl_path(opt.wdl_dir);
	dtc_tables.add_dtc_path(opt.dtc_dir);

	Probe_Tables dtm_tables;
	dtm_tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
	dtm_tables.add_wdl_path(opt.wdl_dir);
	dtm_tables.add_dtm_path(opt.dtm_dir);
	dtm_tables.add_dtm50_path(opt.dtm50_dir);

	constexpr size_t CHUNK_SIZE = CACHE_LINE_SIZE * CHAR_BIT * 64;
	std::atomic<size_t> next_idx{0};
	Concurrent_Progress_Bar progress_bar(
		N, std::max<size_t>(1, g_num_threads * CHUNK_SIZE), ps.name());

	auto shards = global_pool().run_sync_task_on_all_threads(
		[&](size_t) -> Shard
	{
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

					// Same WDL table in both, so either one's class is the class.
					const Probe_Result cr = dtz_tables.probe(ps, pos, /*rule50=*/0);
					if (cr.status != Probe_Result::Status::OK) continue;
					const Probe_Result mr = dtm_tables.probe(ps, pos, /*rule50=*/0);
					++s.scanned;
					const WDL_Entry w = cr.wdl;
					s.per_wdl[static_cast<size_t>(w)] += 1;

					// DRAW/ILLEGAL stored bits are don't-care (see egtb_entry.h).
					if (w == WDL_Entry::ILLEGAL || w == WDL_Entry::DRAW) continue;
					++s.classified;

					s.seen_dtz   |= cr.has_dtz;
					s.seen_dtm   |= mr.has_dtm;
					s.seen_dtm50 |= mr.has_dtm50;

					if (!cr.has_dtz) { ++s.v_missing_dtz; }
					else
					{
						const uint16_t dtz_v = static_cast<uint16_t>(cr.dtz);
						if (is_win_class(w) && dtz_v == 0) {
							++s.v_dtz_win_zero;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtz_v, "DTZ_WIN_ZERO");
						}
						if (is_lose_class(w) && dtz_v == 0 && !position_is_checkmate(pos)) {
							++s.v_dtz_lose_zero_non_mate;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtz_v, "DTZ_LOSE_ZERO_NOMATE");
						}
						if ((w == WDL_Entry::WIN || w == WDL_Entry::LOSE)
							&& dtz_v > DTZ_MAX_NON_CURSED) {
							++s.v_dtz_noncursed_range;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtz_v, "DTZ_NONCURSED_RANGE");
						}
					}

					// Outside the cursed band the pack pins dtz for every cell it
					// answers at all, so a silent loss of the recovery is a
					// violation, not a quiet pass.
					if (mr.has_dtm50 && !mr.has_dtz
						&& (w == WDL_Entry::WIN || w == WDL_Entry::LOSE)) {
						++s.v_pack_dtz_missing;
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w, 0, "PACK_DTZ_MISSING");
					}

					// Whatever the pack pinned, by flip or by derive, has to be the
					// distance the table holds -- cursed band included, which the
					// derive can reach at the boundary.
					if (mr.has_dtz && cr.has_dtz
						&& static_cast<uint16_t>(mr.dtz) != static_cast<uint16_t>(cr.dtz)) {
						++s.v_pack_dtz_mismatch;
						char tag[64];
						std::snprintf(tag, sizeof(tag), "PACK_DTZ_MISMATCH pack=%u",
							static_cast<unsigned>(mr.dtz));
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w,
							static_cast<uint16_t>(cr.dtz), tag);
					}

					// DTC, off the pack alone. Its class is the one the clock
					// leaves, which at a fresh clock is the cursed fold: every
					// clean win has a budget that reaches it inside the band,
					// and no budget settles a cursed one.
					const Probe_Result pr = dtc_tables.probe(ps, pos, /*rule50=*/0);
					s.seen_dtc |= pr.has_dtc;
					if (pr.has_dtc)
					{
						const bool clean = (w == WDL_Entry::WIN || w == WDL_Entry::LOSE);
						const uint16_t order = static_cast<uint16_t>(pr.dtc_order);
						const uint16_t value = static_cast<uint16_t>(pr.dtc);
						if (!clean && pr.dtc_wdl != WDL_Entry::DRAW) {
							++s.v_dtc_cursed_priced;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, value, "DTC_CURSED_PRICED");
						}
						if (clean && pr.dtc_wdl != w) {
							++s.v_dtc_unpriced_clean;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, value, "DTC_UNPRICED_CLEAN");
						}
						if (clean && pr.dtc_wdl == w)
						{
							if (order > DTC_BUDGET_LAYERS) {
								++s.v_dtc_order_range;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, order, "DTC_ORDER_RANGE");
							}
							if (value > DTZ_MAX_NON_CURSED) {
								++s.v_dtc_value_range;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, value, "DTC_VALUE_RANGE");
							}
							if (w == WDL_Entry::WIN && value == 0) {
								++s.v_dtc_win_zero;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, value, "DTC_WIN_ZERO");
							}
							if (w == WDL_Entry::LOSE && value == 0 && !position_is_checkmate(pos)) {
								++s.v_dtc_lose_zero_non_mate;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, value, "DTC_LOSE_ZERO_NOMATE");
							}
						}
						// The pack carries the DTZ table as its unbounded row, so
						// every cell it answers has that number and it is the
						// table's -- the cursed band included, which is the row's
						// alone.
						if (!pr.has_dtz) {
							++s.v_dtc_dtz_missing;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, 0, "DTC_DTZ_MISSING");
						} else if (cr.has_dtz
							&& static_cast<uint16_t>(pr.dtz) != static_cast<uint16_t>(cr.dtz)) {
							++s.v_dtc_dtz_mismatch;
							char tag[64];
							std::snprintf(tag, sizeof(tag), "DTC_DTZ_MISMATCH pack=%u",
								static_cast<unsigned>(pr.dtz));
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w,
								static_cast<uint16_t>(cr.dtz), tag);
						}
					}
					else ++s.v_missing_dtc;  // gated on seen_dtc below

					if (!mr.has_dtm) { ++s.v_missing_dtm; continue; }
					const uint16_t dtm_v = static_cast<uint16_t>(mr.dtm);

					if (is_win_class(w) && dtm_v == 0) {
						++s.v_dtm_win_zero;
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtm_v, "DTM_WIN_ZERO");
					}

					if (is_lose_class(w) && dtm_v == 0 && !position_is_checkmate(pos)) {
						++s.v_dtm_lose_zero_non_mate;
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtm_v, "DTM_LOSE_ZERO_NOMATE");
					}

					if (is_win_class(w) && (dtm_v % 2u) == 0u) {
						++s.v_dtm_parity_win;
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtm_v, "DTM_PARITY_WIN_EVEN");
					}
					if (is_lose_class(w) && (dtm_v % 2u) != 0u) {
						++s.v_dtm_parity_lose;
						push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtm_v, "DTM_PARITY_LOSE_ODD");
					}

					if (!mr.has_dtm50) ++s.v_missing_dtm50;
				}
			}
			progress_bar += hi - lo;
		}
		return s;
	});
	progress_bar.set_finished();

	Shard t;
	for (const auto& sh : shards) {
		t.scanned              += sh.scanned;
		t.classified           += sh.classified;
		t.seen_dtz   |= sh.seen_dtz;
		t.seen_dtc   |= sh.seen_dtc;
		t.seen_dtm   |= sh.seen_dtm;
		t.seen_dtm50 |= sh.seen_dtm50;
		for (size_t i = 0; i < 8; ++i) t.per_wdl[i] += sh.per_wdl[i];
		t.v_missing_dtz              += sh.v_missing_dtz;
		t.v_dtz_win_zero             += sh.v_dtz_win_zero;
		t.v_dtz_lose_zero_non_mate   += sh.v_dtz_lose_zero_non_mate;
		t.v_dtz_noncursed_range      += sh.v_dtz_noncursed_range;
		t.v_pack_dtz_missing         += sh.v_pack_dtz_missing;
		t.v_pack_dtz_mismatch        += sh.v_pack_dtz_mismatch;
		t.v_missing_dtc              += sh.v_missing_dtc;
		t.v_dtc_cursed_priced        += sh.v_dtc_cursed_priced;
		t.v_dtc_unpriced_clean       += sh.v_dtc_unpriced_clean;
		t.v_dtc_order_range          += sh.v_dtc_order_range;
		t.v_dtc_win_zero             += sh.v_dtc_win_zero;
		t.v_dtc_lose_zero_non_mate   += sh.v_dtc_lose_zero_non_mate;
		t.v_dtc_value_range          += sh.v_dtc_value_range;
		t.v_dtc_dtz_missing          += sh.v_dtc_dtz_missing;
		t.v_dtc_dtz_mismatch         += sh.v_dtc_dtz_mismatch;
		t.v_missing_dtm              += sh.v_missing_dtm;
		t.v_dtm_win_zero             += sh.v_dtm_win_zero;
		t.v_dtm_lose_zero_non_mate   += sh.v_dtm_lose_zero_non_mate;
		t.v_dtm_parity_win           += sh.v_dtm_parity_win;
		t.v_dtm_parity_lose          += sh.v_dtm_parity_lose;
		t.v_missing_dtm50            += sh.v_missing_dtm50;
		for (const auto& s : sh.samples)
			if (t.samples.size() < opt.sample_cap) t.samples.push_back(s);
	}

	if (t.scanned == 0) {
		std::printf("%-8s: no probeable tables on disk, skipped\n", ps.name().c_str());
		return true;
	}

	// A table type that is entirely absent (never observed) is not validated:
	// !has_X then means "no such table", not a per-cell gap. Only count the
	// missing-entry violation for table types that were actually seen, where a
	// nonzero count is a genuine hole (present for some cells, absent for others).
	const size_t v_missing_dtz   = t.seen_dtz   ? t.v_missing_dtz   : 0;
	const size_t v_missing_dtc   = t.seen_dtc   ? t.v_missing_dtc   : 0;
	const size_t v_missing_dtm   = t.seen_dtm   ? t.v_missing_dtm   : 0;
	const size_t v_missing_dtm50 = t.seen_dtm50 ? t.v_missing_dtm50 : 0;

	const size_t violations =
		v_missing_dtz + t.v_dtz_win_zero
		+ t.v_dtz_lose_zero_non_mate
		+ t.v_dtz_noncursed_range
		+ t.v_pack_dtz_missing + t.v_pack_dtz_mismatch
		+ v_missing_dtc + t.v_dtc_cursed_priced + t.v_dtc_unpriced_clean
		+ t.v_dtc_order_range + t.v_dtc_win_zero + t.v_dtc_lose_zero_non_mate
		+ t.v_dtc_value_range + t.v_dtc_dtz_missing + t.v_dtc_dtz_mismatch
		+ v_missing_dtm + t.v_dtm_win_zero
		+ t.v_dtm_lose_zero_non_mate + t.v_dtm_parity_win
		+ t.v_dtm_parity_lose
		+ v_missing_dtm50;

	std::printf("==== %s ====\n", ps.name().c_str());
	std::printf("  tables:%s%s%s%s\n",
		t.seen_dtz ? " DTZ" : "", t.seen_dtc ? " DTC" : "",
		t.seen_dtm ? " DTM" : "", t.seen_dtm50 ? " DTM50" : "");
	std::printf("  scanned=%zu classified=%zu  (W=%llu CW=%llu D=%llu L=%llu BL=%llu)\n",
		t.scanned, t.classified,
		(unsigned long long)t.per_wdl[static_cast<size_t>(WDL_Entry::WIN)],
		(unsigned long long)t.per_wdl[static_cast<size_t>(WDL_Entry::CURSED_WIN)],
		(unsigned long long)t.per_wdl[static_cast<size_t>(WDL_Entry::DRAW)],
		(unsigned long long)t.per_wdl[static_cast<size_t>(WDL_Entry::LOSE)],
		(unsigned long long)t.per_wdl[static_cast<size_t>(WDL_Entry::BLESSED_LOSS)]);

	if (violations == 0) {
		std::printf("  OK: all invariants hold\n");
		return true;
	}
	std::printf("  VIOLATIONS:\n");
	if (v_missing_dtz)
		std::printf("    %zu  legal cells with no DTZ entry (file missing or dropped color)\n",
			v_missing_dtz);
	if (t.v_dtz_win_zero)
		std::printf("    %zu  WIN cells with dtz=0\n", t.v_dtz_win_zero);
	if (t.v_dtz_lose_zero_non_mate)
		std::printf("    %zu  LOSE cells with dtz=0 but not checkmate\n",
			t.v_dtz_lose_zero_non_mate);
	if (t.v_dtz_noncursed_range)
		std::printf("    %zu  WIN/LOSE cells with dtz>100\n",
			t.v_dtz_noncursed_range);
	if (t.v_pack_dtz_missing)
		std::printf("    %zu  strict W/L cells the DTM50 pack answered but left no dtz\n",
			t.v_pack_dtz_missing);
	if (t.v_pack_dtz_mismatch)
		std::printf("    %zu  W/L cells where the DTM50-recovered dtz differs from the DTZ table\n",
			t.v_pack_dtz_mismatch);
	if (v_missing_dtc)
		std::printf("    %zu  classified cells with no DTC entry (file missing or dropped color)\n",
			v_missing_dtc);
	if (t.v_dtc_cursed_priced)
		std::printf("    %zu  cursed/blessed cells the DTC pack priced\n",
			t.v_dtc_cursed_priced);
	if (t.v_dtc_unpriced_clean)
		std::printf("    %zu  clean W/L cells left unpriced at a fresh clock\n",
			t.v_dtc_unpriced_clean);
	if (t.v_dtc_order_range)
		std::printf("    %zu  DTC cells owing more pushes than the budgets built\n",
			t.v_dtc_order_range);
	if (t.v_dtc_win_zero)
		std::printf("    %zu  WIN cells with dtc=0\n", t.v_dtc_win_zero);
	if (t.v_dtc_lose_zero_non_mate)
		std::printf("    %zu  LOSE cells with dtc=0 but not checkmate\n",
			t.v_dtc_lose_zero_non_mate);
	if (t.v_dtc_value_range)
		std::printf("    %zu  DTC cells with a wait past the band\n",
			t.v_dtc_value_range);
	if (t.v_dtc_dtz_missing)
		std::printf("    %zu  cells the DTC pack answered but left no dtz\n",
			t.v_dtc_dtz_missing);
	if (t.v_dtc_dtz_mismatch)
		std::printf("    %zu  cells where the DTC-embedded dtz differs from the DTZ table\n",
			t.v_dtc_dtz_mismatch);
	if (v_missing_dtm)
		std::printf("    %zu  W/L cells with no DTM entry (file missing or dropped color)\n",
			v_missing_dtm);
	if (t.v_dtm_win_zero)
		std::printf("    %zu  WIN cells with dtm=0\n", t.v_dtm_win_zero);
	if (t.v_dtm_lose_zero_non_mate)
		std::printf("    %zu  LOSE cells with dtm=0 but not checkmate\n",
			t.v_dtm_lose_zero_non_mate);
	if (t.v_dtm_parity_win)
		std::printf("    %zu  WIN cells with even dtm\n", t.v_dtm_parity_win);
	if (t.v_dtm_parity_lose)
		std::printf("    %zu  LOSE cells with odd dtm\n", t.v_dtm_parity_lose);
	if (v_missing_dtm50)
		std::printf("    %zu  W/L cells with no DTM50 entry (file missing or dropped color)\n",
			v_missing_dtm50);

	for (const auto& s : t.samples)
		std::printf("    SAMPLE: %s\n", s.c_str());
	return false;
}

std::vector<std::string> read_list_file(const std::string& path)
{
	std::ifstream f(path);
	if (!f) { std::fprintf(stderr, "Cannot open list file %s\n", path.c_str()); std::exit(1); }
	std::vector<std::string> out;
	for (std::string line; std::getline(f, line);) {
		auto h = line.find_first_of(";#");
		if (h != std::string::npos) line.resize(h);
		while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
		size_t i = 0; while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
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
					if (!Piece_Config::is_constructible_from(
					        Const_Span<Piece>(pcs.data(), pcs.size())))
						return;
					seen.add_unique(Piece_Config(Const_Span<Piece>(pcs.data(), pcs.size())));
				});
			});
		}
	}

	Unique_Piece_Configs ordered;
	for (const Piece_Config& ps : seen)
		ps.add_closure_in_dependency_order_to(ordered, true);
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
		for (int i = 1; i < argc; ++i)
		{
			std::string a = argv[i];
			if (a == "-r" && i + 1 < argc)           {
				auto more = split_materials(argv[++i]);
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "-t" && i + 1 < argc)           { g_num_threads = std::max<size_t>(1, std::strtoull(argv[++i], nullptr, 10)); continue; }
			if (a == "--wdl" && i + 1 < argc)        { opt.wdl_dir = argv[++i]; continue; }
			if (a == "--dtz" && i + 1 < argc)        { opt.dtz_dir = argv[++i]; continue; }
			if (a == "--dtc" && i + 1 < argc)        { opt.dtc_dir = argv[++i]; continue; }
			if (a == "--dtm" && i + 1 < argc)        { opt.dtm_dir = argv[++i]; continue; }
			if (a == "--dtm50" && i + 1 < argc)      { opt.dtm50_dir = argv[++i]; continue; }
			if (a == "--limit" && i + 1 < argc)      { opt.sample_cap = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (a == "--cache" && i + 1 < argc)      { opt.cache_mib = std::strtoull(argv[++i], nullptr, 10); continue; }
			if (a == "--checksum-only")              { opt.checksum_only = true; continue; }
			if (a == "--list" && i + 1 < argc)       {
				auto more = read_list_file(argv[++i]);
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "--enumerate" && i + 1 < argc)  {
				auto more = enumerate_materials(std::strtoull(argv[++i], nullptr, 10));
				mats.insert(mats.end(), more.begin(), more.end());
				continue;
			}
			if (a == "-h" || a == "--help") {
				std::cout << "Usage: " << argv[0] << " [options]\n"
					"Options:\n"
					"  -r LIST           comma-separated material names\n"
					"  -t N              worker threads (default: hardware_concurrency)\n"
					"  --list FILE       newline-separated material names\n"
					"  --enumerate N     check every material with <= N pieces\n"
					"  --wdl DIR         WDL directory (default ./wdl/)\n"
					"  --dtz DIR         DTZ directory (default ./dtz/)\n"
					"  --dtc DIR         DTC directory (default ./dtc/)\n"
					"  --dtm DIR         DTM directory (default ./dtm/)\n"
					"  --dtm50 DIR       DTM50 directory (default ./dtm50/)\n"
					"  --limit N         max sample FENs per material (default 20)\n"
					"  --cache MiB       decoded-block cache budget per material (default 64)\n"
					"  --checksum-only   just open tables to run checksums; skip the scan\n";
				return 0;
			}
			std::cerr << "unknown arg: " << a << "\n";
			return 1;
		}
		if (mats.empty()) {
			std::cerr << "No materials given. Use -r LIST, --list FILE, or --enumerate N.\n";
			return 1;
		}

		for (const auto& n : mats) {
			if (opt.checksum_only)
				open_material(opt, n);
			else if (!check_material(opt, n))
				return 1;
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
