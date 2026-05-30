// Check disk-table invariants for every legal canonical position.
//
//   DTC:   wins >0, losses 0 only at mate, cursed iff dtc>100. (DRAW cells
//          skipped: WDL companion is class-authoritative; see egtb_entry.h.)
//   DTM:   wins >0, losses 0 only at mate, WIN odd, LOSE even.
//   DTM50: layer-0 class == fold_dtm50_wdl(wdl); W/L follow DTM nonzero+parity.
//
//   ./check_tables KRRK [KQQK ...]
//   ./check_tables --list FILE
//   ./check_tables --enumerate N
//   ./check_tables --limit 50 KRRK
//   ./check_tables --wdl ./wdl --dtc ./dtc --dtm ./dtm --dtm50 ./dtm50 KRRK

#include "probe/probe.h"
#include "probe/position_index.h"
#include "chess/attack.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"
#include "util/progress_bar.h"
#include "util/thread_pool.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
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
bool is_cursed_class(WDL_Entry w)
{
	return w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS;
}

// 5-class → DTM50 layer-0 3-class: cursed/blessed → DRAW.
WDL_Entry fold_dtm50_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN || w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::DRAW;
	return w;
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
	copy.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
		if (copy.is_pseudo_legal_move_legal(ml[i])) return false;
	return true;
}

struct Shard
{
	size_t scanned = 0;
	size_t classified = 0;
	uint64_t per_wdl[8] = {};

	size_t v_missing_dtc = 0;
	size_t v_dtc_win_zero = 0;
	size_t v_dtc_lose_zero_non_mate = 0;
	size_t v_dtc_cursed_range = 0;
	size_t v_dtc_noncursed_range = 0;

	size_t v_missing_dtm = 0;
	size_t v_dtm_win_zero = 0;
	size_t v_dtm_lose_zero_non_mate = 0;
	size_t v_dtm_parity_win = 0;
	size_t v_dtm_parity_lose = 0;
	// DTM50 layer-0 (vs fold_dtm50_wdl).
	size_t v_missing_dtm50 = 0;
	size_t v_dtm50_class_mismatch = 0;
	size_t v_dtm50_win_zero = 0;
	size_t v_dtm50_lose_zero_non_mate = 0;
	size_t v_dtm50_parity_win = 0;
	size_t v_dtm50_parity_lose = 0;
	std::vector<std::string> samples;
};

struct Options
{
	std::string wdl_dir = "./wdl/";
	std::string dtc_dir = "./dtc/";
	std::string dtm_dir = "./dtm/";
	std::string dtm50_dir = "./dtm50/";
	size_t sample_cap = 20;
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

bool check_material(const Options& opt, const std::string& name)
{
	if (!Piece_Config::is_constructible_from(name)) {
		std::printf("%-8s: invalid material name\n", name.c_str()); return false;
	}
	Piece_Config ps(name);

	const auto dtc_path = std::filesystem::path(opt.dtc_dir) / (ps.name() + ".lzdtc");
	const auto dtm_path = std::filesystem::path(opt.dtm_dir) / (ps.name() + ".lzdtm");
	const auto dtm50_path = std::filesystem::path(opt.dtm50_dir) / (ps.name() + ".lzdtm50");
	const bool have_dtc = std::filesystem::exists(dtc_path);
	const bool have_dtm = std::filesystem::exists(dtm_path);
	const bool have_dtm50 = std::filesystem::exists(dtm50_path);
	if (!have_dtc && !have_dtm && !have_dtm50) {
		std::printf("%-8s: no .lzdtc/.lzdtm/.lzdtm50 on disk, skipped\n", ps.name().c_str()); return true;
	}

	Position_Index_Config epsi(ps);
	// Native (identity-permutation) index — the inverse of position_from_index.
	const Index_Storage_Layout native = make_index_storage_layout(epsi, 0);
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = (mat_key == mir_key);
	const size_t N = epsi.num_positions();

	// Share table caches across workers.
	Probe_Tables tables;
	tables.add_wdl_path(opt.wdl_dir);
	tables.add_dtc_path(opt.dtc_dir);
	tables.add_dtm_path(opt.dtm_dir);
	tables.add_dtm50_path(opt.dtm50_dir);

	constexpr size_t CHUNK_SIZE = 64 * 64 * 8;
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

				const Probe_Result pr = tables.probe(ps, pos, /*rule50=*/0);
				if (pr.status != Probe_Result::Status::OK) continue;
				++s.scanned;
				const WDL_Entry w = pr.wdl;
				s.per_wdl[static_cast<size_t>(w)] += 1;

				// DRAW/ILLEGAL stored bits are don't-care (see egtb_entry.h).
				if (w == WDL_Entry::ILLEGAL || w == WDL_Entry::DRAW) continue;
				++s.classified;

				if (have_dtc)
				{
					if (!pr.has_dtc) { ++s.v_missing_dtc; }
					else
					{
						const uint16_t dtc_v = static_cast<uint16_t>(pr.dtc);
						if (is_win_class(w) && dtc_v == 0) {
							++s.v_dtc_win_zero;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtc_v, "DTC_WIN_ZERO");
						}
						if (is_lose_class(w) && dtc_v == 0 && !position_is_checkmate(pos)) {
							++s.v_dtc_lose_zero_non_mate;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtc_v, "DTC_LOSE_ZERO_NOMATE");
						}
						if (is_cursed_class(w) && dtc_v <= DTC_MAX_NON_CURSED_DTZ) {
							++s.v_dtc_cursed_range;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtc_v, "DTC_CURSED_RANGE");
						}
						if ((w == WDL_Entry::WIN || w == WDL_Entry::LOSE)
						    && dtc_v > DTC_MAX_NON_CURSED_DTZ) {
							++s.v_dtc_noncursed_range;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w, dtc_v, "DTC_NONCURSED_RANGE");
						}
					}
				}

				if (have_dtm)
				{
					if (!pr.has_dtm) { ++s.v_missing_dtm; continue; }
					const uint16_t dtm_v = static_cast<uint16_t>(pr.dtm);

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
				}

				if (have_dtm50)
				{
					const WDL_Entry expect = fold_dtm50_wdl(w);
					if (!pr.has_dtm50) {
						++s.v_missing_dtm50;
					}
					else
					{
						if (pr.dtm50_wdl != expect) {
							++s.v_dtm50_class_mismatch;
							push_sample(s, opt.sample_cap, ps, i, stm, pos, w,
								static_cast<uint16_t>(pr.dtm50), "DTM50_CLASS_MISMATCH");
						}
						if (expect == WDL_Entry::WIN || expect == WDL_Entry::LOSE)
						{
							const uint16_t v50 = static_cast<uint16_t>(pr.dtm50);
							if (expect == WDL_Entry::WIN && v50 == 0) {
								++s.v_dtm50_win_zero;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, v50, "DTM50_WIN_ZERO");
							}
							if (expect == WDL_Entry::LOSE && v50 == 0 && !position_is_checkmate(pos)) {
								++s.v_dtm50_lose_zero_non_mate;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, v50, "DTM50_LOSE_ZERO_NOMATE");
							}
							if (expect == WDL_Entry::WIN && (v50 % 2u) == 0u) {
								++s.v_dtm50_parity_win;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, v50, "DTM50_PARITY_WIN_EVEN");
							}
							if (expect == WDL_Entry::LOSE && (v50 % 2u) != 0u) {
								++s.v_dtm50_parity_lose;
								push_sample(s, opt.sample_cap, ps, i, stm, pos, w, v50, "DTM50_PARITY_LOSE_ODD");
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

	Shard t;
	for (const auto& sh : shards) {
		t.scanned              += sh.scanned;
		t.classified           += sh.classified;
		for (size_t i = 0; i < 8; ++i) t.per_wdl[i] += sh.per_wdl[i];
		t.v_missing_dtc              += sh.v_missing_dtc;
		t.v_dtc_win_zero             += sh.v_dtc_win_zero;
		t.v_dtc_lose_zero_non_mate   += sh.v_dtc_lose_zero_non_mate;
		t.v_dtc_cursed_range         += sh.v_dtc_cursed_range;
		t.v_dtc_noncursed_range      += sh.v_dtc_noncursed_range;
		t.v_missing_dtm              += sh.v_missing_dtm;
		t.v_dtm_win_zero             += sh.v_dtm_win_zero;
		t.v_dtm_lose_zero_non_mate   += sh.v_dtm_lose_zero_non_mate;
		t.v_dtm_parity_win           += sh.v_dtm_parity_win;
		t.v_dtm_parity_lose          += sh.v_dtm_parity_lose;
		t.v_missing_dtm50            += sh.v_missing_dtm50;
		t.v_dtm50_class_mismatch     += sh.v_dtm50_class_mismatch;
		t.v_dtm50_win_zero           += sh.v_dtm50_win_zero;
		t.v_dtm50_lose_zero_non_mate += sh.v_dtm50_lose_zero_non_mate;
		t.v_dtm50_parity_win         += sh.v_dtm50_parity_win;
		t.v_dtm50_parity_lose        += sh.v_dtm50_parity_lose;
		for (const auto& s : sh.samples)
			if (t.samples.size() < opt.sample_cap) t.samples.push_back(s);
	}

	const size_t violations =
		t.v_missing_dtc + t.v_dtc_win_zero
		+ t.v_dtc_lose_zero_non_mate + t.v_dtc_cursed_range
		+ t.v_dtc_noncursed_range + t.v_missing_dtm + t.v_dtm_win_zero
		+ t.v_dtm_lose_zero_non_mate + t.v_dtm_parity_win
		+ t.v_dtm_parity_lose
		+ t.v_missing_dtm50 + t.v_dtm50_class_mismatch + t.v_dtm50_win_zero
		+ t.v_dtm50_lose_zero_non_mate + t.v_dtm50_parity_win
		+ t.v_dtm50_parity_lose;

	std::printf("==== %s ====\n", ps.name().c_str());
	std::printf("  tables:%s%s%s\n",
		have_dtc ? " DTC" : "", have_dtm ? " DTM" : "", have_dtm50 ? " DTM50" : "");
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
	if (t.v_missing_dtc)
		std::printf("    %zu  legal cells with no DTC entry (file missing or dropped color)\n",
			t.v_missing_dtc);
	if (t.v_dtc_win_zero)
		std::printf("    %zu  WIN cells with dtc=0\n", t.v_dtc_win_zero);
	if (t.v_dtc_lose_zero_non_mate)
		std::printf("    %zu  LOSE cells with dtc=0 but not checkmate\n",
			t.v_dtc_lose_zero_non_mate);
	if (t.v_dtc_cursed_range)
		std::printf("    %zu  CURSED/BLESSED cells with dtc<=100\n",
			t.v_dtc_cursed_range);
	if (t.v_dtc_noncursed_range)
		std::printf("    %zu  WIN/LOSE cells with dtc>100\n",
			t.v_dtc_noncursed_range);
	if (t.v_missing_dtm)
		std::printf("    %zu  W/L cells with no DTM entry (file missing or dropped color)\n",
			t.v_missing_dtm);
	if (t.v_dtm_win_zero)
		std::printf("    %zu  WIN cells with dtm=0\n", t.v_dtm_win_zero);
	if (t.v_dtm_lose_zero_non_mate)
		std::printf("    %zu  LOSE cells with dtm=0 but not checkmate\n",
			t.v_dtm_lose_zero_non_mate);
	if (t.v_dtm_parity_win)
		std::printf("    %zu  WIN cells with even dtm\n", t.v_dtm_parity_win);
	if (t.v_dtm_parity_lose)
		std::printf("    %zu  LOSE cells with odd dtm\n", t.v_dtm_parity_lose);
	if (t.v_missing_dtm50)
		std::printf("    %zu  W/L cells with no DTM50 entry (file missing or dropped color)\n",
			t.v_missing_dtm50);
	if (t.v_dtm50_class_mismatch)
		std::printf("    %zu  DTM50 class != fold_dtm50_wdl(wdl)\n",
			t.v_dtm50_class_mismatch);
	if (t.v_dtm50_win_zero)
		std::printf("    %zu  DTM50 WIN cells with value=0\n", t.v_dtm50_win_zero);
	if (t.v_dtm50_lose_zero_non_mate)
		std::printf("    %zu  DTM50 LOSE cells with value=0 but not checkmate\n",
			t.v_dtm50_lose_zero_non_mate);
	if (t.v_dtm50_parity_win)
		std::printf("    %zu  DTM50 WIN cells with even value\n", t.v_dtm50_parity_win);
	if (t.v_dtm50_parity_lose)
		std::printf("    %zu  DTM50 LOSE cells with odd value\n", t.v_dtm50_parity_lose);

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
					if (!Piece_Config::is_constructible_from(
					        Const_Span<Piece>(pcs.data(), pcs.size())))
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
			if (a == "--wdl" && i + 1 < argc)        { opt.wdl_dir = argv[++i]; continue; }
			if (a == "--dtc" && i + 1 < argc)        { opt.dtc_dir = argv[++i]; continue; }
			if (a == "--dtm" && i + 1 < argc)        { opt.dtm_dir = argv[++i]; continue; }
			if (a == "--dtm50" && i + 1 < argc)      { opt.dtm50_dir = argv[++i]; continue; }
			if (a == "--limit" && i + 1 < argc)      { opt.sample_cap = std::strtoull(argv[++i], nullptr, 10); continue; }
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
				std::printf(
					"Usage: %s [options] MATERIAL...\n"
					"Options:\n"
					"  -t N              worker threads (default: hardware_concurrency)\n"
					"  --list FILE       newline-separated material names\n"
					"  --enumerate N     check every material with <= N pieces\n"
					"  --wdl DIR         WDL directory (default ./wdl/)\n"
					"  --dtc DIR         DTC directory (default ./dtc/)\n"
					"  --dtm DIR         DTM directory (default ./dtm/)\n"
					"  --dtm50 DIR       DTM50 directory (default ./dtm50/)\n"
					"  --limit N         max sample FENs per material (default 20)\n",
					argv[0]);
				return 0;
			}
			mats.push_back(a);
		}
		if (mats.empty()) {
			std::fprintf(stderr, "No materials given. Use positional args, --list FILE, or --enumerate N.\n");
			return 1;
		}

		bool ok = true;
		for (const auto& n : mats) ok &= check_material(opt, n);
		return ok ? 0 : 1;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
