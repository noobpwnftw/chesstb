// Compare disk WDL against Syzygy for every legal canonical position.
//
//   ./run_compare KQK KRK ...
//   ./run_compare --list FILE
//   ./run_compare --enumerate N

#include "probe/probe.h"
#include "probe/position_index.h"
#include "chess/attack.h"
#include "chess/piece_config.h"
#include "util/progress_bar.h"
#include "util/thread_pool.h"

extern "C" {
#include "tbprobe.h"
}

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

static const char* wdl_name(WDL_Entry w)
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

static WDL_Entry fathom_to_wdl(unsigned r)
{
	switch (r) {
		case TB_LOSS:         return WDL_Entry::LOSE;
		case TB_BLESSED_LOSS: return WDL_Entry::BLESSED_LOSS;
		case TB_DRAW:         return WDL_Entry::DRAW;
		case TB_CURSED_WIN:   return WDL_Entry::CURSED_WIN;
		case TB_WIN:          return WDL_Entry::WIN;
	}
	return WDL_Entry::ILLEGAL;
}

static size_t g_num_threads = std::max(1u, std::thread::hardware_concurrency());

static Thread_Pool& global_pool()
{
	static Thread_Pool pool(g_num_threads);
	return pool;
}

// Fathom bitboards; generated positions have no castling, ep, or rule50 state.
struct Fathom_BBs { uint64_t white, black, kings, queens, rooks, bishops, knights, pawns; };

static Fathom_BBs fathom_bbs(const Position& pos)
{
	return {
		pos.color_bb(WHITE).bits(),
		pos.color_bb(BLACK).bits(),
		pos.piece_bb(WHITE_KING).bits()   | pos.piece_bb(BLACK_KING).bits(),
		pos.piece_bb(WHITE_QUEEN).bits()  | pos.piece_bb(BLACK_QUEEN).bits(),
		pos.piece_bb(WHITE_ROOK).bits()   | pos.piece_bb(BLACK_ROOK).bits(),
		pos.piece_bb(WHITE_BISHOP).bits() | pos.piece_bb(BLACK_BISHOP).bits(),
		pos.piece_bb(WHITE_KNIGHT).bits() | pos.piece_bb(BLACK_KNIGHT).bits(),
		pos.piece_bb(WHITE_PAWN).bits()   | pos.piece_bb(BLACK_PAWN).bits(),
	};
}

struct Shard
{
	size_t total_legal = 0, matches = 0, mismatches = 0;
	std::vector<std::string> examples;
};

struct Options
{
	std::string wdl_dir = "./wdl/";
	std::string syzygy_dir = "./syzygy";
};

static bool compare_material(const Options& opt, const char* name)
{
	if (!Piece_Config::is_constructible_from(std::string(name))) {
		std::printf("%-8s: invalid name\n", name); return false;
	}
	Piece_Config ps(std::string{name});

	// Accept both full and dropped-STM WDL files; generator lookup rejects the latter.
	if (!std::filesystem::exists(std::filesystem::path(opt.wdl_dir) / (ps.name() + ".lzw"))) {
		std::printf("%-8s: no .lzw on disk, skipped\n", name); return true;
	}
	if (ps.num_pieces() > TB_LARGEST) {
		std::printf("%-8s: %zu pieces > TB_LARGEST=%u, skipped\n",
			name, ps.num_pieces(), TB_LARGEST);
		return true;
	}

	Position_Index_Config epsi(ps);
	// Native (identity-permutation) index — the inverse of position_from_index.
	const Index_Storage_Layout native = make_index_storage_layout(epsi, 0);
	// Symmetric materials store one color; BLACK would derive the same answers.
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = (mat_key == mir_key);
	const size_t N = epsi.num_positions();
	// Share table caches across workers.
	Probe_Tables tables;
	tables.add_wdl_path(opt.wdl_dir);

	constexpr size_t CHUNK_SIZE = CACHE_LINE_SIZE * CHAR_BIT * 64;
	std::atomic<size_t> next_idx{0};
	Concurrent_Progress_Bar progress_bar(
		N, std::max<size_t>(1, g_num_threads * CHUNK_SIZE), std::string(name));

	auto shards = global_pool().run_sync_task_on_all_threads(
		[&](size_t) -> Shard {
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

						// Keep one index per canonical position.
						if (board_index_of_position(epsi, native, pos) != idx) continue;

						const WDL_Entry got = tables.probe_wdl(ps, pos, SQ_END, 0);
						if (got == WDL_Entry::ILLEGAL) continue;

						const Fathom_BBs bb = fathom_bbs(pos);
						const unsigned r = tb_probe_wdl(bb.white, bb.black, bb.kings, bb.queens,
														bb.rooks, bb.bishops, bb.knights, bb.pawns,
														0, 0, 0, stm == WHITE);
						if (r == TB_RESULT_FAILED) continue;

						++s.total_legal;
						const WDL_Entry want = fathom_to_wdl(r);
						if (want == got) { ++s.matches; continue; }

						++s.mismatches;
						if (s.examples.size() < 5) {
							char fen[128]; pos.to_fen(Span(fen, 128));
							s.examples.push_back(std::string(name) + " idx=" + std::to_string(i)
								+ " stm=" + (stm == WHITE ? "W" : "B") + " fen=" + fen
								+ " ours=" + wdl_name(got) + " syzygy=" + wdl_name(want));
						}
					}
				}
				progress_bar += hi - lo;
			}
			return s;
		});
	progress_bar.set_finished();

	size_t total = 0, matches = 0, mismatches = 0;
	std::vector<std::string> examples;
	for (auto& s : shards) {
		total += s.total_legal; matches += s.matches; mismatches += s.mismatches;
		for (auto& ex : s.examples) if (examples.size() < 5) examples.push_back(std::move(ex));
	}

	std::printf("%-8s: probed=%zu matches=%zu mismatches=%zu", name, total, matches, mismatches);
	std::printf("\n");
	for (const auto& ex : examples) std::printf("    DIFF: %s\n", ex.c_str());
	return mismatches == 0;
}

static std::vector<std::string> read_list_file(const std::string& path)
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

// Canonical materials up to max_pieces.
static std::vector<std::string> enumerate_materials(size_t max_pieces)
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
	ordered.remove_if([](const Piece_Config& ps) { return ps.num_pieces() <= 2; });

	std::vector<std::string> out;
	out.reserve(ordered.size());
	for (const Piece_Config& ps : ordered) out.push_back(ps.name());
	return out;
}

int main(int argc, char** argv)
{
	try {
		attack_init();

		Options opt;
		std::vector<std::string> materials;
		for (int i = 1; i < argc; ++i) {
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
			} else if (a == "--wdl" && i + 1 < argc) {
				opt.wdl_dir = argv[++i];
			} else if (a == "--syzygy" && i + 1 < argc) {
				opt.syzygy_dir = argv[++i];
			} else if (a == "--list" && i + 1 < argc) {
				auto more = read_list_file(argv[++i]);
				materials.insert(materials.end(), more.begin(), more.end());
			} else if (a == "--enumerate" && i + 1 < argc) {
				auto more = enumerate_materials(std::strtoull(argv[++i], nullptr, 10));
				materials.insert(materials.end(), more.begin(), more.end());
			} else if (a == "-h" || a == "--help") {
				std::printf(
					"Usage: %s [options] [MATERIAL...]\n"
					"Options:\n"
					"  -t N           worker threads (default: hardware_concurrency)\n"
					"  --wdl DIR      disk WDL directory (default ./wdl/)\n"
					"  --syzygy DIR   Syzygy .rtbw directory (default ./syzygy)\n"
					"  --list FILE    newline-separated material names\n"
					"  --enumerate N  all materials up to N pieces\n",
					argv[0]);
				return 0;
			} else {
				materials.push_back(a);
			}
		}

		if (!tb_init(opt.syzygy_dir.c_str())) {
			std::fprintf(stderr, "tb_init failed (need %s/ with .rtbw files)\n",
				opt.syzygy_dir.c_str());
			return 1;
		}
		// tb_init accepts an empty directory; fail early so the run is meaningful.
		if (TB_LARGEST == 0) {
			std::fprintf(stderr,
				"%s/ contains no usable tablebase files — every probe will "
				"fail. Place .rtbw files there before running.\n",
				opt.syzygy_dir.c_str());
			return 1;
		}

		if (materials.empty()) {
			std::fprintf(stderr, "No materials given. Use positional args, --list FILE, or --enumerate N.\n");
			return 1;
		}

		for (const auto& name : materials)
			if (!compare_material(opt, name.c_str()))
			{
				tb_free();
				return 1;
			}

		tb_free();
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
