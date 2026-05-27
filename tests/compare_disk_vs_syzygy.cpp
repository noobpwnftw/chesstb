// Compare disk WDL against Syzygy for every legal canonical position.
//
//   ./run_compare KQK KRK ...
//   ./run_compare --list FILE
//   ./run_compare --enumerate N

#include "probe/probe.h"
#include "probe/position_index.h"
#include "chess/attack.h"
#include "chess/piece_config.h"
#include "util/thread_pool.h"

#include <filesystem>

#include <atomic>

extern "C" {
#include "tbprobe.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
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
	uint16_t longest_win_ply = 0;
	size_t longest_win_idx = 0;
	Color longest_win_stm = WHITE;
};

static bool compare_material(const char* name)
{
	if (!Piece_Config::is_constructible_from(std::string(name))) {
		std::printf("%-8s: invalid name\n", name); return false;
	}
	Piece_Config ps(std::string{name});

	// Accept both full and dropped-STM WDL files; generator lookup rejects the latter.
	if (!std::filesystem::exists(std::filesystem::path("./wdl") / (ps.name() + ".lzw"))) {
		std::printf("%-8s: no .lzw on disk, skipped\n", name); return true;
	}
	if (ps.num_pieces() > TB_LARGEST) {
		std::printf("%-8s: %zu pieces > TB_LARGEST=%u, skipped\n",
			name, ps.num_pieces(), TB_LARGEST);
		return true;
	}

	Position_Index_Config epsi(ps);
	// Symmetric materials store one color; BLACK would derive the same answers.
	const auto [mat_key, mir_key] = ps.material_keys();
	const bool symmetric = (mat_key == mir_key);
	const size_t N = epsi.num_positions();
	// Share table caches across workers.
	Probe_Tables tables;
	tables.add_wdl_path("./wdl/");
	tables.add_dtc_path("./dtc/");

	constexpr size_t CHUNK_SIZE = 64 * 64 * 8;
	std::atomic<size_t> next_idx{0};

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
						if (board_index_of_position(epsi, pos) != idx) continue;

						const Probe_Result pr = tables.probe(ps, pos);
						if (pr.status != Probe_Result::Status::OK) continue;
						const WDL_Entry got = pr.wdl;
						if (got == WDL_Entry::ILLEGAL) continue;

						const Fathom_BBs bb = fathom_bbs(pos);
						const unsigned r = tb_probe_wdl(bb.white, bb.black, bb.kings, bb.queens,
														bb.rooks, bb.bishops, bb.knights, bb.pawns,
														0, 0, 0, stm == WHITE);
						if (r == TB_RESULT_FAILED) continue;

						++s.total_legal;
						if (pr.has_dtc && (got == WDL_Entry::WIN || got == WDL_Entry::CURSED_WIN))
						{
							const uint16_t v = static_cast<uint16_t>(pr.dtc);
							if (v > s.longest_win_ply)
							{
								s.longest_win_ply = v;
								s.longest_win_idx = i;
								s.longest_win_stm = stm;
							}
						}

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
			}
			return s;
		});

	size_t total = 0, matches = 0, mismatches = 0;
	std::vector<std::string> examples;
	uint16_t longest_win_ply = 0;
	size_t longest_win_idx = 0;
	Color longest_win_stm = WHITE;
	for (auto& s : shards) {
		total += s.total_legal; matches += s.matches; mismatches += s.mismatches;
		for (auto& ex : s.examples) if (examples.size() < 5) examples.push_back(std::move(ex));
		if (s.longest_win_ply > longest_win_ply) {
			longest_win_ply = s.longest_win_ply;
			longest_win_idx = s.longest_win_idx;
			longest_win_stm = s.longest_win_stm;
		}
	}

	std::printf("%-8s: probed=%zu matches=%zu mismatches=%zu", name, total, matches, mismatches);

	// Fathom root DTZ is accurate to +/-1.
	bool ply_ok = true;
	if (longest_win_ply > 0)
	{
		Position pos;
		(void)position_from_index(
			epsi, static_cast<Board_Index>(longest_win_idx), longest_win_stm,
			out_param(pos));
		const Fathom_BBs bb = fathom_bbs(pos);
		const unsigned rr = tb_probe_root(bb.white, bb.black, bb.kings, bb.queens,
		                                  bb.rooks, bb.bishops, bb.knights, bb.pawns,
		                                  0, 0, 0, longest_win_stm == WHITE, nullptr);
		if (rr != TB_RESULT_FAILED && rr != TB_RESULT_CHECKMATE && rr != TB_RESULT_STALEMATE)
		{
			const int want = static_cast<int>(TB_GET_DTZ(rr));
			const int diff = std::abs(want - static_cast<int>(longest_win_ply));
			if (diff > 1) {
				ply_ok = false;
				char fen[128]; pos.to_fen(Span(fen, 128));
				std::printf(" PLY_MISMATCH idx=%zu ours=%u syzygy_dtz=%d fen=%s",
					longest_win_idx, (unsigned)longest_win_ply, want, fen);
			} else {
				std::printf(" longest_win_ply=%u (~%d ok)",
					(unsigned)longest_win_ply, want);
			}
		}
	}
	std::printf("\n");
	for (const auto& ex : examples) std::printf("    DIFF: %s\n", ex.c_str());
	return mismatches == 0 && ply_ok;
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
					Piece_Config ps(Const_Span<Piece>(pcs.data(), pcs.size()));
					seen.emplace(total, ps.name());
				});
			});
		}
	}
	std::vector<std::string> out;
	out.reserve(seen.size());
	for (const auto& [_, n] : seen) out.push_back(n);
	return out;
}

int main(int argc, char** argv)
{
	attack_init();
	if (!tb_init("./syzygy")) {
		std::fprintf(stderr, "tb_init failed (need ./syzygy/ with .rtbw files)\n");
		return 1;
	}
	// tb_init accepts an empty directory; fail early so the run is meaningful.
	if (TB_LARGEST == 0) {
		std::fprintf(stderr,
			"./syzygy/ contains no usable tablebase files — every probe will "
			"fail. Place .rtbw files in ./syzygy/ before running.\n");
		return 1;
	}

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
		} else if (a == "--list" && i + 1 < argc) {
			auto more = read_list_file(argv[++i]);
			materials.insert(materials.end(), more.begin(), more.end());
		} else if (a == "--enumerate" && i + 1 < argc) {
			auto more = enumerate_materials(std::strtoull(argv[++i], nullptr, 10));
			materials.insert(materials.end(), more.begin(), more.end());
		} else if (a == "-h" || a == "--help") {
			std::printf("Usage: %s [-t N] [MATERIAL...] [--list FILE] [--enumerate N]\n", argv[0]);
			return 0;
		} else {
			materials.push_back(a);
		}
	}
	if (materials.empty()) {
		std::fprintf(stderr, "No materials given. Use positional args, --list FILE, or --enumerate N.\n");
		return 1;
	}

	bool ok = true;
	for (const auto& name : materials) ok &= compare_material(name.c_str());

	tb_free();
	return ok ? 0 : 1;
}
